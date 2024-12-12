
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

#include "fstore.h"

#define MAX_N_MAPS 256
#define MAX_N_KEYS 256
#define MAX_N_COMBINERS 256
#define HASH_TABLE_SIZE 4096
#define HASH_TABLE_ASSOC 1
#define MAX_N_COMBINER_ARGS 16
#define MAX_N_MAPS_PER_COMBINER 16

struct key_scratch_info_t {
	const char* id;
	int scratch_offs;
	int scratch_sz;
};
static struct key_scratch_info_t key_infos[MAX_N_KEYS];

struct kv_t {
	int p;
	pthread_spinlock_t lock;
	bool valid[HASH_TABLE_ASSOC];
	key_type_t k[HASH_TABLE_ASSOC];
	val_type_t v[HASH_TABLE_ASSOC];
};
struct hash_map_t {
	int capacity;
	struct kv_t* table;
};
static uint64_t hash(key_type_t x) {
	return x;
}
static void hash_map__init(struct hash_map_t* map, int sz) {
	map->capacity = sz;
	map->table = malloc(sizeof(struct kv_t) * sz);
	for (int i = 0; i<sz; ++i) {
		map->table[i].p = 0;
		pthread_spin_init(&map->table[i].lock, 0);
	}
}
static bool hash_map__insert(struct hash_map_t* map, key_type_t k, val_type_t v) {
	struct kv_t* kv = &map->table[hash(k)];
	pthread_spin_lock(&kv->lock);
	int pos = kv->p++ % HASH_TABLE_ASSOC;
	kv->k[pos] = k;
	kv->v[pos] = v;
	pthread_spin_unlock(&kv->lock);
	return true;
}
static bool hash_map__lookup(struct hash_map_t* map, key_type_t k, val_type_t* v) {
	struct kv_t* kv = &map->table[hash(k)];
	pthread_spin_lock(&kv->lock);
	for (int i = 0; i<HASH_TABLE_ASSOC; ++i) {
		if (kv->k[i] == k) {
			*v = kv->v[i];
			return true;
		}
	}
	pthread_spin_unlock(&kv->lock);
	return false;
}
static void hash_map__free(struct hash_map_t* map) {
	free(map->table);
	for (int i = 0; i<map->capacity; ++i) {
		pthread_spin_destroy(&map->table[i].lock);
	}
}

struct circ_buf_entry_t {
	bool valid;
};
struct circ_buf_t {
	int sz;
	int el_size;
	int p;
	pthread_spinlock_t lock;
	struct circ_buf_entry_t* entries;
	char* buf;
	
};
static bool is_pow_2(int x) {
	return (x & (x - 1)) == 0;
}
static void circ_buf__init(struct circ_buf_t* cbuf, int sz, int el_size) {
	assert(is_pow_2(sz));
	cbuf->sz = sz;
	cbuf->el_size = el_size;
	cbuf->p = 0;
	pthread_spin_init(&cbuf->lock, 0);
	cbuf->entries = malloc(sz * sizeof(struct circ_buf_entry_t));
	cbuf->buf = malloc(sz * el_size);
	for (int i = 0; i<sz; ++i) {
		cbuf->entries[i].valid = false;
	}
}
static char* circ_buf__alloc(struct circ_buf_t* cbuf) {
	pthread_spin_lock(&cbuf->lock);
	int idx = cbuf->p++ % cbuf->sz;
	cbuf->entries[idx].valid = true;
	return &cbuf->buf[cbuf->el_size * idx];
}
static void circ_buf__mark_visible(struct circ_buf_t* cbuf) {
	pthread_spin_unlock(&cbuf->lock);
}
static bool circ_buf__get(struct circ_buf_t* cbuf, int i, void* fill) {
	pthread_spin_lock(&cbuf->lock);
	int idx = (cbuf->p + (cbuf->sz - 1 - i)) % cbuf->sz;
	if (!cbuf->entries[idx].valid) {
		pthread_spin_unlock(&cbuf->lock);
		return false;
	}
	memcpy(fill, &cbuf->buf[cbuf->el_size * idx], cbuf->el_size);
	pthread_spin_unlock(&cbuf->lock);
	return true;
}
static void circ_buf__free(struct circ_buf_t* cbuf) {
	free(cbuf->entries);
	free(cbuf->buf);
	pthread_spin_destroy(&cbuf->lock);
}

struct map_t {
	const char* id;
	int scratch_offs;
	struct hash_map_t map;
	struct circ_buf_t past_keys;
};
static struct map_t maps[MAX_N_MAPS];

struct combiner_t {
	combiner_fn_t fn;
	int n_maps;
	struct map_t** maps;
	struct circ_buf_t past_results;
};
static struct combiner_t combiners[MAX_N_COMBINERS];
static pthread_mutex_t init_mutex;

void fstore_init() {
	pthread_mutex_init(&init_mutex, NULL);
	for (int i = 0; i<MAX_N_MAPS; ++i) {
		maps[i].id = NULL;
	}
	for (int i = 0; i<MAX_N_COMBINERS; ++i) {
		combiners[i].maps = NULL;
	}
	for (int i = 0; i<MAX_N_KEYS; ++i) {
		key_infos[i].id = NULL;
	}
}

void fstore_exit() {
	for (int i = 0; i<MAX_N_MAPS; ++i) {
		if (maps[i].id != NULL) {
			hash_map__free(&maps[i].map);
			circ_buf__free(&maps[i].past_keys);
		}
	}
	for (int i = 0; i<MAX_N_COMBINERS; ++i) {
		if (combiners[i].maps != NULL) {
			free(combiners[i].maps);
			circ_buf__free(&combiners[i].past_results);
		}
	}
	for (int i = 0; i<MAX_N_KEYS; ++i) {
		key_infos[i].id = NULL;
	}
	pthread_mutex_destroy(&init_mutex);
}

bool fstore_register_map(const char* id, const char* key_id, int scratch_offs, unsigned scratch_sz, map_ptr_t* map, int n_past_track) {
	pthread_mutex_lock(&init_mutex);

	int map_i = 0;
	while (map_i < MAX_N_MAPS && maps[map_i].id != NULL) {
		map_i += 1;
	}
	if (map_i >= MAX_N_MAPS) {
		pthread_mutex_unlock(&init_mutex);
		return false;
	}

	int key_i = 0;
	while (key_i < MAX_N_KEYS && key_infos[key_i].id != NULL && strcmp(key_id, key_infos[key_i].id) != 0) {
		key_i += 1;
	}
	if (key_i >= MAX_N_KEYS) {
		pthread_mutex_unlock(&init_mutex);
		return false;
	}

	if (key_infos[key_i].id == NULL) {
		key_infos[key_i].id = key_id;
		key_infos[key_i].scratch_offs = scratch_offs;
		key_infos[key_i].scratch_sz = scratch_sz;
	}

	maps[map_i].id = id;
	if (key_infos[key_i].scratch_sz >= sizeof(val_type_t)) {
		maps[map_i].scratch_offs = key_infos[key_i].scratch_offs;
		key_infos[key_i].scratch_offs += sizeof(val_type_t);
		key_infos[key_i].scratch_sz -= sizeof(val_type_t);
	} else {
		maps[map_i].scratch_offs = -1;
		hash_map__init(&maps[map_i].map, HASH_TABLE_SIZE);
	}

	circ_buf__init(&maps[map_i].past_keys, n_past_track, sizeof(key_type_t));
	*map = (map_ptr_t) &maps[map_i];

	pthread_mutex_unlock(&init_mutex);
	return true;
}

bool fstore_register_combiner_fn(int n_maps, int n_past, const char** ids, combiner_fn_t fn, int n_bytes_ret, combiner_id_t* id) {
	pthread_mutex_lock(&init_mutex);
	int i = 0;
	while (i < MAX_N_COMBINERS && combiners[i].maps != NULL) {
		i += 1;
	}
	if (i >= MAX_N_COMBINERS) {
		pthread_mutex_unlock(&init_mutex);
		return false;
	}
	if (n_maps > MAX_N_MAPS_PER_COMBINER) {
		pthread_mutex_unlock(&init_mutex);
		return false;
	}

	combiners[i].n_maps = n_maps;
	combiners[i].fn = fn;
	circ_buf__init(&combiners[i].past_results, n_past, n_bytes_ret);

	struct map_t** p_maps = malloc(sizeof(struct map_t*) * n_maps);
	combiners[i].maps = p_maps;

	for (int j = 0; j<n_maps; ++j) {
		for (int i = 0; i<MAX_N_MAPS; ++i) {
			if (strcmp(maps[i].id, ids[j]) == 0) {
				p_maps[j] = &maps[i];
				break;
			}
		}
	}

	*id = i;
	pthread_mutex_unlock(&init_mutex);
	return true;
}

bool fstore_insert(map_ptr_t map_p, key_type_t k, val_type_t v) {
	struct map_t* map = (struct map_t*) map_p;
	bool ret;

	if (map->scratch_offs >= 0) {
		char* scratch_p = ((char*) k) + map->scratch_offs;
		*((val_type_t*) scratch_p) = v;
		ret = true;
	} else {
		ret = hash_map__insert(&map->map, k, v);
	}

	char* p = circ_buf__alloc(&map->past_keys);
	memcpy(p, &k, sizeof(key_type_t));
	circ_buf__mark_visible(&map->past_keys);

	return ret;
}

bool fstore_combine(combiner_id_t id, key_type_t* keys, int lookup_dim) {
	val_type_t args[MAX_N_COMBINER_ARGS];
	struct combiner_t* m = &combiners[id];

	key_type_t map_keys_buf[MAX_N_MAPS_PER_COMBINER];
	key_type_t* map_keys;
	if (keys == NULL) {
		if (lookup_dim == -1) {
			for (int j = 0; j<m->n_maps; ++j) {
				struct map_t* map = m->maps[j];
				if (!circ_buf__get(&map->past_keys, 0, &map_keys_buf[j])) {
					return false;
				}
			}
		} else {
			struct map_t* map = m->maps[lookup_dim];
			key_type_t k;
			if (!circ_buf__get(&map->past_keys, 0, &k)) {
				return false;
			}
			for (int j = 0; j<m->n_maps; ++j) {
				map_keys_buf[j] = k;
			}
		}
		map_keys = &map_keys_buf[0];
	} else {
		map_keys = keys;
	}
	for (int j = 0; j<m->n_maps; ++j) {
		struct map_t* map = m->maps[j];
		val_type_t v;
		if (map->scratch_offs >= 0) {
			char* p = ((char*) map_keys[j]) + map->scratch_offs;
			v = *((val_type_t*) p);
		} else {
			if (!hash_map__lookup(&map->map, map_keys[j], &v)) {
				return false;
			}
		}
		args[j] = v;
	}
	m->fn(&args[0], circ_buf__alloc(&m->past_results));
	circ_buf__mark_visible(&m->past_results);
	return true;
}

bool fstore_query_past(combiner_id_t id, int n_past, void** vals) {
	val_type_t args[MAX_N_COMBINER_ARGS];
	struct combiner_t* m = &combiners[id];
	if (n_past > m->past_results.sz) {
		return false;
	}

	for (int i = n_past-1; i>=0; --i) {
		char* p = (char*) vals[i];
		if (!circ_buf__get(&m->past_results, i, p)) {
			return false;
		}
	}
	return true;
}
