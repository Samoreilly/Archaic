#include "threadpool.h"
#include <stdlib.h>
#include <unistd.h>

static void* worker_loop(void* arg) {
    threadpool* pool = (threadpool*) arg;
    while (1) {
        pthread_mutex_lock(&pool->queue_lock);
        while (pool->queue_count == 0 && !atomic_load(&pool->shutdown)) {
            pthread_cond_wait(&pool->queue_not_empty, &pool->queue_lock);
        }
        if (pool->queue_count == 0 && atomic_load(&pool->shutdown)) {
            pthread_mutex_unlock(&pool->queue_lock);
            break;
        }
        threadpool_task task = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % THREADPOOL_QUEUE_SIZE;
        pool->queue_count--;
        pthread_cond_signal(&pool->queue_not_full);
        pthread_mutex_unlock(&pool->queue_lock);

        task.fn(task.arg);
    }
    return NULL;
}

threadpool* threadpool_init(int num_workers) {
    if (num_workers <= 0) {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        num_workers = (nproc > 0 && nproc < 8) ? (int) nproc : 8;
    }

    threadpool* pool = calloc(1, sizeof(threadpool));
    if (!pool)
        return NULL;

    pool->workers = calloc((size_t) num_workers, sizeof(pthread_t));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }

    pool->queue = calloc((size_t) THREADPOOL_QUEUE_SIZE, sizeof(threadpool_task));
    if (!pool->queue) {
        free(pool->workers);
        free(pool);
        return NULL;
    }

    pool->num_workers = num_workers;
    pool->queue_head = 0;
    pool->queue_tail = 0;
    pool->queue_count = 0;
    atomic_store(&pool->shutdown, false);

    pthread_mutex_init(&pool->queue_lock, NULL);
    pthread_cond_init(&pool->queue_not_empty, NULL);
    pthread_cond_init(&pool->queue_not_full, NULL);

    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_loop, pool) != 0) {
            atomic_store(&pool->shutdown, true);
            pthread_cond_broadcast(&pool->queue_not_empty);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->workers[j], NULL);
            }
            pthread_mutex_destroy(&pool->queue_lock);
            pthread_cond_destroy(&pool->queue_not_empty);
            pthread_cond_destroy(&pool->queue_not_full);
            free(pool->queue);
            free(pool->workers);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

int threadpool_submit(threadpool* pool, threadpool_task_fn fn, void* arg) {
    if (atomic_load(&pool->shutdown))
        return -1;

    pthread_mutex_lock(&pool->queue_lock);
    while (pool->queue_count == THREADPOOL_QUEUE_SIZE) {
        pthread_cond_wait(&pool->queue_not_full, &pool->queue_lock);
    }

    pool->queue[pool->queue_tail].fn = fn;
    pool->queue[pool->queue_tail].arg = arg;
    pool->queue_tail = (pool->queue_tail + 1) % THREADPOOL_QUEUE_SIZE;
    pool->queue_count++;

    pthread_cond_signal(&pool->queue_not_empty);
    pthread_mutex_unlock(&pool->queue_lock);

    return 0;
}

void threadpool_shutdown(threadpool* pool) {
    atomic_store(&pool->shutdown, true);

    pthread_mutex_lock(&pool->queue_lock);
    pthread_cond_broadcast(&pool->queue_not_empty);
    pthread_mutex_unlock(&pool->queue_lock);

    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->workers[i], NULL);
    }

    pthread_mutex_destroy(&pool->queue_lock);
    pthread_cond_destroy(&pool->queue_not_empty);
    pthread_cond_destroy(&pool->queue_not_full);

    free(pool->queue);
    free(pool->workers);
    free(pool);
}

int threadpool_queue_depth(const threadpool* pool) {
    return pool->queue_count;
}
