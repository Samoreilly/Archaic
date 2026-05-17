#pragma once
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>

struct t_bucket_store;
struct node;

typedef struct {
    pthread_t worker;
    atomic_bool stop;
    atomic_bool running;
} state;

#define SCANNER_MAX_THREADS 8
#define SCANNER_QUEUE_SIZE 1024
#define SCANNER_MAX_IGNORE 64
#define SCANNER_MAX_IGNORE_LEN 128

typedef struct {
    char* path;
    int depth;
} scan_work_item;

typedef struct {
    scan_work_item queue[SCANNER_QUEUE_SIZE];
    int queue_head;
    int queue_tail;
    int queue_count;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_not_empty;
} scan_queue;

typedef struct {
    pthread_t workers[SCANNER_MAX_THREADS];
    int num_threads;
    atomic_bool stop;
    atomic_int active_workers;
    atomic_bool threads_started;
    atomic_bool threads_joined;

    scan_queue* queue;
    struct t_bucket_store* lfu;
    struct node* parent;
    int max_depth;

    char ignore_dirs[SCANNER_MAX_IGNORE][SCANNER_MAX_IGNORE_LEN];
    int ignore_dir_count;
    char ignore_files[SCANNER_MAX_IGNORE][SCANNER_MAX_IGNORE_LEN];
    int ignore_file_count;
} parallel_scanner;
