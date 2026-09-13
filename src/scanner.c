#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ignore-file.h"
#include "log.h"
#include "scanner.h"
#include "threadmanager.h"
#include "trie-storage.h"
#include "trie.h"

static int should_ignore_dir(parallel_scanner* scanner, const char* name) {
    if (hashset_contains(&scanner->ignore_dir_set, name))
        return 1;
    for (int i = 0; i < scanner->ignore_dir_count; i++) {
        if (fnmatch(scanner->ignore_dirs[i], name, 0) == 0)
            return 1;
    }
    return 0;
}

static int should_ignore_file(parallel_scanner* scanner, const char* name) {
    if (hashset_contains(&scanner->ignore_file_exact_set, name))
        return 1;
    for (int i = 0; i < scanner->ignore_file_count; i++) {
        if (fnmatch(scanner->ignore_files[i], name, 0) == 0)
            return 1;
    }
    return 0;
}

/* Per-root policy helpers: root_idx comes from the queue item (-1 = none). */
static int scanner_max_depth_for(parallel_scanner* scanner, int root_idx) {
    if (scanner && root_idx >= 0 && root_idx < SCANNER_MAX_ROOTPOLICY &&
        scanner->root_max_depths[root_idx] >= 0)
        return scanner->root_max_depths[root_idx];
    return scanner ? scanner->max_depth : 0;
}

static int scanner_root_ignores_dir(parallel_scanner* scanner, int root_idx, const char* name) {
    if (!scanner || !name || root_idx < 0 || root_idx >= SCANNER_MAX_ROOTPOLICY)
        return 0;
    for (int i = 0; i < scanner->root_ignore_dir_counts[root_idx]; i++) {
        if (fnmatch(scanner->root_ignore_dirs[root_idx][i], name, 0) == 0)
            return 1;
    }
    return 0;
}

static int should_ignore_dir_local(const ignore_file* local, const char* name) {
    if (!local || local->count == 0)
        return 0;
    return ignore_file_should_ignore(local, name, true) ? 1 : 0;
}

static int should_ignore_file_local(const ignore_file* local, const char* name) {
    if (!local || local->count == 0)
        return 0;
    return ignore_file_should_ignore(local, name, false) ? 1 : 0;
}

void scan_queue_init(scan_queue* q) {
    q->queue_head = 0;
    q->queue_tail = 0;
    q->queue_count = 0;
    pthread_mutex_init(&q->queue_lock, NULL);
    pthread_cond_init(&q->queue_not_empty, NULL);
    pthread_cond_init(&q->queue_not_full, NULL);
}

int scan_queue_push(scan_queue* q, const char* path, int depth, int root_idx) {
    if (!q || !path)
        return -1;
    char* copy = strdup(path);
    if (!copy)
        return -1;
    pthread_mutex_lock(&q->queue_lock);
    while (q->queue_count >= SCANNER_QUEUE_SIZE)
        pthread_cond_wait(&q->queue_not_full, &q->queue_lock);
    scan_work_item* item = &q->queue[q->queue_tail];
    item->path = copy;
    item->depth = depth;
    item->root_idx = root_idx;
    q->queue_tail = (q->queue_tail + 1) % SCANNER_QUEUE_SIZE;
    q->queue_count++;
    pthread_cond_signal(&q->queue_not_empty);
    pthread_mutex_unlock(&q->queue_lock);
    return 0;
}

typedef struct {
    dev_t dev;
    ino_t ino;
} dev_ino_pair;

#define SYMLINK_DEDUP_MAX 65536

typedef struct {
    dev_ino_pair entries[SYMLINK_DEDUP_MAX];
    int count;
    pthread_mutex_t lock;
} symlink_dedup_set;

static void symlink_dedup_reset(symlink_dedup_set* set) {
    pthread_mutex_lock(&set->lock);
    set->count = 0;
    pthread_mutex_unlock(&set->lock);
}

static int symlink_dedup_check_and_add(symlink_dedup_set* set, dev_t dev, ino_t ino) {
    pthread_mutex_lock(&set->lock);
    for (int i = 0; i < set->count; i++) {
        if (set->entries[i].dev == dev && set->entries[i].ino == ino) {
            pthread_mutex_unlock(&set->lock);
            return 1;
        }
    }
    if (set->count < SYMLINK_DEDUP_MAX) {
        set->entries[set->count].dev = dev;
        set->entries[set->count].ino = ino;
        set->count++;
    }
    pthread_mutex_unlock(&set->lock);
    return 0;
}

static symlink_dedup_set g_symlink_seen = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void* scanner_worker(void* arg) {
    parallel_scanner* scanner = (parallel_scanner*) arg;

    while (1) {
        char path[4096];
        int depth;
        int root_idx = -1;

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
        root_idx = item->root_idx;
        free(item->path);
        item->path = NULL;
        scanner->queue->queue_head = (scanner->queue->queue_head + 1) % SCANNER_QUEUE_SIZE;
        scanner->queue->queue_count--;
        pthread_cond_signal(&scanner->queue->queue_not_full);

        atomic_fetch_add(&scanner->active_workers, 1);
        pthread_mutex_unlock(&scanner->queue->queue_lock);

        struct timespec dir_start;
        clock_gettime(CLOCK_MONOTONIC, &dir_start);

        DIR* dir = opendir(path);
        if (!dir) {
            if (errno == EACCES) {
                LOG_WARN("scanner", "permission denied: %s (check directory permissions)", path);
                atomic_fetch_add(&scanner->skipped_dirs, 1);
            } else if (errno == ENOENT) {
                LOG_WARN("scanner", "directory removed during scan: %s", path);
            } else if (errno == ENOTDIR) {
                LOG_WARN("scanner", "not a directory: %s", path);
            } else if (errno == EMFILE || errno == ENFILE) {
                LOG_ERR("scanner", "too many open files, cannot open: %s (errno=%d)", path, errno);
            } else {
                LOG_WARN("scanner", "cannot open directory: %s (errno=%d)", path, errno);
            }
            int prev = atomic_fetch_sub(&scanner->active_workers, 1);
            if (prev == 1) {
                pthread_mutex_lock(&scanner->queue->queue_lock);
                pthread_cond_broadcast(&scanner->queue->queue_not_empty);
                pthread_mutex_unlock(&scanner->queue->queue_lock);
            }
            continue;
        }

        typedef struct {
            char name[256];
            bool is_dir;
        } scan_ent;
        size_t entries_cap = 256;
        scan_ent* entries = malloc(entries_cap * sizeof(scan_ent));
        if (!entries) {
            closedir(dir);
            int prev = atomic_fetch_sub(&scanner->active_workers, 1);
            if (prev == 1) {
                pthread_mutex_lock(&scanner->queue->queue_lock);
                pthread_cond_broadcast(&scanner->queue->queue_not_empty);
                pthread_mutex_unlock(&scanner->queue->queue_lock);
            }
            continue;
        }
        int entry_count = 0;

        ignore_file local_ignore;
        ignore_file_clear(&local_ignore);
        char ignore_path[4096];
        snprintf(ignore_path, sizeof(ignore_path), "%s/.gitignore", path);
        ignore_file_load(&local_ignore, ignore_path);
        snprintf(ignore_path, sizeof(ignore_path), "%s/.archaicignore", path);
        ignore_file_load(&local_ignore, ignore_path);

        struct dirent* entry;
        while ((entry = readdir(dir))) {
            if (scanner->dir_timeout_ms > 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - dir_start.tv_sec) * 1000 +
                                  (now.tv_nsec - dir_start.tv_nsec) / 1000000;
                if (elapsed_ms > scanner->dir_timeout_ms) {
                    LOG_WARN("scanner", "directory scan timeout (%dms): %s (%d entries read)",
                             scanner->dir_timeout_ms, path, entry_count);
                    atomic_fetch_add(&scanner->skipped_dirs, 1);
                    break;
                }
            }

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            bool is_dir = (entry->d_type == DT_DIR);
            bool is_symlink = false;
            if (entry->d_type == DT_LNK) {
                is_symlink = true;
                char child_path[4096];
                snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
                struct stat target_st;
                if (stat(child_path, &target_st) == 0) {
                    is_dir = S_ISDIR(target_st.st_mode);
                    if (is_dir) {
                        if (symlink_dedup_check_and_add(&g_symlink_seen, target_st.st_dev,
                                                        target_st.st_ino)) {
                            continue;
                        }
                    }
                } else {
                    continue;
                }
            } else if (!is_dir && entry->d_type == DT_UNKNOWN) {
                char child_path[4096];
                snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
                struct stat st;
                if (lstat(child_path, &st) == 0) {
                    if (S_ISLNK(st.st_mode)) {
                        is_symlink = true;
                        struct stat target_st;
                        if (stat(child_path, &target_st) == 0) {
                            is_dir = S_ISDIR(target_st.st_mode);
                            if (is_dir) {
                                if (symlink_dedup_check_and_add(&g_symlink_seen, target_st.st_dev,
                                                                target_st.st_ino)) {
                                    continue;
                                }
                            }
                        } else {
                            continue;
                        }
                    } else {
                        is_dir = S_ISDIR(st.st_mode);
                    }
                }
            }

            (void) is_symlink;
            if (is_dir && should_ignore_dir(scanner, entry->d_name))
                continue;
            if (is_dir && scanner_root_ignores_dir(scanner, root_idx, entry->d_name))
                continue;
            if (!is_dir && should_ignore_file(scanner, entry->d_name))
                continue;
            if (is_dir && should_ignore_dir_local(&local_ignore, entry->d_name))
                continue;
            if (!is_dir && should_ignore_file_local(&local_ignore, entry->d_name))
                continue;

            if ((size_t) entry_count >= entries_cap) {
                size_t ncap = entries_cap * 2;
                scan_ent* nent = realloc(entries, ncap * sizeof(scan_ent));
                if (!nent)
                    break;
                entries = nent;
                entries_cap = ncap;
            }
            strncpy(entries[entry_count].name, entry->d_name, 255);
            entries[entry_count].name[255] = '\0';
            entries[entry_count].is_dir = is_dir;
            entry_count++;
        }
        closedir(dir);

        int dirs = atomic_fetch_add(&scanner->dirs_scanned, 1) + 1;
        atomic_fetch_add(&scanner->files_scanned, entry_count);

        int last_log = atomic_load(&scanner->last_progress_log);
        int now = (int) time(NULL);
        if (now - last_log >= 5) {
            if (atomic_compare_exchange_strong(&scanner->last_progress_log, &last_log, now)) {
                int files = atomic_load(&scanner->files_scanned);
                LOG_INFO("scanner", "progress: %d dirs, %d files indexed", dirs, files);
                /* Refresh the heap estimate on the same throttle so the
                 * budget check below reads a live value, not a stale one. */
                update_memory_estimate(scanner->lfu);
            }
        }

        if (scanner->lfu->max_memory_bytes > 0 &&
            atomic_load(&scanner->lfu->estimated_memory_bytes) > scanner->lfu->max_memory_bytes) {
            atomic_store(&scanner->stop, true);
            LOG_WARN("scanner", "stopping scan: memory budget exceeded");
        }

        for (int i = 0; i < entry_count; i++) {
            if (atomic_load(&scanner->stop))
                break;

            char child_path[4096];
            if (strlen(path) + 1 + strlen(entries[i].name) + 2 >= sizeof(child_path))
                continue;
            snprintf(child_path, sizeof(child_path), "%s/%s", path, entries[i].name);

            store_lock(scanner->lfu);
            t_bucket* bucket = find_bucket(scanner->lfu, child_path, child_path, 3, false);
            if (bucket && bucket->dir_count >= scanner->lfu->max_nodes_per_bucket)
                bucket = NULL;
            else if (bucket)
                atomic_fetch_add(&bucket->refcount, 1);
            store_unlock(scanner->lfu);

            if (bucket) {
                trie_lock(bucket);
                if (entries[i].is_dir) {
                    size_t path_len = strlen(child_path);
                    if (path_len + 2 > sizeof(child_path)) {
                        trie_unlock(bucket);
                        bucket_release(bucket);
                        continue;
                    }
                    char dir_path[4096];
                    memcpy(dir_path, child_path, path_len);
                    dir_path[path_len] = '/';
                    dir_path[path_len + 1] = '\0';
                    insert(bucket->dir_trie, dir_path);
                } else {
                    insert(bucket->dir_trie, child_path);
                }
                bucket->dir_count++;
                atomic_fetch_add(&scanner->lfu->total_nodes, 1);
                trie_unlock(bucket);
                bucket_release(bucket);
            }

            if (entries[i].is_dir && depth < scanner_max_depth_for(scanner, root_idx)) {
                scan_queue_push(scanner->queue, child_path, depth + 1, root_idx);
            }
        }
        free(entries);

        int prev = atomic_fetch_sub(&scanner->active_workers, 1);
        if (prev == 1) {
            pthread_mutex_lock(&scanner->queue->queue_lock);
            pthread_cond_broadcast(&scanner->queue->queue_not_empty);
            pthread_mutex_unlock(&scanner->queue->queue_lock);
        }
    }
    return NULL;
}

void parallel_scanner_init(parallel_scanner* scanner, t_bucket_store* store, struct node* parent,
                           int max_depth, int num_threads) {
    memset(scanner, 0, sizeof(parallel_scanner));
    scanner->queue = (scan_queue*) calloc(1, sizeof(scan_queue));
    scan_queue_init(scanner->queue);
    scanner->num_threads = num_threads > SCANNER_MAX_THREADS ? SCANNER_MAX_THREADS : num_threads;
    scanner->max_depth = max_depth;
    for (int i = 0; i < SCANNER_MAX_ROOTPOLICY; i++)
        scanner->root_max_depths[i] = -1;
    scanner->lfu = store;
    scanner->parent = parent;
    atomic_store(&scanner->stop, false);
    atomic_store(&scanner->active_workers, 0);
    atomic_store(&scanner->threads_started, false);
    atomic_store(&scanner->threads_joined, false);
    atomic_store(&scanner->dirs_scanned, 0);
    atomic_store(&scanner->files_scanned, 0);
    atomic_store(&scanner->last_progress_log, 0);
}

void parallel_scanner_set_dir_timeout(parallel_scanner* scanner, int timeout_ms) {
    if (scanner)
        scanner->dir_timeout_ms = timeout_ms > 0 ? timeout_ms : 0;
}

void parallel_scanner_set_ignores(parallel_scanner* scanner, const char** dirs, int dir_count,
                                  const char** files, int file_count) {
    scanner->ignore_dir_count = 0;
    scanner->ignore_file_count = 0;

    hashset_free(&scanner->ignore_dir_set);
    hashset_free(&scanner->ignore_file_exact_set);
    hashset_init(&scanner->ignore_dir_set, (size_t) (dir_count > 0 ? dir_count * 2 : 16));
    hashset_init(&scanner->ignore_file_exact_set, (size_t) (file_count > 0 ? file_count * 2 : 16));

    for (int i = 0; i < dir_count && i < SCANNER_MAX_IGNORE; i++) {
        strncpy(scanner->ignore_dirs[i], dirs[i], SCANNER_MAX_IGNORE_LEN - 1);
        scanner->ignore_dirs[i][SCANNER_MAX_IGNORE_LEN - 1] = '\0';
        scanner->ignore_dir_count++;
        hashset_insert(&scanner->ignore_dir_set, dirs[i]);
    }
    for (int i = 0; i < file_count && i < SCANNER_MAX_IGNORE; i++) {
        strncpy(scanner->ignore_files[i], files[i], SCANNER_MAX_IGNORE_LEN - 1);
        scanner->ignore_files[i][SCANNER_MAX_IGNORE_LEN - 1] = '\0';
        scanner->ignore_file_count++;
        if (!strchr(files[i], '*') && !strchr(files[i], '?') && !strchr(files[i], '[')) {
            hashset_insert(&scanner->ignore_file_exact_set, files[i]);
        }
    }
}

void parallel_scanner_start(parallel_scanner* scanner, const char* root_path) {
    atomic_store(&scanner->stop, false);
    atomic_store(&scanner->active_workers, 0);
    atomic_store(&scanner->threads_joined, false);
    symlink_dedup_reset(&g_symlink_seen);

    scan_queue_push(scanner->queue, root_path, 0, -1);

    for (int i = 0; i < scanner->num_threads; i++) {
        pthread_create(&scanner->workers[i], NULL, scanner_worker, (void*) scanner);
    }
}

void parallel_scanner_start_multi(parallel_scanner* scanner, const char** roots, int root_count) {
    parallel_scanner_start_multi_depth(scanner, roots, NULL, root_count);
}

void parallel_scanner_start_multi_depth(parallel_scanner* scanner, const char** roots,
                                        const int* depths, int root_count) {
    atomic_store(&scanner->stop, false);
    atomic_store(&scanner->active_workers, 0);
    atomic_store(&scanner->threads_joined, false);
    symlink_dedup_reset(&g_symlink_seen);

    for (int i = 0; i < SCANNER_MAX_ROOTPOLICY; i++)
        scanner->root_max_depths[i] = -1;
    if (root_count > SCANNER_MAX_ROOTPOLICY)
        root_count = SCANNER_MAX_ROOTPOLICY;
    for (int i = 0; i < root_count; i++) {
        if (depths)
            scanner->root_max_depths[i] = depths[i];
        scan_queue_push(scanner->queue, roots[i], 0, i);
    }

    for (int i = 0; i < scanner->num_threads; i++) {
        pthread_create(&scanner->workers[i], NULL, scanner_worker, (void*) scanner);
    }
}

void parallel_scanner_set_root_ignores(parallel_scanner* scanner, int root_idx,
                                       const char** dirs, int dir_count) {
    if (!scanner || root_idx < 0 || root_idx >= SCANNER_MAX_ROOTPOLICY)
        return;
    scanner->root_ignore_dir_counts[root_idx] = 0;
    if (!dirs)
        return;
    for (int i = 0; i < dir_count && i < SCANNER_MAX_ROOT_IGNORE; i++) {
        if (!dirs[i])
            continue;
        strncpy(scanner->root_ignore_dirs[root_idx][i], dirs[i], SCANNER_MAX_IGNORE_LEN - 1);
        scanner->root_ignore_dirs[root_idx][i][SCANNER_MAX_IGNORE_LEN - 1] = '\0';
        scanner->root_ignore_dir_counts[root_idx]++;
    }
}

void parallel_scanner_wait(parallel_scanner* scanner) {
    for (int i = 0; i < scanner->num_threads; i++) {
        pthread_join(scanner->workers[i], NULL);
        scanner->workers[i] = (pthread_t) 0;
    }
}

void parallel_scanner_stop(parallel_scanner* scanner) {
    atomic_store(&scanner->stop, true);
    pthread_mutex_lock(&scanner->queue->queue_lock);
    pthread_cond_broadcast(&scanner->queue->queue_not_empty);
    pthread_cond_broadcast(&scanner->queue->queue_not_full);
    pthread_mutex_unlock(&scanner->queue->queue_lock);
    for (int i = 0; i < scanner->num_threads; i++) {
        if (scanner->workers[i]) {
            pthread_join(scanner->workers[i], NULL);
            scanner->workers[i] = (pthread_t) 0;
        }
    }
}
