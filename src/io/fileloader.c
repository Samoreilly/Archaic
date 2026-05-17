#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

#include "fileloader.h"
#include "../threadmanager.h"
#include "../scanner.h"
#include "../metrics.h"
#include "../trie-storage.h"
#include "../lru.h"
#include "../../ipc/server.h"

void load_trie() {
}

void save_trie(Trie* trie) {
    (void)trie;
}

void daemon_save_state(daemon_state* state, const char* path) {
    if (!state || !state->store || !path) return;

    FILE* f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# archaic state file\n");
    fprintf(f, "# format: freq last_access is_dir path\n");

    store_lock(state->store);
    for (size_t i = 0; i < state->store->right_index; i++) {
        t_bucket* bucket = state->store->buckets[i];
        if (!bucket || !bucket->dir_trie) continue;

        trie_lock(bucket);
        fprintf(f, "bucket %s %u\n", bucket->dir_name, (unsigned)bucket->dir_count);
        trie_unlock(bucket);
    }
    store_unlock(state->store);

    fclose(f);
}

path_validation process_input(t_bucket_store* store, const char* cwd, const char* input) {
    path_validation validation = validate_input_path(cwd, input);

    if (!validation.exists || !validation.full_path) {
        return validation;
    }

    store_lock(store);
    t_bucket* bucket = find_bucket(store, validation.full_path, validation.full_path, 3, false);
    if (bucket) {
        trie_lock(bucket);
        insert(bucket->dir_trie, validation.full_path);
        bucket->dir_count++;
        trie_unlock(bucket);
    }
    store_unlock(store);

    return validation;
}

daemon_state* daemon_init(void) {
    daemon_state* state = (daemon_state*) calloc(1, sizeof(daemon_state));
    if (!state) {
        return NULL;
    }

    state->store = (t_bucket_store*) calloc(1, sizeof(t_bucket_store));
    if (!state->store) {
        free(state);
        return NULL;
    }

    pthread_mutex_init(&state->store->store_lock, NULL);

    state->parent = (struct node*) calloc(1, sizeof(struct node));
    if (!state->parent) {
        free(state->store);
        free(state);
        return NULL;
    }

    state->parent->is_parent = true;
    state->store->parent = state->parent;

    parallel_scanner_init(&state->scanner, state->store, state->parent, 10, 4);

    metrics_init(&state->metrics);

    return state;
}

void daemon_shutdown(daemon_state* state) {
    if (!state) {
        return;
    }

    if (state->ipc) {
        ipc_server_stop(state->ipc);
        state->ipc = NULL;
    }

    /* Wait for background scan to finish */
    if (atomic_load(&state->scanning)) {
        pthread_join(state->scan_thread, NULL);
    }

    parallel_scanner_stop(&state->scanner);
    if (state->scanner.queue) {
        pthread_mutex_destroy(&state->scanner.queue->queue_lock);
        pthread_cond_destroy(&state->scanner.queue->queue_not_empty);
        free(state->scanner.queue);
    }

    if (state->store) {
        for (size_t i = 0; i < state->store->right_index; i++) {
            t_bucket* bucket = state->store->buckets[i];
            if (bucket) {
                destroy_bucket(bucket);
            }
        }
        pthread_mutex_destroy(&state->store->store_lock);
        free(state->store);
    }

    if (state->parent) {
        free(state->parent);
    }

    free(state);
}

typedef struct {
    daemon_state* state;
    char* path;
} scan_thread_ctx;

static void* scan_thread_func(void* arg) {
    scan_thread_ctx* ctx = (scan_thread_ctx*)arg;
    daemon_state* state = ctx->state;
    char* path = ctx->path;
    free(ctx);

    atomic_store(&state->scanning, true);
    atomic_store(&state->scan_bucket_count, 0);

    metrics_record_scan(&state->metrics);

    parallel_scanner_start(&state->scanner, path);
    parallel_scanner_wait(&state->scanner);

    atomic_store(&state->scan_bucket_count, state->store->right_index);
    atomic_store(&state->scanning, false);

    free((char*)path);
    return NULL;
}

void daemon_run_scan(daemon_state* state, const char* path) {
    if (!state || !path) return;

    /* If a scan is already running, don't start another */
    if (atomic_load(&state->scanning)) return;

    scan_thread_ctx* ctx = malloc(sizeof(scan_thread_ctx));
    if (!ctx) return;
    ctx->state = state;
    ctx->path = strdup(path);
    if (!ctx->path) { free(ctx); return; }

    if (pthread_create(&state->scan_thread, NULL, scan_thread_func, ctx) != 0) {
        free(ctx->path);
        free(ctx);
    }
}

scan_status daemon_scan_status(daemon_state* state) {
    scan_status s = {0};
    if (!state) return s;
    s.scanning = atomic_load(&state->scanning);
    s.buckets_so_far = atomic_load(&state->scan_bucket_count);
    if (s.scanning && state->store) {
        s.buckets_so_far = state->store->right_index;
    }
    return s;
}

path_validation daemon_process_query(daemon_state* state, const char* cwd, const char* input) {
    if (!state || !state->store) {
        path_validation empty = {0};
        return empty;
    }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    path_validation result = process_input(state->store, cwd, input);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    uint64_t latency_ns = (uint64_t)(ts_end.tv_sec - ts_start.tv_sec) * 1000000000ULL
                        + (uint64_t)(ts_end.tv_nsec - ts_start.tv_nsec);
    metrics_record_query(&state->metrics, latency_ns);

    return result;
}

completions* daemon_get_completions(daemon_state* state, const char* prefix, size_t limit) {
    if (!state || !state->store || !prefix) {
        return NULL;
    }

    metrics_record_completion(&state->metrics);

    completions* out = completions_create(limit > 0 ? limit : 50);
    if (!out) return NULL;

    /* Step 1: Snapshot bucket pointers under store_lock, increment refcounts */
    store_lock(state->store);
    size_t count = state->store->right_index;
    t_bucket** snapshot = (t_bucket**) malloc(count * sizeof(t_bucket*));
    if (!snapshot) {
        store_unlock(state->store);
        completions_free(out);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        t_bucket* bucket = state->store->buckets[i];
        if (bucket && bucket->dir_trie) {
            atomic_fetch_add(&bucket->refcount, 1);
            snapshot[i] = bucket;
        } else {
            snapshot[i] = NULL;
        }
    }
    store_unlock(state->store);

    /* Step 2: Iterate snapshot without store_lock */
    for (size_t i = 0; i < count && out->count < out->capacity; i++) {
        t_bucket* bucket = snapshot[i];
        if (!bucket) continue;

        trie_lock(bucket);
        completions_collect(bucket->dir_trie, prefix, out);
        trie_unlock(bucket);
        bucket_release(bucket);
    }

    free(snapshot);
    return out;
}

scored_completions* daemon_get_scored_completions(daemon_state* state, const char* prefix, size_t limit, uint64_t now) {
    if (!state || !state->store || !prefix) {
        return NULL;
    }

    metrics_record_completion(&state->metrics);

    scored_completions* out = scored_completions_create(limit > 0 ? limit : 50);
    if (!out) return NULL;

    store_lock(state->store);
    size_t count = state->store->right_index;
    t_bucket** snapshot = (t_bucket**) malloc(count * sizeof(t_bucket*));
    if (!snapshot) {
        store_unlock(state->store);
        scored_completions_free(out);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        t_bucket* bucket = state->store->buckets[i];
        if (bucket && bucket->dir_trie) {
            atomic_fetch_add(&bucket->refcount, 1);
            snapshot[i] = bucket;
        } else {
            snapshot[i] = NULL;
        }
    }
    store_unlock(state->store);

    for (size_t i = 0; i < count && out->count < out->capacity; i++) {
        t_bucket* bucket = snapshot[i];
        if (!bucket) continue;

        trie_lock(bucket);
        scored_completions_collect(bucket->dir_trie, prefix, out, now);
        trie_unlock(bucket);
        bucket_release(bucket);
    }

    free(snapshot);
    return out;
}

int daemon_start_ipc(daemon_state* state, const char* sock_path) {
    if (!state) return -1;
    state->ipc = ipc_server_start(state, sock_path);
    return state->ipc ? 0 : -1;
}
