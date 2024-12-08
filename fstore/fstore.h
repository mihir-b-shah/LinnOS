
#ifndef _FSTORE_H_
#define _FSTORE_H_

#include <stdint.h>
#include <stdbool.h>

typedef void* map_ptr_t;
typedef uint64_t key_type_t;
typedef uint64_t val_type_t;
typedef void (*model_fn_t)(uint64_t*, void*);
typedef int model_id_t;

void fstore_init();
void fstore_exit();

// Take an unique string identifier for the map and key, and return an opaque pointer to the map.
// Provide a scratch space on the key scratch, if available (if not, scratch_offs=-1).
bool fstore_register_map(const char* id, const char* key_id, int scratch_offs, unsigned scratch_sz, map_ptr_t* map, int n_past_track);

// Provide a function to model from maps the user is interested in. Return an id for the model requestor.
bool fstore_register_model_fn(int n_maps, int n_past, const char** ids, model_fn_t fn, int n_bytes_ret, model_id_t* id);

// Insert (k,v) into the map p
bool fstore_insert(map_ptr_t p, key_type_t k, uint64_t v);

// Create n_advance features based on provided keys, which have dim n_advance x n_maps. If keys is NULL, then instead of looking up
// using provided keys, we get the temporally last n_advance keys on map lookup_dim, and use that key to lookup across all maps. If
// lookup_dim = -1, then we get the last n_advance features across ALL maps (that do not necessarily line up), and add features.
bool fstore_advance(model_id_t id, key_type_t* keys, int lookup_dim);

// Compose n_advance new features, and return n_past total features.
// There is an array of n_advance x n_maps keys, and n_past vals.
bool fstore_model(model_id_t id, int n_past, void** val);

#endif
