#include "cache.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* djb2 hash */
static uint64_t hash_key(const char* str) {
    uint64_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

/* Doubly-linked list node for LRU ordering */
typedef struct cache_lru_node {
    struct cache_lru_node* prev;
    struct cache_lru_node* next;
} cache_lru_node;

/* Hash table entry */
typedef struct cache_entry {
    char key[CACHE_MAX_KEY_LEN];
    scored_completions* value;
    uint64_t timestamp;
    bool occupied;
    bool deleted;
    cache_lru_node lru;
} cache_entry;

struct query_cache {
    cache_entry* table;
    size_t capacity;
    size_t count;
    int ttl_seconds;
    cache_lru_node lru_head;
    cache_lru_node lru_tail;
    pthread_mutex_t lock;
};

static void lru_init(query_cache* cache) {
    cache->lru_head.prev = NULL;
    cache->lru_head.next = &cache->lru_tail;
    cache->lru_tail.prev = &cache->lru_head;
    cache->lru_tail.next = NULL;
}

static void lru_detach(cache_lru_node* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

static void lru_push_front(query_cache* cache, cache_lru_node* node) {
    node->next = cache->lru_head.next;
    node->prev = &cache->lru_head;
    cache->lru_head.next->prev = node;
    cache->lru_head.next = node;
}

static void lru_move_to_front(query_cache* cache, cache_lru_node* node) {
    lru_detach(node);
    lru_push_front(cache, node);
}

static cache_entry* lru_back(query_cache* cache) {
    cache_lru_node* node = cache->lru_tail.prev;
    if (node == &cache->lru_head) return NULL;
    return (cache_entry*)((char*)node - offsetof(cache_entry, lru));
}

static scored_completions* deep_copy_scored(const scored_completions* src) {
    if (!src) return NULL;
    scored_completions* dst = scored_completions_create(src->capacity);
    if (!dst) return NULL;
    for (size_t i = 0; i < src->count; i++) {
        if (dst->count >= dst->capacity) break;
        memcpy(&dst->entries[dst->count], &src->entries[i], sizeof(scored_entry));
        dst->count++;
    }
    return dst;
}

static void free_entry_value(cache_entry* entry) {
    if (entry->value) {
        scored_completions_free(entry->value);
        entry->value = NULL;
    }
}

static uint64_t now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

query_cache* cache_create(size_t max_entries, int ttl_seconds) {
    if (max_entries == 0) max_entries = CACHE_DEFAULT_MAX_ENTRIES;
    if (ttl_seconds <= 0) ttl_seconds = CACHE_DEFAULT_TTL_SECONDS;

    query_cache* cache = calloc(1, sizeof(query_cache));
    if (!cache) return NULL;

    cache->table = calloc(max_entries, sizeof(cache_entry));
    if (!cache->table) {
        free(cache);
        return NULL;
    }

    cache->capacity = max_entries;
    cache->count = 0;
    cache->ttl_seconds = ttl_seconds;
    lru_init(cache);
    pthread_mutex_init(&cache->lock, NULL);
    return cache;
}

void cache_destroy(query_cache* cache) {
    if (!cache) return;
    cache_clear(cache);
    free(cache->table);
    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

static cache_entry* cache_find(query_cache* cache, const char* key) {
    uint64_t h = hash_key(key);
    size_t idx = h % cache->capacity;

    for (size_t i = 0; i < cache->capacity; i++) {
        size_t pos = (idx + i) % cache->capacity;
        cache_entry* entry = &cache->table[pos];
        if (!entry->occupied) return NULL;
        if (entry->deleted) continue;
        if (strcmp(entry->key, key) == 0) return entry;
    }
    return NULL;
}

scored_completions* cache_get(query_cache* cache, const char* prefix) {
    if (!cache || !prefix) return NULL;

    pthread_mutex_lock(&cache->lock);

    cache_entry* entry = cache_find(cache, prefix);
    if (!entry) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    /* Check TTL */
    uint64_t now = now_seconds();
    if ((int)(now - entry->timestamp) > cache->ttl_seconds) {
        free_entry_value(entry);
        entry->occupied = false;
        entry->deleted = false;
        lru_detach(&entry->lru);
        cache->count--;
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    /* Hit: move to front, return deep copy */
    lru_move_to_front(cache, &entry->lru);
    scored_completions* copy = deep_copy_scored(entry->value);
    pthread_mutex_unlock(&cache->lock);
    return copy;
}

void cache_put(query_cache* cache, const char* prefix, const scored_completions* sc) {
    if (!cache || !prefix || !sc) return;

    pthread_mutex_lock(&cache->lock);

    /* Check if key already exists */
    cache_entry* existing = cache_find(cache, prefix);
    if (existing) {
        free_entry_value(existing);
        existing->value = deep_copy_scored(sc);
        existing->timestamp = now_seconds();
        lru_move_to_front(cache, &existing->lru);
        pthread_mutex_unlock(&cache->lock);
        return;
    }

    /* Evict LRU if full */
    if (cache->count >= cache->capacity) {
        cache_entry* lru = lru_back(cache);
        if (lru) {
            free_entry_value(lru);
            lru->occupied = false;
            lru->deleted = false;
            lru_detach(&lru->lru);
            cache->count--;
        }
    }

    /* Find empty slot via linear probing */
    uint64_t h = hash_key(prefix);
    size_t idx = h % cache->capacity;
    cache_entry* slot = NULL;

    for (size_t i = 0; i < cache->capacity; i++) {
        size_t pos = (idx + i) % cache->capacity;
        cache_entry* entry = &cache->table[pos];
        if (!entry->occupied || entry->deleted) {
            slot = entry;
            break;
        }
    }

    if (!slot) {
        pthread_mutex_unlock(&cache->lock);
        return;
    }

    strncpy(slot->key, prefix, CACHE_MAX_KEY_LEN - 1);
    slot->key[CACHE_MAX_KEY_LEN - 1] = '\0';
    slot->value = deep_copy_scored(sc);
    slot->timestamp = now_seconds();
    slot->occupied = true;
    slot->deleted = false;
    lru_push_front(cache, &slot->lru);
    cache->count++;

    pthread_mutex_unlock(&cache->lock);
}

void cache_clear(query_cache* cache) {
    if (!cache) return;

    pthread_mutex_lock(&cache->lock);

    for (size_t i = 0; i < cache->capacity; i++) {
        cache_entry* entry = &cache->table[i];
        if (entry->occupied) {
            free_entry_value(entry);
            entry->occupied = false;
            entry->deleted = false;
        }
    }
    cache->count = 0;
    lru_init(cache);

    pthread_mutex_unlock(&cache->lock);
}

cache_stats cache_get_stats(const query_cache* cache) {
    cache_stats s = {0};
    if (!cache) return s;
    s.entries = cache->count;
    s.max_entries = cache->capacity;
    s.ttl_seconds = cache->ttl_seconds;
    return s;
}
