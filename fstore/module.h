

#ifndef _FSTORE_H_
#define _FSTORE_H_

#include <linux/types.h>

#define FSTORE_API_SUCCESS 0
#define FSTORE_API_FAILURE -1

/*
This API provides a feature store that tries to decouple reporting data from using it, in the kernel.
The idea is, any point in the kernel can report data, for example, every time a block_io is started,
we report the timestamp. Then, anyone wanting to build features from such data (a "subscriber"), can
subscribe to multiple such reporting streams, and build features from the streams.

All functions return 0/-1 for success/failure, real return values are given through parameters.
*/

typedef struct fstore_uuid_impl_t {
	const char* strs[2];
} fstore_uuid_t;

typedef void* fstore_map_ptr_t;
typedef u64 fstore_key_type_t;
typedef u64 fstore_val_type_t;

/*
This is a control plane operation for anyone wanting to report data, anywhere in the kernel. Reported data is stored using a key-value map.
id is a UUID for this reporting location, and key_id is a UUID for the key type.
Sometimes for efficiency, instead of maintaining many maps, we can store fields on the key struct itself. If this is the case,
the user can supply the offset on the key struct where there is scratch space, and how much there is (scratch_offs and scratch_sz).
If that is not possible, they can supply scratch_offs = -1.
Finally, they pass n_past_track to determine the size of the map (since we cannot store unbounded streams of data).
The function returns an opaque handle fstore_map_ptr_t to the created map, via the parameter map. 
*/
int fstore_register_map(fstore_uuid_t id, const char* key_id, int scratch_offs, unsigned scratch_sz, fstore_map_ptr_t* map, int n_past_track);

/*
This is a control plane operation for anyone wanting to subscribe to some maps. The function is provided a list of n_maps map id's,
and it fills the maps array with map handles.
*/
int fstore_register_subscriber(int n_maps, fstore_uuid_t* ids, fstore_map_ptr_t* maps);

/*
This is the data plane operation for reporting data. It takes the map handle obtained from the register_map function, and inserts
(k,v) pair into that map. If k is already there, v is updated.
*/
int fstore_insert(fstore_map_ptr_t p, fstore_key_type_t k, fstore_val_type_t v);

/*
This is the data plane operation for finding the past k keys reported by a map. It takes the map handle, and how many keys to report.
It then fills the keys array.
*/
int fstore_get_past_keys(fstore_map_ptr_t map, int n_past, fstore_key_type_t* keys);

/*
This is the data plane operation for simple map key-value lookup.
*/
int fstore_query(fstore_map_ptr_t map, fstore_key_type_t k, fstore_val_type_t* val);

#endif
