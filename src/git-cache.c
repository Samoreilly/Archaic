#include "git-cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

void git_cache_init(git_cache* cache) {
    if (!cache) return;
    cache->root[0] = '\0';
    cache->files = NULL;
    cache->count = 0;
    cache->capacity = 0;
    cache->active = false;
}

void git_cache_free(git_cache* cache) {
    if (!cache) return;
    for (size_t i = 0; i < cache->count; i++) {
        free(cache->files[i]);
    }
    free(cache->files);
    cache->files = NULL;
    cache->count = 0;
    cache->capacity = 0;
    cache->active = false;
}

void git_cache_reset(git_cache* cache) {
    git_cache_free(cache);
}

static int cmp_strings(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

bool git_cache_populate(git_cache* cache, const char* root) {
    if (!cache || !root) return false;

    char cmd[1024];
    int n = snprintf(cmd, sizeof(cmd), "git -C \"%s\" ls-files 2>/dev/null", root);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return false;

    FILE* fp = popen(cmd, "r");
    if (!fp) return false;

    git_cache_free(cache);

    char** files = malloc(GIT_CACHE_MAX_FILES * sizeof(char*));
    if (!files) {
        pclose(fp);
        return false;
    }

    size_t count = 0;
    char line[GIT_CACHE_PATH_LEN];
    while (fgets(line, sizeof(line), fp) && count < GIT_CACHE_MAX_FILES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        files[count] = strdup(line);
        if (!files[count]) continue;
        count++;
    }
    pclose(fp);

    if (count == 0) {
        free(files);
        return false;
    }

    qsort(files, count, sizeof(char*), cmp_strings);

    cache->files = files;
    cache->count = count;
    cache->capacity = count;
    cache->active = true;
    strncpy(cache->root, root, sizeof(cache->root) - 1);
    cache->root[sizeof(cache->root) - 1] = '\0';

    LOG_INFO("git-cache", "cached %zu tracked files from %s", count, root);
    return true;
}

bool git_cache_is_tracked(git_cache* cache, const char* abs_path) {
    if (!cache || !cache->active || !abs_path) return false;

    size_t root_len = strlen(cache->root);
    if (strncmp(abs_path, cache->root, root_len) != 0) return false;

    const char* rel = abs_path + root_len;
    if (rel[0] == '/') rel++;

    char** result = bsearch(&rel, cache->files, cache->count, sizeof(char*), cmp_strings);
    return result != NULL;
}
