#pragma once

#include <pthread.h>

#ifdef __APPLE__

#define PTHREAD_BARRIER_SERIAL_THREAD 1

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned int count;
    unsigned int arrived;
    unsigned int generation;
} portable_barrier_t;

static inline int portable_barrier_init(portable_barrier_t* barrier, const void* attr,
                                        unsigned int count) {
    (void) attr;
    int rc;
    if ((rc = pthread_mutex_init(&barrier->mutex, NULL)) != 0)
        return rc;
    if ((rc = pthread_cond_init(&barrier->cond, NULL)) != 0) {
        pthread_mutex_destroy(&barrier->mutex);
        return rc;
    }
    barrier->count = count;
    barrier->arrived = 0;
    barrier->generation = 0;
    return 0;
}

static inline int portable_barrier_wait(portable_barrier_t* barrier) {
    pthread_mutex_lock(&barrier->mutex);
    unsigned int gen = barrier->generation;
    barrier->arrived++;
    if (barrier->arrived == barrier->count) {
        barrier->generation++;
        barrier->arrived = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (gen == barrier->generation) {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
    }
    pthread_mutex_unlock(&barrier->mutex);
    return 0;
}

static inline int portable_barrier_destroy(portable_barrier_t* barrier) {
    int rc1 = pthread_mutex_destroy(&barrier->mutex);
    int rc2 = pthread_cond_destroy(&barrier->cond);
    return rc1 ? rc1 : rc2;
}

#define pthread_barrier_t portable_barrier_t
#define pthread_barrierattr_t void
#define pthread_barrier_init portable_barrier_init
#define pthread_barrier_wait portable_barrier_wait
#define pthread_barrier_destroy portable_barrier_destroy

#else

#ifndef PTHREAD_BARRIER_SERIAL_THREAD
#define PTHREAD_BARRIER_SERIAL_THREAD 1
#endif

#endif
