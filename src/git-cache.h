#pragma once

#include <stdbool.h>
#include <stddef.h>

#define GIT_CACHE_MAX_FILES 200000
#define GIT_CACHE_PATH_LEN 512

typedef struct {
    char root[512];
    char** files;
    size_t count;
    size_t capacity;
    bool active;
} git_cache;

/* Initialize an empty cache */
void git_cache_init(git_cache* cache);

/* Populate cache by running `git ls-files` in the given root.
   Returns true on success, false if git is unavailable or root is not a git repo. */
bool git_cache_populate(git_cache* cache, const char* root);

/* Check if a path is tracked. Path must be absolute.
   Returns false if cache is not active. */
bool git_cache_is_tracked(git_cache* cache, const char* abs_path);

/* Free cached file list */
void git_cache_free(git_cache* cache);

/* Reset cache to empty state */
void git_cache_reset(git_cache* cache);
