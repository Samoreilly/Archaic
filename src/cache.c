#include "cache.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── djb2 hash ────────────────────────────────────────────────────── */
static uint64_t hash_key(const char* str) {
    uint64_t hash = 5381;
    int c;
    while ((c = (unsigned char) *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

/* ── Per-entry refcount for borrowed references ────────────────────── */
typedef struct cache_entry {
    char key[CACHE_MAX_KEY_LEN];
    scored_completions* value; /* cache-owned, heap-allocated */
    uint64_t timestamp;
    bool occupied;
    bool deleted;
    atomic_int refs; /* borrowed reference count */
    struct cache_entry* lru_prev;
    struct cache_entry* lru_next;
} cache_entry;

/* ── Shard: independent lock + hash table segment ─────────────────── */
typedef struct {
    cache_entry* table;
    size_t capacity;
    size_t count;
    cache_entry lru_head; /* sentinel nodes for LRU list */
    cache_entry lru_tail;
    pthread_mutex_t lock;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
} cache_shard;

/* ── Top-level cache ──────────────────────────────────────────────── */
struct query_cache {
    cache_shard shards[CACHE_NUM_SHARDS];
    size_t entries_per_shard;
    int ttl_seconds;
};

static size_t shard_for_key(const char* key) {
    return hash_key(key) % CACHE_NUM_SHARDS;
}

/* ── Shard-local helpers ───────────────────────────────────────────── */

static void shard_lru_init(cache_shard* s) {
    s->lru_head.lru_prev = NULL;
    s->lru_head.lru_next = &s->lru_tail;
    s->lru_tail.lru_prev = &s->lru_head;
    s->lru_tail.lru_next = NULL;
}

static void shard_lru_detach(cache_entry* node) {
    node->lru_prev->lru_next = node->lru_next;
    node->lru_next->lru_prev = node->lru_prev;
}

static void shard_lru_push_front(cache_shard* s, cache_entry* node) {
    node->lru_next = s->lru_head.lru_next;
    node->lru_prev = &s->lru_head;
    s->lru_head.lru_next->lru_prev = node;
    s->lru_head.lru_next = node;
}

static void shard_lru_move_to_front(cache_shard* s, cache_entry* node) {
    shard_lru_detach(node);
    shard_lru_push_front(s, node);
}

static cache_entry* shard_lru_back(cache_shard* s) {
    cache_entry* node = s->lru_tail.lru_prev;
    if (node == &s->lru_head)
        return NULL;
    return node;
}

static void shard_free_entry_value(cache_entry* entry) {
    if (entry->value) {
        scored_completions_free(entry->value);
        entry->value = NULL;
    }
}

static uint64_t now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec;
}

static cache_entry* shard_find(cache_shard* s, const char* key) {
    uint64_t h = hash_key(key);
    size_t idx = h % s->capacity;

    for (size_t i = 0; i < s->capacity; i++) {
        size_t pos = (idx + i) % s->capacity;
        cache_entry* entry = &s->table[pos];
        if (!entry->occupied)
            return NULL;
        if (entry->deleted)
            continue;
        if (strcmp(entry->key, key) == 0)
            return entry;
    }
    return NULL;
}

static scored_completions* deep_copy_scored(const scored_completions* src) {
    if (!src)
        return NULL;
    scored_completions* dst = scored_completions_create(src->capacity);
    if (!dst)
        return NULL;
    for (size_t i = 0; i < src->count; i++) {
        if (dst->count >= dst->capacity)
            break;
        memcpy(&dst->entries[dst->count], &src->entries[i], sizeof(scored_entry));
        dst->count++;
    }
    return dst;
}

/* ── Public API ────────────────────────────────────────────────────── */

query_cache* cache_create(size_t max_entries, int ttl_seconds) {
    if (max_entries == 0)
        max_entries = CACHE_DEFAULT_MAX_ENTRIES;
    if (ttl_seconds <= 0)
        ttl_seconds = CACHE_DEFAULT_TTL_SECONDS;

    query_cache* cache = calloc(1, sizeof(query_cache));
    if (!cache)
        return NULL;

    cache->entries_per_shard = (max_entries + CACHE_NUM_SHARDS - 1) / CACHE_NUM_SHARDS;
    cache->ttl_seconds = ttl_seconds;

    for (int i = 0; i < CACHE_NUM_SHARDS; i++) {
        cache_shard* s = &cache->shards[i];
        s->capacity = cache->entries_per_shard;
        s->table = calloc(s->capacity, sizeof(cache_entry));
        if (!s->table) {
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&cache->shards[j].lock);
                free(cache->shards[j].table);
            }
            free(cache);
            return NULL;
        }
        s->count = 0;
        s->hits = 0;
        s->misses = 0;
        s->evictions = 0;
        shard_lru_init(s);
        pthread_mutex_init(&s->lock, NULL);
    }

    return cache;
}

void cache_destroy(query_cache* cache) {
    if (!cache)
        return;
    cache_clear(cache);
    for (int i = 0; i < CACHE_NUM_SHARDS; i++) {
        cache_shard* s = &cache->shards[i];
        pthread_mutex_destroy(&s->lock);
        free(s->table);
    }
    free(cache);
}

const scored_completions* cache_get(query_cache* cache, const char* prefix) {
    if (!cache || !prefix)
        return NULL;

    size_t si = shard_for_key(prefix);
    cache_shard* s = &cache->shards[si];

    pthread_mutex_lock(&s->lock);

    cache_entry* entry = shard_find(s, prefix);
    if (!entry) {
        s->misses++;
        pthread_mutex_unlock(&s->lock);
        return NULL;
    }

    uint64_t now = now_seconds();
    if ((int) (now - entry->timestamp) > cache->ttl_seconds) {
        shard_free_entry_value(entry);
        entry->occupied = false;
        entry->deleted = false;
        shard_lru_detach(entry);
        s->count--;
        s->misses++;
        pthread_mutex_unlock(&s->lock);
        return NULL;
    }

    shard_lru_move_to_front(s, entry);
    atomic_fetch_add(&entry->refs, 1);
    s->hits++;
    pthread_mutex_unlock(&s->lock);

    return entry->value;
}

void cache_release(query_cache* cache, const scored_completions* sc) {
    if (!cache || !sc)
        return;

    for (int i = 0; i < CACHE_NUM_SHARDS; i++) {
        cache_shard* s = &cache->shards[i];
        pthread_mutex_lock(&s->lock);
        for (size_t j = 0; j < s->capacity; j++) {
            cache_entry* entry = &s->table[j];
            if (entry->occupied && !entry->deleted && entry->value == sc) {
                int old = atomic_fetch_sub(&entry->refs, 1);
                if (old == 1 && entry->deleted) {
                    shard_free_entry_value(entry);
                    entry->occupied = false;
                    entry->deleted = false;
                    shard_lru_detach(entry);
                    s->count--;
                }
                pthread_mutex_unlock(&s->lock);
                return;
            }
        }
        pthread_mutex_unlock(&s->lock);
    }
}

void cache_put(query_cache* cache, const char* prefix, const scored_completions* sc) {
    if (!cache || !prefix || !sc)
        return;

    size_t si = shard_for_key(prefix);
    cache_shard* s = &cache->shards[si];

    pthread_mutex_lock(&s->lock);

    cache_entry* existing = shard_find(s, prefix);
    if (existing) {
        shard_free_entry_value(existing);
        existing->value = deep_copy_scored(sc);
        existing->timestamp = now_seconds();
        shard_lru_move_to_front(s, existing);
        pthread_mutex_unlock(&s->lock);
        return;
    }

    if (s->count >= s->capacity) {
        cache_entry* lru = shard_lru_back(s);
        if (lru) {
            int refs = atomic_load(&lru->refs);
            if (refs > 0) {
                lru->deleted = true;
                shard_lru_detach(lru);
            } else {
                shard_free_entry_value(lru);
                lru->occupied = false;
                lru->deleted = false;
                shard_lru_detach(lru);
            }
            s->count--;
            s->evictions++;
        }
    }

    uint64_t h = hash_key(prefix);
    size_t idx = h % s->capacity;
    cache_entry* slot = NULL;

    for (size_t i = 0; i < s->capacity; i++) {
        size_t pos = (idx + i) % s->capacity;
        cache_entry* entry = &s->table[pos];
        if (!entry->occupied || entry->deleted) {
            slot = entry;
            break;
        }
    }

    if (!slot) {
        pthread_mutex_unlock(&s->lock);
        return;
    }

    strncpy(slot->key, prefix, CACHE_MAX_KEY_LEN - 1);
    slot->key[CACHE_MAX_KEY_LEN - 1] = '\0';
    slot->value = deep_copy_scored(sc);
    slot->timestamp = now_seconds();
    slot->occupied = true;
    slot->deleted = false;
    atomic_store(&slot->refs, 0);
    shard_lru_push_front(s, slot);
    s->count++;

    pthread_mutex_unlock(&s->lock);
}

void cache_invalidate(query_cache* cache) {
    if (!cache)
        return;

    for (int i = 0; i < CACHE_NUM_SHARDS; i++) {
        cache_shard* s = &cache->shards[i];
        pthread_mutex_lock(&s->lock);
        for (size_t j = 0; j < s->capacity; j++) {
            cache_entry* entry = &s->table[j];
            if (entry->occupied && !entry->deleted) {
                entry->timestamp = 0;
            }
        }
        pthread_mutex_unlock(&s->lock);
    }
}

void cache_clear(query_cache* cache) {
    if (!cache)
        return;

    for (int i = 0; i < CACHE_NUM_SHARDS; i++) {
        cache_shard* s = &cache->shards[i];
        pthread_mutex_lock(&s->lock);
        for (size_t j = 0; j < s->capacity; j++) {
            cache_entry* entry = &s->table[j];
            if (entry->occupied) {
                int refs = atomic_load(&entry->refs);
                if (refs > 0) {
                    entry->deleted = true;
                } else {
                    shard_free_entry_value(entry);
                    entry->occupied = false;
                    entry->deleted = false;
                }
            }
        }
        s->count = 0;
        shard_lru_init(s);
        pthread_mutex_unlock(&s->lock);
    }
}

cache_stats cache_get_stats(const query_cache* cache) {
    cache_stats total = {0};
    if (!cache)
        return total;

    for (int i = 0; i < CACHE_NUM_SHARDS; i++) {
        const cache_shard* s = &cache->shards[i];
        total.entries += s->count;
        total.hits += s->hits;
        total.misses += s->misses;
        total.evictions += s->evictions;
    }
    total.max_entries = cache->entries_per_shard * CACHE_NUM_SHARDS;
    total.ttl_seconds = cache->ttl_seconds;
    return total;
}
