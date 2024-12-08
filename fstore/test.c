
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "fstore.h"

#define member_sz(st, m) sizeof(((st*) 0)->m)

struct bio {
	int bdev;
	int offset;
	int size;
	int finish_time;

	char scratch[8];
};

static bool cmp_bio_finish_times(struct bio* b1, struct bio* b2) {
	return b1->finish_time < b2->finish_time;
}

struct bio_pq {
	int size;
	bool (*cmp_fn_lt)(struct bio*, struct bio*);
	struct bio* arr[1024];
};

static void swap(struct bio** b1, struct bio** b2) {
	struct bio* tmp = *b1;
	*b1 = *b2;
	*b2 = tmp;
}

static void bio_pq__init(struct bio_pq* pq, bool (*cmp_fn_lt)(struct bio*, struct bio*)) {
	pq->size = 0;
	pq->cmp_fn_lt = cmp_fn_lt;
}

static bool bio_pq__push(struct bio_pq* pq, struct bio* b) {
	int pos = pq->size++;
	if (pos >= sizeof(pq->arr)/sizeof(pq->arr[0])) {
		return false;
	}
	pq->arr[pos] = b;
	while (pos > 0 && pq->cmp_fn_lt(pq->arr[pos], pq->arr[pos / 2])) {
		swap(&pq->arr[pos], &pq->arr[pos / 2]);
		pos /= 2;
	}
	return true;

}

static bool bio_pq__top(struct bio_pq* pq, struct bio** fill) {
	if (pq->size <= 0) {
		return false;
	}
	*fill = pq->arr[0];
	return true;
}

static bool bio_pq__pop(struct bio_pq* pq) {
	int pos = --(pq->size);
	if (pos < 0) {
		return false;
	}
	swap(&pq->arr[0], &pq->arr[pos]);

	pos = 0;
	while (true) {
		bool can_sift_left = pos*2 < pq->size && !pq->cmp_fn_lt(pq->arr[pos], pq->arr[pos*2]);
		bool can_sift_right = pos*2+1 < pq->size && !pq->cmp_fn_lt(pq->arr[pos], pq->arr[pos*2+1]);

		if (can_sift_left && can_sift_right) {
			if (pq->cmp_fn_lt(pq->arr[2*pos], pq->arr[2*pos+1])) {
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
	return true;
}

static void latency_fn(uint64_t* r, void* ret) {
	uint64_t latency = r[1] - r[0];
	memcpy(ret, &latency, sizeof(latency));
}

int main() {
	struct bio_pq* pq = malloc(sizeof(struct bio_pq));
	bio_pq__init(pq, cmp_bio_finish_times);

	map_ptr_t start_map;
	fstore_register_map("bio__start_time", "bio", offsetof(struct bio, scratch), member_sz(struct bio, scratch), &start_map, 4);
	map_ptr_t end_map;
	fstore_register_map("bio__end_time", "bio", offsetof(struct bio, scratch), member_sz(struct bio, scratch), &end_map, 4);
	const char* ids[2] = {"bio__start_time", "bio__end_time"};
	model_id_t model_id;
	fstore_register_model_fn(2, 4, &ids[0], latency_fn, sizeof(uint64_t), &model_id);

	for (int t = 0; t < 1000; ++t) {
		struct bio* top;
		if (bio_pq__top(pq, &top) && top->finish_time <= t) {
			bio_pq__pop(pq);

			fstore_insert(end_map, (uint64_t) top, (uint64_t) top->finish_time);
			
			uint64_t keys[2] = {(uint64_t) top, (uint64_t) top};
			uint64_t* keys_ptr = &keys[0];
			fstore_advance(model_id, NULL, 1);
		}
		if (t % 4 == 0) {
			struct bio* b = malloc(sizeof(struct bio));
			b->bdev = 1;
			b->offset = rand() % 1048576;
			b->size = rand() % 1024;
			b->finish_time = t + (rand() % 200);
			bio_pq__push(pq, b);
			
			fstore_insert(start_map, (uint64_t) b, (uint64_t) t);

			char fill[sizeof(uint64_t) * 4];
			void* arr[4] = {&fill[0], &fill[sizeof(uint64_t)], &fill[2 * sizeof(uint64_t)], &fill[3 * sizeof(uint64_t)]};
			fstore_model(model_id, 4, &arr[0]);
		}
	}

	free(pq);
	return 0;
}
