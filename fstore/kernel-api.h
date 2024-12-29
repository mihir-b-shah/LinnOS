
#ifndef KERNEL_API_
#define KERNEL_API_

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef pthread_spinlock_t spinlock_t;
typedef pthread_mutex_t mutex;
typedef uint64_t u64;

#define GFP_KERNEL 0

static void* kzalloc(int sz, int t) {
	return calloc(sz, 1);
}

static void kfree(void* p) {
	free(p);
}

static void spin_lock_init(pthread_spinlock_t* lock) {
	pthread_spin_init(lock, 0);
}

static void spin_lock(pthread_spinlock_t* lock) {
	pthread_spin_lock(lock);
}

static void spin_unlock(pthread_spinlock_t* lock) {
	pthread_spin_unlock(lock);
}

static void mutex_init(pthread_mutex_t* mutex) {
	pthread_mutex_init(mutex, NULL);
}

static void mutex_lock(pthread_mutex_t* mutex) {
	pthread_mutex_lock(mutex);
}

static void mutex_unlock(pthread_mutex_t* mutex) {
	pthread_mutex_unlock(mutex);
}

#define KERN_INFO ""

static void printk(const char* p) {
	printf("%s", p);
}

#define __init
#define __exit
#define late_initcall(x) 

#endif

