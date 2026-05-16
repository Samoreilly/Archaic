#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "fileloader.h"
#include "../threadmanager.h"
#include "../trie-storage.h"
#include "../lru.h"
#include "../../ipc/server.h"

void load_trie() {

}

void save_trie(Trie* trie) {
    (void)trie;
}

static int collect_paths_cb(Trie* node, char* buffer, size_t depth, FILE* out) {
    (void)node;
    (void)buffer;
    (void)depth;
    (void)out;
    return 0;
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
        /* Walk the trie and emit entries */
        /* For now, just emit bucket-level metadata */
        fprintf(f, "bucket %s %zu\n", bucket->dir_name, bucket->dir_count);
        trie_unlock(bucket);
    }
    store_unlock(state->store);

    fclose(f);
}

void spin_scan_thread(file_thread* f_thread, char* path) {
    //if another thread is running, wait until completed
    if(f_thread->running) {
        f_thread->stop = true;
        pthread_join(f_thread->worker, NULL);
        f_thread->running = false;
    }

    f_thread->running = true;
    f_thread->path = path;
    pthread_create(&f_thread->worker, NULL, scan_curr_dir, (void*) f_thread);
}

static void scan_dir_recursive(file_thread* f_thread, const char* base_path, int depth, int max_depth) {
    DIR* dir = opendir(base_path);

    if (!dir) {
        return;
    }

    struct dirent* entry;

    while ((entry = readdir(dir))) {
        if (f_thread->stop) {
            closedir(dir);
            return;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t len = strlen(base_path) + 1 + strlen(entry->d_name) + 1;
        char* path = (char*) malloc(len + 1);
        if (!path) {
            continue;
        }

        snprintf(path, len, "%s/%s", base_path, entry->d_name);

        if (entry->d_type == DT_REG || entry->d_type == DT_DIR) {
            store_lock(f_thread->lfu);
            t_bucket* bucket = find_bucket(f_thread->lfu, path, path, 3, false);
            if (bucket) {
                trie_lock(bucket);
                if (entry->d_type == DT_DIR) {
                    size_t path_len = strlen(path);
                    char* dir_path = malloc(path_len + 2);
                    memcpy(dir_path, path, path_len);
                    dir_path[path_len] = '/';
                    dir_path[path_len + 1] = '\0';
                    insert(bucket->dir_trie, dir_path);
                    free(dir_path);
                } else {
                    insert(bucket->dir_trie, path);
                }
                bucket->dir_count++;
                trie_unlock(bucket);
            }
            store_unlock(f_thread->lfu);
        }

        if (entry->d_type == DT_DIR && depth < max_depth) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                scan_dir_recursive(f_thread, path, depth + 1, max_depth);
            }
        }

        free(path);
    }

    closedir(dir);
}

void* scan_curr_dir(void* args) {
    file_thread* f_thread = (file_thread*) args;
    scan_dir_recursive(f_thread, f_thread->path, 0, 10);
    return NULL;
}

/*
    NOTE: Once IPC is implemented, this will be used to verify input from terminal-bound process in main()
*/
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

/*
    DAEMON STATE LIFECYCLE
*/

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

    state->scanner = (file_thread*) calloc(1, sizeof(file_thread));
    if (!state->scanner) {
        free(state->parent);
        free(state->store);
        free(state);
        return NULL;
    }

    state->scanner->lfu = state->store;
    state->scanner->parent = state->parent;

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

    if (state->scanner) {
        if (state->scanner->running) {
            state->scanner->stop = true;
            pthread_join(state->scanner->worker, NULL);
        }
        free(state->scanner);
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

void daemon_run_scan(daemon_state* state, const char* path) {
    if (!state || !state->scanner || !path) {
        return;
    }

    char* norm = normalise_dir(path);
    state->scanner->path = norm;
    spin_scan_thread(state->scanner, norm);
    pthread_join(state->scanner->worker, NULL);
    state->scanner->running = false;
    state->scanner->path = NULL;
    free(norm);
}

path_validation daemon_process_query(daemon_state* state, const char* cwd, const char* input) {
    if (!state || !state->store) {
        path_validation empty = {0};
        return empty;
    }

    return process_input(state->store, cwd, input);
}

completions* daemon_get_completions(daemon_state* state, const char* prefix, size_t limit) {
    if (!state || !state->store || !prefix) {
        return NULL;
    }

    completions* out = completions_create(limit > 0 ? limit : 50);
    if (!out) return NULL;

    store_lock(state->store);
    for (size_t i = 0; i < state->store->right_index && out->count < out->capacity; i++) {
        t_bucket* bucket = state->store->buckets[i];
        if (!bucket || !bucket->dir_trie) continue;

        trie_lock(bucket);
        completions_collect(bucket->dir_trie, prefix, out);
        trie_unlock(bucket);
    }
    store_unlock(state->store);

    return out;
}

scored_completions* daemon_get_scored_completions(daemon_state* state, const char* prefix, size_t limit, uint64_t now) {
    if (!state || !state->store || !prefix) {
        return NULL;
    }

    scored_completions* out = scored_completions_create(limit > 0 ? limit : 50);
    if (!out) return NULL;

    store_lock(state->store);
    for (size_t i = 0; i < state->store->right_index && out->count < out->capacity; i++) {
        t_bucket* bucket = state->store->buckets[i];
        if (!bucket || !bucket->dir_trie) continue;

        trie_lock(bucket);
        scored_completions_collect(bucket->dir_trie, prefix, out, now);
        trie_unlock(bucket);
    }
    store_unlock(state->store);

    return out;
}

int daemon_start_ipc(daemon_state* state, const char* sock_path) {
    if (!state) return -1;
    state->ipc = ipc_server_start(state, sock_path);
    return state->ipc ? 0 : -1;
}

