#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define MAX_INCR_TRACKED_DIRS 10000
#define MAX_INCR_PATH_LEN 4096

typedef struct dir_timestamp {
    char path[MAX_INCR_PATH_LEN];
    time_t mtime;
    bool exists;
} dir_timestamp;

typedef struct incremental_state {
    dir_timestamp dirs[MAX_INCR_TRACKED_DIRS];
    int count;
    pthread_mutex_t lock;
} incremental_state;

void incremental_init(incremental_state* state);
void incremental_free(incremental_state* state);

/* Record a directory's modification time during scan */
void incremental_record_dir(incremental_state* state, const char* path, time_t mtime);

/* Check if directory needs re-scan (mtime changed or not tracked) */
bool incremental_needs_rescan(incremental_state* state, const char* path, time_t current_mtime);

/* Mark all directories as needing verification */
void incremental_mark_all_stale(incremental_state* state);

/* Remove directories that no longer exist and return count removed */
int incremental_remove_missing(incremental_state* state);

/* Clear all tracked directories */
void incremental_clear(incremental_state* state);
