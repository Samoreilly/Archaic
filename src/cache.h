#pragma once
#include "trie.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CACHE_MAX_KEY_LEN 4096
#define CACHE_DEFAULT_MAX_ENTRIES 1024
#define CACHE_DEFAULT_TTL_SECONDS 2
#define CACHE_NUM_SHARDS 16

typedef struct query_cache query_cache;

/* ── Cache creation / destruction ─────────────────────────────────── */

query_cache* cache_create(size_t max_entries, int ttl_seconds);
void cache_destroy(query_cache* cache);

/* ── Cache lookups ────────────────────────────────────────────────── */

/*
 * cache_get - look up cached completions for prefix.
 *
 * Returns a READ-ONLY borrowed pointer on hit (caller must NOT free it).
 * Returns NULL on miss.
 *
 * The returned pointer is valid only until cache_release() is called.
 * Caller MUST call cache_release() exactly once when done.
 *
 * Usage:
 *   scored_completions* sc = cache_get(cache, "/home/user");
 *   if (sc) {
 *       // use sc (read-only)
 *       cache_release(cache, sc);
 *   }
 */
const scored_completions* cache_get(query_cache* cache, const char* prefix);

/*
 * cache_release - release a borrowed reference obtained from cache_get().
 *
 * Must be called exactly once for every successful cache_get().
 * After release, the pointer is invalid and must not be dereferenced.
 */
void cache_release(query_cache* cache, const scored_completions* sc);

/*
 * cache_put - store completions for prefix.
 *
 * Takes ownership: creates an internal copy (cache owns its own data).
 * The caller may free `sc` after this call.
 */
void cache_put(query_cache* cache, const char* prefix, const scored_completions* sc);

void cache_invalidate(query_cache* cache);

void cache_clear(query_cache* cache);

/* ── Stats ─────────────────────────────────────────────────────────── */

typedef struct {
    size_t entries;
    size_t max_entries;
    int ttl_seconds;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
} cache_stats;

cache_stats cache_get_stats(const query_cache* cache);
