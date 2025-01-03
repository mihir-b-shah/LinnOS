
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include "module.h"

#define member_sz(st, m) sizeof(((st*) 0)->m)

struct bio {
	int bdev;
	int offset;
	int size;
	int finish_time;

	char scratch[16];
};

static int bio_key(struct bio* b) {
	return b->finish_time;
}

struct bio_pq {
	int size;
	int (*key_fn)(struct bio*);
	struct bio* arr[1024];
	pthread_spinlock_t lock;
};

static void swap(struct bio** b1, struct bio** b2) {
	struct bio* tmp = *b1;
	*b1 = *b2;
	*b2 = tmp;
}

static void bio_pq__init(struct bio_pq* pq, int (*key_fn)(struct bio*)) {
	pq->size = 0;
	pq->key_fn = key_fn;
	pthread_spin_init(&pq->lock, 0);
}

static bool bio_pq__push(struct bio_pq* pq, struct bio* b) {
	pthread_spin_lock(&pq->lock);
	int pos = pq->size++;
	if (pos >= sizeof(pq->arr)/sizeof(pq->arr[0])) {
		pthread_spin_unlock(&pq->lock);
		return false;
	}
	pq->arr[pos] = b;
	while (pos > 0 && pq->key_fn(pq->arr[pos]) < pq->key_fn(pq->arr[pos / 2])) {
		swap(&pq->arr[pos], &pq->arr[pos / 2]);
		pos /= 2;
	}
	pthread_spin_unlock(&pq->lock);
	return true;
}

static bool bio_pq__cond_pop(struct bio_pq* pq, struct bio** fill, int t) {
	pthread_spin_lock(&pq->lock);
	if (pq->size <= 0) {
		pthread_spin_unlock(&pq->lock);
		return false;
	}
	if (pq->key_fn(pq->arr[0]) >= t) {
		pthread_spin_unlock(&pq->lock);
		return false;
	}

	int pos = --(pq->size);
	*fill = pq->arr[0];
	swap(&pq->arr[0], &pq->arr[pos]);

	pos = 0;
	while (true) {
		bool can_sift_left = pos*2 < pq->size && pq->key_fn(pq->arr[pos]) >= pq->key_fn(pq->arr[pos*2]);
		bool can_sift_right = pos*2+1 < pq->size && pq->key_fn(pq->arr[pos]) >= pq->key_fn(pq->arr[pos*2+1]);

		if (can_sift_left && can_sift_right) {
			if (pq->key_fn(pq->arr[2*pos]) < pq->key_fn(pq->arr[2*pos+1])) {
				swap(&pq->arr[pos], &pq->arr[2*pos]);
				pos = 2*pos;
			} else {
				swap(&pq->arr[pos], &pq->arr[2*pos+1]);
				pos = 2*pos+1;
			}
		} else if (can_sift_left) {
			swap(&pq->arr[pos], &pq->arr[2*pos]);
			pos = 2*pos;
		} else if (can_sift_right) {
			swap(&pq->arr[pos], &pq->arr[2*pos+1]);
			pos = 2*pos+1;
		} else {
			break;
		}
	}
	pthread_spin_unlock(&pq->lock);
	return true;
}

int main() {
	struct bio_pq* pq = malloc(sizeof(struct bio_pq));
	bio_pq__init(pq, bio_key);

	const char* devices[13] = {"sda", "sdb", "sdc", "loop0", "loop1", "loop2", "loop3", "loop4", "loop5", "loop6", "loop7", "loop8", "loop9"};
	const char* maps[2] = {"bio__start_time", "bio__end_time"};

	fstore_uuid_t uuids[13][2];
	fstore_map_ptr_t map_ptrs[13][2];
	for (int i = 0; i<13; ++i) {
		for (int j = 0; j<2; ++j) {
			uuids[i][j].strs[0] = devices[i];
			uuids[i][j].strs[1] = maps[j];
			if (fstore_register_map(uuids[i][j], "bio", offsetof(struct bio, scratch), member_sz(struct bio, scratch), &map_ptrs[i][j], 16) != FSTORE_API_SUCCESS) {
				printf("big sad\n");
			}
		}
	}

	fv_init();

	for (int t = 0; t < 1000; ++t) {
		struct bio* top;
		if (bio_pq__cond_pop(pq, &top, t)) {
			fstore_insert(map_ptrs[t % 13][1], (uint64_t) top, (uint64_t) top->finish_time);
		}
		if (t % 4 == 0) {
			for (int j = 0; j<13; ++j) {
				fstore_key_type_t keys[4];
				bool r = true;
				r &= fstore_get_past_keys(map_ptrs[j][1], 4, &keys[0]) == FSTORE_API_SUCCESS;
				uint64_t latency[4];
				if (r) {
					for (int i = 0; i<4; ++i) {
						fstore_val_type_t start_time, end_time;
						r &= fstore_query(map_ptrs[j][0], keys[i], &start_time) == FSTORE_API_SUCCESS;
						r &= fstore_query(map_ptrs[j][1], keys[i], &end_time) == FSTORE_API_SUCCESS;
						latency[i] = (int) (end_time - start_time);
					}
					if (r) {
						printf("t: %d | past_latency: %lu %lu %lu %lu\n", t, latency[0], latency[1], latency[2], latency[3]);
					}
				}
			}

			struct bio* b = malloc(sizeof(struct bio));
			b->bdev = 1;
			b->offset = rand() % 1048576;
			b->size = rand() % 1024;
			b->finish_time = t + (rand() % 200);
			bio_pq__push(pq, b);
			
			fstore_insert(map_ptrs[(t / 4) % 13][0], (uint64_t) b, (uint64_t) t);
		}
	}

	free(pq);
	return 0;
}
