#pragma once
#include "trie.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CACHE_MAX_KEY_LEN 4096
#define CACHE_DEFAULT_MAX_ENTRIES 1024
#define CACHE_DEFAULT_TTL_SECONDS 2

typedef struct query_cache query_cache;

query_cache* cache_create(size_t max_entries, int ttl_seconds);
void cache_destroy(query_cache* cache);

/* Look up cached completions for prefix. Returns NULL on miss.
   Caller must NOT free the returned pointer — it's a cache-owned reference.
   Returns a deep copy that caller owns. */
scored_completions* cache_get(query_cache* cache, const char* prefix);

/* Store completions for prefix. Takes ownership of a copy. */
void cache_put(query_cache* cache, const char* prefix, const scored_completions* sc);

/* Clear all entries (called when scan completes). */
void cache_clear(query_cache* cache);

/* Get stats */
typedef struct {
    size_t entries;
    size_t max_entries;
    int ttl_seconds;
} cache_stats;
cache_stats cache_get_stats(const query_cache* cache);
