#include "recent-files.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

void recent_files_init(recent_files* rf, int capacity) {
    if (!rf)
        return;
    if (capacity > MAX_RECENT_FILES)
        capacity = MAX_RECENT_FILES;
    if (capacity < 1)
        capacity = 10;

    rf->count = 0;
    rf->capacity = capacity;
    pthread_mutex_init(&rf->lock, NULL);

    for (int i = 0; i < MAX_RECENT_FILES; i++) {
        rf->entries[i].path[0] = '\0';
        rf->entries[i].last_access = 0;
        rf->entries[i].is_dir = false;
    }
}

void recent_files_free(recent_files* rf) {
    if (!rf)
        return;
    pthread_mutex_destroy(&rf->lock);
}

void recent_files_touch(recent_files* rf, const char* path, bool is_dir) {
    if (!rf || !path)
        return;

    pthread_mutex_lock(&rf->lock);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t) ts.tv_sec;

    int existing_idx = -1;
    for (int i = 0; i < rf->count; i++) {
        if (strcmp(rf->entries[i].path, path) == 0) {
            existing_idx = i;
            break;
        }
    }

    if (existing_idx >= 0) {
        rf->entries[existing_idx].last_access = now;
        rf->entries[existing_idx].is_dir = is_dir;

        recent_file_entry temp = rf->entries[existing_idx];
        for (int i = existing_idx; i > 0; i--) {
            rf->entries[i] = rf->entries[i - 1];
        }
        rf->entries[0] = temp;
    } else {
        if (rf->count >= rf->capacity) {
            for (int i = rf->capacity - 1; i > 0; i--) {
                rf->entries[i] = rf->entries[i - 1];
            }
        } else {
            for (int i = rf->count; i > 0; i--) {
                rf->entries[i] = rf->entries[i - 1];
            }
            rf->count++;
        }

        strncpy(rf->entries[0].path, path, MAX_RECENT_PATH_LEN - 1);
        rf->entries[0].path[MAX_RECENT_PATH_LEN - 1] = '\0';
        rf->entries[0].last_access = now;
        rf->entries[0].is_dir = is_dir;
    }

    pthread_mutex_unlock(&rf->lock);
}

int recent_files_get(recent_files* rf, char** paths, bool* is_dirs, int n) {
    if (!rf || !paths || !is_dirs || n <= 0)
        return 0;

    pthread_mutex_lock(&rf->lock);

    int count = n < rf->count ? n : rf->count;
    for (int i = 0; i < count; i++) {
        strncpy(paths[i], rf->entries[i].path, MAX_RECENT_PATH_LEN - 1);
        paths[i][MAX_RECENT_PATH_LEN - 1] = '\0';
        is_dirs[i] = rf->entries[i].is_dir;
    }

    pthread_mutex_unlock(&rf->lock);
    return count;
}

void recent_files_clear(recent_files* rf) {
    if (!rf)
        return;

    pthread_mutex_lock(&rf->lock);
    rf->count = 0;
    for (int i = 0; i < MAX_RECENT_FILES; i++) {
        rf->entries[i].path[0] = '\0';
        rf->entries[i].last_access = 0;
        rf->entries[i].is_dir = false;
    }
    pthread_mutex_unlock(&rf->lock);
}
