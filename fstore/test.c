
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

struct bio {
	int bdev;
	int offset;
	int size;
	int finish_time;
};

static bool cmp_bio_finish_times(struct bio* b1, struct bio* b2) {
	return b1->finish_time < b2->finish_time;
}

struct bio_pq {
	int size;
	bool (*cmp_fn_lt)(struct bio*, struct bio*);
	struct bio arr[1024];
};

static void swap(struct bio* b1, struct bio* b2) {
	struct bio tmp = *b1;
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
	pq->arr[pos] = *b;
	while (pos > 0 && pq->cmp_fn_lt(&pq->arr[pos], &pq->arr[pos / 2])) {
		swap(&pq->arr[pos], &pq->arr[pos / 2]);
		pos /= 2;
	}
	return true;

}

static bool bio_pq__top(struct bio_pq* pq, struct bio* fill) {
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
		bool can_sift_left = pos*2 < pq->size && !pq->cmp_fn_lt(&pq->arr[pos], &pq->arr[pos*2]);
		bool can_sift_right = pos*2+1 < pq->size && !pq->cmp_fn_lt(&pq->arr[pos], &pq->arr[pos*2+1]);

		if (can_sift_left && can_sift_right) {
			if (pq->cmp_fn_lt(&pq->arr[2*pos], &pq->arr[2*pos+1])) {
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


int main() {
	struct bio_pq* pq = malloc(sizeof(struct bio_pq));
	bio_pq__init(pq, cmp_bio_finish_times);
	
	for (int t = 0; t < 1000; ++t) {
		struct bio top;
		if (bio_pq__top(pq, &top) && top.finish_time <= t) {
			bio_pq__pop(pq);
		}
		if (t % 4 == 0) {
			struct bio b;
			b.bdev = 1;
			b.offset = rand() % 1048576;
			b.size = rand() % 1024;
			b.finish_time = t + (rand() % 200);
			bio_pq__push(pq, &b);
		}
	}

	free(pq);
	return 0;
}
