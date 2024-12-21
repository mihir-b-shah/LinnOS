
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/namei.h>

#include "module.h"

#define MAX_N_MAPS 256
#define MAX_N_KEYS 256
#define MAX_N_COMBINERS 256
#define HASH_TABLE_SIZE 4096
#define HASH_TABLE_ASSOC 1
#define MAX_N_COMBINER_ARGS 16
#define MAX_N_MAPS_PER_COMBINER 16

typedef struct fstore_uuid_impl_t {
	const char* strs[2];
} fstore_uuid_t;

static bool is_uuid_empty(const fstore_uuid_t* uuid) {
	return uuid.strs[0] == NULL;
}

static bool uuid_eql(const fstore_uuid_t* uuid0, const fstore_uuid_t* uuid1) {
	for (int i = 0; i<sizeof(uuid0->strs)/sizeof(uuid0->strs[0]); ++i) {
		if (uuid0->strs[i] == NULL && uuid1->strs[i] == NULL) {
			return true;
		} else if (uuid0->strs[i] == NULL ^ uuid1->strs[i] == NULL) {
			return false;
		}
		if (strcmp(uuid0->strs[i], uuid1->strs[i]) != 0) {
			return false;
		}
	}
	return true;
}

struct key_scratch_info_t {
	const char* id;
	int scratch_offs;
	int scratch_sz;
};
static struct key_scratch_info_t key_infos[MAX_N_KEYS];

struct kv_t {
	int p;
	spinlock_t lock;
	bool valid[HASH_TABLE_ASSOC];
	fstore_key_type_t k[HASH_TABLE_ASSOC];
	fstore_val_type_t v[HASH_TABLE_ASSOC];
};
struct hash_map_t {
	int capacity;
	struct kv_t* table;
};
static u64 hash(fstore_key_type_t x) {
	return x;
}
static void hash_map__init(struct hash_map_t* map, int sz) {
	int i;
	map->capacity = sz;
	map->table = kzalloc(sizeof(struct kv_t) * sz, GFP_KERNEL);
	for (i = 0; i<sz; ++i) {
		map->table[i].p = 0;
		spin_lock_init(&map->table[i].lock);
	}
}
static bool hash_map__insert(struct hash_map_t* map, fstore_key_type_t k, fstore_val_type_t v) {
	int pos;
	struct kv_t* kv;

	kv = &map->table[hash(k)];
	spin_lock(&kv->lock);
	pos = kv->p++ % HASH_TABLE_ASSOC;
	kv->k[pos] = k;
	kv->v[pos] = v;
	spin_unlock(&kv->lock);
	return true;
}
static bool hash_map__lookup(struct hash_map_t* map, fstore_key_type_t k, fstore_val_type_t* v) {
	int i;
	struct kv_t* kv;
       
	kv = &map->table[hash(k)];
	spin_lock(&kv->lock);
	for (i = 0; i<HASH_TABLE_ASSOC; ++i) {
		if (kv->k[i] == k) {
			*v = kv->v[i];
			return true;
		}
	}
	spin_unlock(&kv->lock);
	return false;
}
static void hash_map__free(struct hash_map_t* map) {
	kfree(map->table);
}

struct circ_buf_entry_t {
	bool valid;
};
struct circ_buf_t {
	int sz;
	int el_size;
	int p;
	spinlock_t lock;
	struct circ_buf_entry_t* entries;
	char* buf;
	
};
static bool is_pow_2(int x) {
	return (x & (x - 1)) == 0;
}
static void circ_buf__init(struct circ_buf_t* cbuf, int sz, int el_size) {
	int i;

	cbuf->sz = sz;
	cbuf->el_size = el_size;
	cbuf->p = 0;
	spin_lock_init(&cbuf->lock);
	cbuf->entries = kzalloc(sz * sizeof(struct circ_buf_entry_t), GFP_KERNEL);
	cbuf->buf = kzalloc(sz * el_size, GFP_KERNEL);
	for (i = 0; i<sz; ++i) {
		cbuf->entries[i].valid = false;
	}
}
static char* circ_buf__alloc(struct circ_buf_t* cbuf) {
	int idx;

	spin_lock(&cbuf->lock);
	idx = cbuf->p++ % cbuf->sz;
	cbuf->entries[idx].valid = true;
	return &cbuf->buf[cbuf->el_size * idx];
}
static void circ_buf__mark_visible(struct circ_buf_t* cbuf) {
	spin_unlock(&cbuf->lock);
}
static bool circ_buf__get(struct circ_buf_t* cbuf, int i, void* fill) {
	int idx;

	spin_lock(&cbuf->lock);
	idx = (cbuf->p + (cbuf->sz - 1 - i)) % cbuf->sz;
	if (!cbuf->entries[idx].valid) {
		spin_unlock(&cbuf->lock);
		return false;
	}
	memcpy(fill, &cbuf->buf[cbuf->el_size * idx], cbuf->el_size);
	spin_unlock(&cbuf->lock);
	return true;
}
static void circ_buf__free(struct circ_buf_t* cbuf) {
	kfree(cbuf->entries);
	kfree(cbuf->buf);
}

struct map_t {
	const char* id;
	int scratch_offs;
	struct hash_map_t map;
	struct circ_buf_t past_keys;
};
static struct map_t maps[MAX_N_MAPS];

struct combiner_t {
	fstore_combiner_fn_t fn;
	int n_maps;
	struct map_t** maps;
	struct circ_buf_t past_results;
};
static struct combiner_t combiners[MAX_N_COMBINERS];
static struct mutex fstore_init_mutex;

static void fstore_init(void) {
	int i;
	mutex_init(&fstore_init_mutex);
	for (i = 0; i<MAX_N_MAPS; ++i) {
		maps[i].id.strs[0] = NULL;
	}
	for (i = 0; i<MAX_N_COMBINERS; ++i) {
		combiners[i].maps = NULL;
	}
	for (i = 0; i<MAX_N_KEYS; ++i) {
		key_infos[i].id = NULL;
	}
}

static void fstore_exit(void) {
	int i;
	for (i = 0; i<MAX_N_MAPS; ++i) {
		if (is_uuid_empty(&maps[i].id)) {
			hash_map__free(&maps[i].map);
			circ_buf__free(&maps[i].past_keys);
		}
	}
	for (i = 0; i<MAX_N_COMBINERS; ++i) {
		if (combiners[i].maps != NULL) {
			kfree(combiners[i].maps);
			circ_buf__free(&combiners[i].past_results);
		}
	}
	for (i = 0; i<MAX_N_KEYS; ++i) {
		key_infos[i].id = NULL;
	}
}

int fstore_register_map(fstore_uuid_t id, const char* key_id, int scratch_offs, unsigned scratch_sz, fstore_map_ptr_t* map, int n_past_track) {
	int map_i;
	int key_i;

	if (!is_pow_2(n_past_track)) {
		return FSTORE_API_FAILURE;
	}
	mutex_lock(&fstore_init_mutex);

	map_i = 0;
	while (map_i < MAX_N_MAPS && !is_uuid_empty(&maps[i].id)) {
		map_i += 1;
	}
	if (map_i >= MAX_N_MAPS) {
		mutex_unlock(&fstore_init_mutex);
		return FSTORE_API_FAILURE;
	}

	key_i = 0;
	while (key_i < MAX_N_KEYS && key_infos[key_i].id != NULL && strcmp(key_id, key_infos[key_i].id) != 0) {
		key_i += 1;
	}
	if (key_i >= MAX_N_KEYS) {
		mutex_unlock(&fstore_init_mutex);
		return FSTORE_API_FAILURE;
	}

	if (key_infos[key_i].id == NULL) {
		key_infos[key_i].id = key_id;
		key_infos[key_i].scratch_offs = scratch_offs;
		key_infos[key_i].scratch_sz = scratch_sz;
	}

	maps[map_i].id = id;

	if (key_infos[key_i].scratch_sz >= sizeof(fstore_val_type_t)) {
		maps[map_i].scratch_offs = key_infos[key_i].scratch_offs;
		key_infos[key_i].scratch_offs += sizeof(fstore_val_type_t);
		key_infos[key_i].scratch_sz -= sizeof(fstore_val_type_t);
	} else {
		maps[map_i].scratch_offs = -1;
		hash_map__init(&maps[map_i].map, HASH_TABLE_SIZE);
	}

	circ_buf__init(&maps[map_i].past_keys, n_past_track, sizeof(fstore_key_type_t));
	*map = (fstore_map_ptr_t) &maps[map_i];

	mutex_unlock(&fstore_init_mutex);
	return FSTORE_API_SUCCESS;
}

int fstore_register_combiner_fn(int n_maps, int n_past, fstore_uuid_t* ids, fstore_combiner_fn_t fn, int n_bytes_ret, fstore_combiner_id_t* id) {
	int i, j;
	struct map_t** p_maps;

	if (!is_pow_2(n_past)) {
		return FSTORE_API_FAILURE;
	}

	mutex_lock(&fstore_init_mutex);
	i = 0;
	while (i < MAX_N_COMBINERS && combiners[i].maps != NULL) {
		i += 1;
	}
	if (i >= MAX_N_COMBINERS) {
		mutex_unlock(&fstore_init_mutex);
		return FSTORE_API_FAILURE;
	}
	if (n_maps > MAX_N_MAPS_PER_COMBINER) {
		mutex_unlock(&fstore_init_mutex);
		return FSTORE_API_FAILURE;
	}

	combiners[i].n_maps = n_maps;
	combiners[i].fn = fn;
	circ_buf__init(&combiners[i].past_results, n_past, n_bytes_ret);

	p_maps = kzalloc(sizeof(struct map_t*) * n_maps, GFP_KERNEL);
	combiners[i].maps = p_maps;

	for (j = 0; j<n_maps; ++j) {
		for (i = 0; i<MAX_N_MAPS; ++i) {
			if (uuid_eql(&maps[i].id, &ids[j])) {
				p_maps[j] = &maps[i];
				break;
			}
		}
	}

	*id = i;
	mutex_unlock(&fstore_init_mutex);
	return FSTORE_API_SUCCESS;
}

// TODO add check if no one is subscribing to the map, don't report any data.
int fstore_insert(fstore_map_ptr_t map_p, fstore_key_type_t k, fstore_val_type_t v) {
	int ret;
	struct map_t* map;
	char* p;

	map = (struct map_t*) map_p;
	if (map->scratch_offs >= 0) {
		char* scratch_p = ((char*) k) + map->scratch_offs;
		*((fstore_val_type_t*) scratch_p) = v;
		ret = FSTORE_API_SUCCESS;
	} else {
		ret = hash_map__insert(&map->map, k, v) ? FSTORE_API_SUCCESS : FSTORE_API_FAILURE;
	}

	p = circ_buf__alloc(&map->past_keys);
	memcpy(p, &k, sizeof(fstore_key_type_t));
	circ_buf__mark_visible(&map->past_keys);

	return ret;
}

int fstore_combine(fstore_combiner_id_t id, fstore_key_type_t* keys, int lookup_dim) {
	int j;
	fstore_key_type_t* map_keys;
	fstore_val_type_t args[MAX_N_COMBINER_ARGS];
	struct combiner_t* m;
	fstore_key_type_t map_keys_buf[MAX_N_MAPS_PER_COMBINER];
	fstore_key_type_t k;
	fstore_val_type_t v;
	char* p;
	struct map_t* map;

	m = &combiners[id];
	if (keys == NULL) {
		if (lookup_dim == -1) {
			for (j = 0; j<m->n_maps; ++j) {
				map = m->maps[j];
				if (!circ_buf__get(&map->past_keys, 0, &map_keys_buf[j])) {
					return FSTORE_API_FAILURE;
				}
			}
		} else {
			map = m->maps[lookup_dim];
			if (!circ_buf__get(&map->past_keys, 0, &k)) {
				return FSTORE_API_FAILURE;
			}
			for (j = 0; j<m->n_maps; ++j) {
				map_keys_buf[j] = k;
			}
		}
		map_keys = &map_keys_buf[0];
	} else {
		map_keys = keys;
	}
	for (j = 0; j<m->n_maps; ++j) {
		map = m->maps[j];
		if (map->scratch_offs >= 0) {
			p = ((char*) map_keys[j]) + map->scratch_offs;
			v = *((fstore_val_type_t*) p);
		} else {
			if (!hash_map__lookup(&map->map, map_keys[j], &v)) {
				return FSTORE_API_FAILURE;
			}
		}
		args[j] = v;
	}
	m->fn(&args[0], circ_buf__alloc(&m->past_results));
	circ_buf__mark_visible(&m->past_results);
	return FSTORE_API_SUCCESS;
}

int fstore_query_past(fstore_combiner_id_t id, int n_past, void** vals) {
	int i;
	struct combiner_t* m;
	char* p;
       
	m = &combiners[id];
	if (n_past > m->past_results.sz) {
		return FSTORE_API_FAILURE;
	}

	for (i = n_past-1; i>=0; --i) {
		p = (char*) vals[i];
		if (!circ_buf__get(&m->past_results, i, p)) {
			return FSTORE_API_FAILURE;
		}
	}
	return FSTORE_API_SUCCESS;
}

static int __init lake_fv_init(void) {
	fstore_init();
	printk(KERN_INFO "lake_fv: module loaded\n");
	return 0;
}

static void __exit lake_fv_exit(void) {
	fstore_exit();
	printk(KERN_INFO "lake_fv: module unloaded\n");
}

// Register the module initialization and exit functions
module_init(lake_fv_init);
module_exit(lake_fv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fstore");
MODULE_AUTHOR("fat rat");
