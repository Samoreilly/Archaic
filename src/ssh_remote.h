#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct remote_cache remote_cache;

typedef struct {
    char** paths;
    size_t count;
    size_t capacity;
} remote_completions;

remote_cache* remote_cache_create(size_t max_entries, int ttl_seconds);
void remote_cache_destroy(remote_cache* cache);

const remote_completions* remote_cache_get(remote_cache* cache, const char* key);
void remote_cache_put(remote_cache* cache, const char* key, const remote_completions* rc);

bool is_ssh_path(const char* prefix);

int parse_ssh_path(const char* prefix, char* user_buf, size_t user_len, char* host_buf,
                   size_t host_len, char* path_buf, size_t path_len);

remote_completions* ssh_get_completions(const char* prefix, int timeout_seconds,
                                        const char* default_user, remote_cache* cache);

void remote_completions_free(remote_completions* rc);
