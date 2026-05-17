#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define THREADPOOL_QUEUE_SIZE 256

typedef void (*threadpool_task_fn)(void* arg);

typedef struct {
    threadpool_task_fn fn;
    void* arg;
} threadpool_task;

typedef struct {
    pthread_t* workers;
    int num_workers;

    threadpool_task* queue;
    int queue_head;
    int queue_tail;
    int queue_count;

    pthread_mutex_t queue_lock;
    pthread_cond_t queue_not_empty;
    pthread_cond_t queue_not_full;

    atomic_bool shutdown;
} threadpool;

threadpool* threadpool_init(int num_workers);
int threadpool_submit(threadpool* pool, threadpool_task_fn fn, void* arg);
void threadpool_shutdown(threadpool* pool);
int threadpool_queue_depth(const threadpool* pool);
