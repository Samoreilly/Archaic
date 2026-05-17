#include "incremental.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

void incremental_init(incremental_state* state) {
    if (!state)
        return;
    state->count = 0;
    pthread_mutex_init(&state->lock, NULL);
    for (int i = 0; i < MAX_INCR_TRACKED_DIRS; i++) {
        state->dirs[i].path[0] = '\0';
        state->dirs[i].mtime = 0;
        state->dirs[i].exists = false;
    }
}

void incremental_free(incremental_state* state) {
    if (!state)
        return;
    pthread_mutex_destroy(&state->lock);
}

void incremental_record_dir(incremental_state* state, const char* path, time_t mtime) {
    if (!state || !path)
        return;

    pthread_mutex_lock(&state->lock);

    for (int i = 0; i < state->count; i++) {
        if (strcmp(state->dirs[i].path, path) == 0) {
            state->dirs[i].mtime = mtime;
            state->dirs[i].exists = true;
            pthread_mutex_unlock(&state->lock);
            return;
        }
    }

    if (state->count < MAX_INCR_TRACKED_DIRS) {
        strncpy(state->dirs[state->count].path, path, MAX_INCR_PATH_LEN - 1);
        state->dirs[state->count].path[MAX_INCR_PATH_LEN - 1] = '\0';
        state->dirs[state->count].mtime = mtime;
        state->dirs[state->count].exists = true;
        state->count++;
    }

    pthread_mutex_unlock(&state->lock);
}

bool incremental_needs_rescan(incremental_state* state, const char* path, time_t current_mtime) {
    if (!state || !path)
        return true;

    pthread_mutex_lock(&state->lock);

    for (int i = 0; i < state->count; i++) {
        if (strcmp(state->dirs[i].path, path) == 0) {
            bool needs_rescan = state->dirs[i].mtime != current_mtime;
            pthread_mutex_unlock(&state->lock);
            return needs_rescan;
        }
    }

    pthread_mutex_unlock(&state->lock);
    return true;
}

void incremental_mark_all_stale(incremental_state* state) {
    if (!state)
        return;

    pthread_mutex_lock(&state->lock);
    for (int i = 0; i < state->count; i++) {
        state->dirs[i].exists = false;
    }
    pthread_mutex_unlock(&state->lock);
}

int incremental_remove_missing(incremental_state* state) {
    if (!state)
        return 0;

    pthread_mutex_lock(&state->lock);

    int removed = 0;
    int new_count = 0;
    for (int i = 0; i < state->count; i++) {
        if (state->dirs[i].exists) {
            if (new_count != i) {
                state->dirs[new_count] = state->dirs[i];
            }
            new_count++;
        } else {
            removed++;
        }
    }
    state->count = new_count;

    pthread_mutex_unlock(&state->lock);
    return removed;
}

void incremental_clear(incremental_state* state) {
    if (!state)
        return;

    pthread_mutex_lock(&state->lock);
    state->count = 0;
    for (int i = 0; i < MAX_INCR_TRACKED_DIRS; i++) {
        state->dirs[i].path[0] = '\0';
        state->dirs[i].mtime = 0;
        state->dirs[i].exists = false;
    }
    pthread_mutex_unlock(&state->lock);
}
