
#ifndef _FSTORE_H_
#define _FSTORE_H_

#include <stdint.h>
#include <stdbool.h>

typedef void* map_ptr_t;
typedef uint64_t key_type_t;
typedef uint64_t val_type_t;
typedef uint64_t (*compose_fn_t)(uint64_t*);
typedef int model_id_t;

// Take an unique string identifier for the map, and return an opaque pointer to the map.
// Provide a scratch space on the key scratch, if available (if not, scratch_offs=-1).
bool fstore_register_map(const char* id, int scratch_offs, unsigned scratch_sz, map_ptr_t* map);

// Provide a function to compose from maps the user is interested in. Return an id for the composer.
bool fstore_register_compose_fn(int n_maps, const char** ids, map_ptr_t* maps, compose_fn_t fn, model_id_t* id);

// Insert (k,v) into the map p
bool fstore_insert(map_ptr_t p, key_type_t k, val_type_t v);

// Compose n_advance new features, and return n_past total features.
// There is an array of n_advance x n_maps keys, and n_past vals.
bool fstore_compose(model_id_t id, int n_advance, int n_past, key_type_t** keys, val_type_t* val);

#endif
