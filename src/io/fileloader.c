#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "fileloader.h"
#include "../threadmanager.h"
#include "../trie-storage.h"
#include "../lru.h"

void load_trie() {

}

void save_trie(Trie* trie) {

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

        size_t len = strlen(base_path) + 1 + strlen(entry->d_name) + 1;
        char* path = (char*) malloc(len);
        if (!path) {
            continue;
        }

        snprintf(path, len, "%s/%s", base_path, entry->d_name);

        if (entry->d_type == DT_REG || entry->d_type == DT_DIR) {
            t_bucket* bucket = find_bucket(f_thread->lfu, path, path, 3, false);
            if (bucket) {
                trie_lock(bucket);
                insert(bucket->dir_trie, path);
                bucket->dir_count++;
                trie_unlock(bucket);
            }
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
    scan_dir_recursive(f_thread, f_thread->path, 0, 2);
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

    t_bucket* bucket = find_bucket(store, validation.full_path, validation.full_path, 3, false);
    if (bucket) {
        trie_lock(bucket);
        insert(bucket->dir_trie, validation.full_path);
        bucket->dir_count++;
        trie_unlock(bucket);
    }

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
    free(norm);
}

path_validation daemon_process_query(daemon_state* state, const char* cwd, const char* input) {
    if (!state || !state->store) {
        path_validation empty = {0};
        return empty;
    }

    return process_input(state->store, cwd, input);
}

