#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "fileloader.h"
#include "../threadmanager.h"
#include "../trie-storage.h"

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
                insert(bucket->dir_trie, path);
                bucket->dir_count++;
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
        insert(bucket->dir_trie, validation.full_path);
        bucket->dir_count++;
    }

    return validation;
}

