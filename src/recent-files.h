#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define MAX_RECENT_FILES 100
#define MAX_RECENT_PATH_LEN 4096

typedef struct recent_file_entry {
    char path[MAX_RECENT_PATH_LEN];
    uint64_t last_access;
    bool is_dir;
} recent_file_entry;

typedef struct recent_files {
    recent_file_entry entries[MAX_RECENT_FILES];
    int count;
    int capacity;
    pthread_mutex_t lock;
} recent_files;

/* Initialize recent files list with given capacity (max 100) */
void recent_files_init(recent_files* rf, int capacity);

/* Free recent files list */
void recent_files_free(recent_files* rf);

/* Add or update a file in the recent files list */
void recent_files_touch(recent_files* rf, const char* path, bool is_dir);

/* Get the n most recent files, returns actual count written */
int recent_files_get(recent_files* rf, char** paths, bool* is_dirs, int n);

/* Clear all recent files */
void recent_files_clear(recent_files* rf);
