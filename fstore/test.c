
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

	fstore_uuid_t uuid_start;
	uuid_start.strs[0] = "sda";
	uuid_start.strs[1] = "bio__start_time";
	fstore_uuid_t uuid_end;
	uuid_end.strs[0] = "sda";
	uuid_end.strs[1] = "bio__end_time";

	fv_init();

	fstore_map_ptr_t start_map;
	if (fstore_register_map(uuid_start, "bio", -1 /*offsetof(struct bio, scratch)*/, 0 /*member_sz(struct bio, scratch)*/, &start_map, 16) != FSTORE_API_SUCCESS) {
		printf("big sad\n");
	}
	fstore_map_ptr_t end_map;
	if (fstore_register_map(uuid_end, "bio", -1 /*offsetof(struct bio, scratch)*/, 0 /*member_sz(struct bio, scratch)*/, &end_map, 16) != FSTORE_API_SUCCESS) {
		printf("big sad 2\n");
	}

	for (int t = 0; t < 1000; ++t) {
		struct bio* top;
		if (bio_pq__cond_pop(pq, &top, t)) {
			fstore_insert(end_map, (uint64_t) top, (uint64_t) top->finish_time);
		}
		if (t % 4 == 0) {
			fstore_key_type_t keys[4];
			bool r = true;
			r &= fstore_get_past_keys(end_map, 4, &keys[0]) == FSTORE_API_SUCCESS;
			uint64_t latency[4];
			if (r) {
				for (int i = 0; i<4; ++i) {
					fstore_val_type_t start_time, end_time;
					r &= fstore_query(start_map, keys[i], &start_time) == FSTORE_API_SUCCESS;
					r &= fstore_query(end_map, keys[i], &end_time) == FSTORE_API_SUCCESS;
					latency[i] = (int) (end_time - start_time);
				}
				if (r) {
					printf("t: %d | past_latency: %lu %lu %lu %lu\n", t, latency[0], latency[1], latency[2], latency[3]);
				}
			}

			struct bio* b = malloc(sizeof(struct bio));
			b->bdev = 1;
			b->offset = rand() % 1048576;
			b->size = rand() % 1024;
			b->finish_time = t + (rand() % 200);
			bio_pq__push(pq, b);
			
			fstore_insert(start_map, (uint64_t) b, (uint64_t) t);
		}
	}

	free(pq);
	return 0;
}
