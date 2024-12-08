
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fstore.h"

#define MAX_N_MAPS 256
#define MAX_N_KEYS 256
#define MAX_N_MODELS 256
#define HASH_TABLE_SIZE 4096
#define HASH_TABLE_ASSOC 1
#define MAX_N_MODEL_ARGS 16

struct key_scratch_info_t {
	const char* id;
	int scratch_offs;
	int scratch_sz;
};
static struct key_scratch_info_t key_infos[MAX_N_KEYS];

struct kv_t {
	int p;
	bool valid[HASH_TABLE_ASSOC];
	uint64_t k[HASH_TABLE_ASSOC];
	uint64_t v[HASH_TABLE_ASSOC];
};
struct hash_map_t {
	struct kv_t* table;
};
static uint64_t hash(uint64_t x) {
	return x;
}
static void hash_map__init(struct hash_map_t* map, int sz) {
	map->table = malloc(sizeof(struct kv_t) * sz);
	for (int i = 0; i<sz; ++i) {
		map->table[i].p = 0;
	}
}
static bool hash_map__insert(struct hash_map_t* map, uint64_t k, uint64_t v) {
	struct kv_t* kv = &map->table[hash(k)];
	int pos = kv->p++ % HASH_TABLE_ASSOC;
	kv->k[pos] = k;
	kv->v[pos] = v;
	return true;
}
static bool hash_map__lookup(struct hash_map_t* map, uint64_t k, uint64_t* v) {
	struct kv_t* kv = &map->table[hash(k)];
	for (int i = 0; i<HASH_TABLE_ASSOC; ++i) {
		if (kv->k[i] == k) {
			*v = kv->v[i];
			return true;
		}
	}
	return false;
}

struct map_t {
	const char* id;
	int scratch_offs;
	struct hash_map_t map;
};
static struct map_t maps[MAX_N_MAPS];

struct model_t {
	model_fn_t fn;
	int n_maps;
	int n_past;
	int n_bytes_ret;
	struct map_t** maps;
	char* past_results;
	int past_p;
};
struct model_t models[MAX_N_MODELS];

void fstore_init() {
	for (int i = 0; i<MAX_N_MAPS; ++i) {
		maps[i].id = NULL;
	}
	for (int i = 0; i<MAX_N_MODELS; ++i) {
		models[i].maps = NULL;
	}
	for (int i = 0; i<MAX_N_KEYS; ++i) {
		key_infos[i].id = NULL;
	}
}

void fstore_exit() {
}

bool fstore_register_map(const char* id, const char* key_id, int scratch_offs, unsigned scratch_sz, map_ptr_t* map) {
	int map_i = 0;
	while (map_i < MAX_N_MAPS && maps[map_i].id != NULL) {
		map_i += 1;
	}
	if (map_i >= MAX_N_MAPS) {
		return false;
	}

	int key_i = 0;
	while (key_i < MAX_N_KEYS && key_infos[key_i].id != NULL && strcmp(key_id, key_infos[key_i].id) != 0) {
		key_i += 1;
	}
	if (key_i >= MAX_N_KEYS) {
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
	*map = (map_ptr_t) &maps[map_i];
	return true;
}

bool fstore_register_model_fn(int n_maps, int n_past, const char** ids, model_fn_t fn, int n_bytes_ret, model_id_t* id) {
	int i = 0;
	while (i < MAX_N_MODELS && models[i].maps != NULL) {
		i += 1;
	}
	if (i >= MAX_N_MODELS) {
		return false;
	}

	models[i].n_maps = n_maps;
	models[i].fn = fn;
	models[i].n_bytes_ret = n_bytes_ret;
	models[i].n_past = n_past;
	models[i].past_results = malloc(n_past * n_bytes_ret);
	models[i].past_p = 0;

	struct map_t** p_maps = malloc(sizeof(struct map_t*) * n_maps);
	models[i].maps = p_maps;

	for (int j = 0; j<n_maps; ++j) {
		for (int i = 0; i<MAX_N_MAPS; ++i) {
			if (strcmp(maps[i].id, ids[j]) == 0) {
				p_maps[j] = &maps[i];
				break;
			}
		}
	}

	*id = i;
	return true;
}

bool fstore_insert(map_ptr_t p, key_type_t k, val_type_t v) {
	struct map_t* map = (struct map_t*) p;
	if (map->scratch_offs >= 0) {
		char* p = ((char*) k) + map->scratch_offs;
		*((val_type_t*) p) = v;
		return true;
	} else {
		return hash_map__insert(&map->map, k, v);
	}
}

bool fstore_model(model_id_t id, int n_advance, int n_past, key_type_t** keys, void** vals) {
	val_type_t args[MAX_N_MODEL_ARGS];
	struct model_t* m = &models[id];

	if (n_past > m->n_past) {
		return false;
	}

	for (int i = 0; i<n_advance; ++i) {
		key_type_t* map_keys = keys[i];
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
		m->fn(&args[0], &m->past_results[m->n_bytes_ret * (m->past_p++ % m->n_past)]);
	}

	for (int i = 0; i<n_past; ++i) {
		char* p = (char*) vals[i];
		memcpy(p, &m->past_results[m->n_bytes_ret * (m->past_p + (m->n_past - 1)  - i)], m->n_bytes_ret);
	}
	return true;
}
