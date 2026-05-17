#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <unistd.h>

#include "scanner.h"
#include "threadmanager.h"
#include "trie-storage.h"
#include "trie.h"

void scan_queue_init(scan_queue* q) {
    q->queue_head = 0;
    q->queue_tail = 0;
    q->queue_count = 0;
    pthread_mutex_init(&q->queue_lock, NULL);
    pthread_cond_init(&q->queue_not_empty, NULL);
}

int scan_queue_push(scan_queue* q, const char* path, int depth) {
    pthread_mutex_lock(&q->queue_lock);
    if (q->queue_count >= SCANNER_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->queue_lock);
        return -1;
    }
    scan_work_item* item = &q->queue[q->queue_tail];
    item->path = strdup(path);
    item->depth = depth;
    q->queue_tail = (q->queue_tail + 1) % SCANNER_QUEUE_SIZE;
    q->queue_count++;
    pthread_cond_signal(&q->queue_not_empty);
    pthread_mutex_unlock(&q->queue_lock);
    return 0;
}

int scan_queue_pop(scan_queue* q, char* path_out, int* depth_out, size_t path_cap) {
    pthread_mutex_lock(&q->queue_lock);
    if (q->queue_count == 0) {
        pthread_mutex_unlock(&q->queue_lock);
        return -1;
    }
    scan_work_item* item = &q->queue[q->queue_head];
    strncpy(path_out, item->path, path_cap - 1);
    path_out[path_cap - 1] = '\0';
    *depth_out = item->depth;
    free(item->path);
    item->path = NULL;
    q->queue_head = (q->queue_head + 1) % SCANNER_QUEUE_SIZE;
    q->queue_count--;
    pthread_mutex_unlock(&q->queue_lock);
    return 0;
}

static void* scanner_worker(void* arg) {
    parallel_scanner* scanner = (parallel_scanner*)arg;

    while (1) {
        char path[4096];
        int depth;

        pthread_mutex_lock(&scanner->queue->queue_lock);
        while (scanner->queue->queue_count == 0) {
            if (atomic_load(&scanner->stop)) {
                pthread_mutex_unlock(&scanner->queue->queue_lock);
                return NULL;
            }
            if (atomic_load(&scanner->active_workers) == 0) {
                pthread_mutex_unlock(&scanner->queue->queue_lock);
                return NULL;
            }
            pthread_cond_wait(&scanner->queue->queue_not_empty, &scanner->queue->queue_lock);
        }

        scan_work_item* item = &scanner->queue->queue[scanner->queue->queue_head];
        strncpy(path, item->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        depth = item->depth;
        free(item->path);
        item->path = NULL;
        scanner->queue->queue_head = (scanner->queue->queue_head + 1) % SCANNER_QUEUE_SIZE;
        scanner->queue->queue_count--;

        atomic_fetch_add(&scanner->active_workers, 1);
        pthread_mutex_unlock(&scanner->queue->queue_lock);

        DIR* dir = opendir(path);
        if (!dir) {
            int prev = atomic_fetch_sub(&scanner->active_workers, 1);
            if (prev == 1) {
                pthread_mutex_lock(&scanner->queue->queue_lock);
                pthread_cond_broadcast(&scanner->queue->queue_not_empty);
                pthread_mutex_unlock(&scanner->queue->queue_lock);
            }
            continue;
        }

        struct {
            char name[256];
            bool is_dir;
        } entries[512];
        int entry_count = 0;

        struct dirent* entry;
        while ((entry = readdir(dir)) && entry_count < 512) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            strncpy(entries[entry_count].name, entry->d_name, 255);
            entries[entry_count].name[255] = '\0';

            bool is_dir = (entry->d_type == DT_DIR);
            if (!is_dir && entry->d_type == DT_UNKNOWN) {
                char child_path[4096];
                snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
                struct stat st;
                if (stat(child_path, &st) == 0) {
                    is_dir = S_ISDIR(st.st_mode);
                }
            }
            entries[entry_count].is_dir = is_dir;
            entry_count++;
        }
        closedir(dir);

        for (int i = 0; i < entry_count; i++) {
            if (atomic_load(&scanner->stop)) break;

            char child_path[4096];
            snprintf(child_path, sizeof(child_path), "%s/%s", path, entries[i].name);

            store_lock(scanner->lfu);
            t_bucket* bucket = find_bucket(scanner->lfu, child_path, child_path, 3, false);
            if (bucket) {
                trie_lock(bucket);
                if (entries[i].is_dir) {
                    size_t path_len = strlen(child_path);
                    char dir_path[4096];
                    memcpy(dir_path, child_path, path_len);
                    dir_path[path_len] = '/';
                    dir_path[path_len + 1] = '\0';
                    insert(bucket->dir_trie, dir_path);
                } else {
                    insert(bucket->dir_trie, child_path);
                }
                bucket->dir_count++;
                trie_unlock(bucket);
            }
            store_unlock(scanner->lfu);

            if (entries[i].is_dir && depth < scanner->max_depth) {
                scan_queue_push(scanner->queue, child_path, depth + 1);
            }
        }

        int prev = atomic_fetch_sub(&scanner->active_workers, 1);
        if (prev == 1) {
            pthread_mutex_lock(&scanner->queue->queue_lock);
            pthread_cond_broadcast(&scanner->queue->queue_not_empty);
            pthread_mutex_unlock(&scanner->queue->queue_lock);
        }
    }
    return NULL;
}

void parallel_scanner_init(parallel_scanner* scanner, t_bucket_store* store, struct node* parent, int max_depth, int num_threads) {
    memset(scanner, 0, sizeof(parallel_scanner));
    scanner->queue = (scan_queue*)calloc(1, sizeof(scan_queue));
    scan_queue_init(scanner->queue);
    scanner->num_threads = num_threads > SCANNER_MAX_THREADS ? SCANNER_MAX_THREADS : num_threads;
    scanner->max_depth = max_depth;
    scanner->lfu = store;
    scanner->parent = parent;
    atomic_store(&scanner->stop, false);
    atomic_store(&scanner->active_workers, 0);
    atomic_store(&scanner->threads_started, false);
    atomic_store(&scanner->threads_joined, false);
}

void parallel_scanner_start(parallel_scanner* scanner, const char* root_path) {
    atomic_store(&scanner->stop, false);
    atomic_store(&scanner->active_workers, 0);
    atomic_store(&scanner->threads_joined, false);

    scan_queue_push(scanner->queue, root_path, 0);

    for (int i = 0; i < scanner->num_threads; i++) {
        pthread_create(&scanner->workers[i], NULL, scanner_worker, (void*)scanner);
    }
}

void parallel_scanner_wait(parallel_scanner* scanner) {
    for (int i = 0; i < scanner->num_threads; i++) {
        pthread_join(scanner->workers[i], NULL);
        scanner->workers[i] = (pthread_t)0;
    }
}

void parallel_scanner_stop(parallel_scanner* scanner) {
    atomic_store(&scanner->stop, true);
    pthread_mutex_lock(&scanner->queue->queue_lock);
    pthread_cond_broadcast(&scanner->queue->queue_not_empty);
    pthread_mutex_unlock(&scanner->queue->queue_lock);
    for (int i = 0; i < scanner->num_threads; i++) {
        if (scanner->workers[i]) {
            pthread_join(scanner->workers[i], NULL);
            scanner->workers[i] = (pthread_t)0;
        }
    }
}
