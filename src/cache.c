#include "cache.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── djb2 hash ────────────────────────────────────────────────────── */
static uint64_t hash_key(const char* str) {
    uint64_t hash = 5381;
    int c;
    while ((c = (unsigned char) *str++))
        hash = ((hash << 5) + hash) + (uint64_t) (unsigned char) c;
    return hash;
}

/* ── Per-entry refcount for borrowed references ──────────────────────
 * Dynamic key (was char key[4096] = 4MB per 1024 entries; now ~avg 60B).
 * Occupancy states:
 *   occupied && !deleted  -> live, findable, in LRU
 *   occupied && deleted   -> pending-delete (borrowed refs > 0), not
 *                            findable, not in LRU, must NOT be reused
 *   !occupied && deleted  -> tombstone (probe chain continues, reusable)
 *   !occupied && !deleted -> empty (chain ends, reusable)
 */
typedef struct cache_entry {
    char* key; /* heap-owned, NULL when not live/tombstone-cleared */
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
    s->lru_head.key = NULL;
    s->lru_head.value = NULL;
    s->lru_head.occupied = false;
    s->lru_head.deleted = false;
    s->lru_tail.key = NULL;
    s->lru_tail.value = NULL;
    s->lru_tail.occupied = false;
    s->lru_tail.deleted = false;
}

static void shard_lru_detach(cache_entry* node) {
    if (!node->lru_prev || !node->lru_next)
        return;
    node->lru_prev->lru_next = node->lru_next;
    node->lru_next->lru_prev = node->lru_prev;
    node->lru_prev = NULL;
    node->lru_next = NULL;
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

static void shard_free_entry(cache_entry* entry) {
    shard_free_entry_value(entry);
    if (entry->key) {
        free(entry->key);
        entry->key = NULL;
    }
}

static uint64_t now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec;
}

static cache_entry* shard_find(cache_shard* s, const char* key) {
    uint64_t h = hash_key(key);
    size_t idx = (size_t) (h % (uint64_t) s->capacity);

    for (size_t i = 0; i < s->capacity; i++) {
        size_t pos = (idx + i) % s->capacity;
        cache_entry* entry = &s->table[pos];
        if (!entry->occupied && !entry->deleted)
            return NULL; /* empty: chain ends */
        if (!entry->occupied && entry->deleted)
            continue; /* tombstone */
        if (entry->occupied && entry->deleted)
            continue; /* pending-delete: logically gone */
        if (!entry->key)
            continue;
        if (strcmp(entry->key, key) == 0)
            return entry;
    }
    return NULL;
}

static scored_completions* deep_copy_scored(const scored_completions* src) {
    if (!src)
        return NULL;
    scored_completions* dst = scored_completions_create(src->count > 0 ? src->count : 1);
    if (!dst)
        return NULL;
    for (size_t i = 0; i < src->count; i++) {
        if (dst->count >= dst->capacity)
            break;
        if (!src->entries[i].path)
            continue;
        scored_entry* d = &dst->entries[dst->count];
        d->path = strdup(src->entries[i].path);
        if (!d->path)
            break;
        d->score = src->entries[i].score;
        d->freq = src->entries[i].freq;
        d->last_access = src->entries[i].last_access;
        d->is_dir = src->entries[i].is_dir;
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
                /* free any keys in prior shards */
                for (size_t k = 0; k < cache->shards[j].capacity; k++)
                    shard_free_entry(&cache->shards[j].table[k]);
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
        pthread_mutex_lock(&s->lock);
        for (size_t j = 0; j < s->capacity; j++)
            shard_free_entry(&s->table[j]);
        pthread_mutex_unlock(&s->lock);
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
        /* Expired: if borrowed, defer free (pending-delete), else tombstone. */
        int refs = atomic_load(&entry->refs);
        if (refs > 0) {
            entry->deleted = true;
            shard_lru_detach(entry);
        } else {
            shard_free_entry(entry);
            entry->occupied = false;
            entry->deleted = true; /* tombstone keeps probe chain intact */
            shard_lru_detach(entry);
        }
        if (s->count > 0)
            s->count--;
        s->misses++;
        pthread_mutex_unlock(&s->lock);
        return NULL;
    }

    shard_lru_move_to_front(s, entry);
    atomic_fetch_add(&entry->refs, 1);
    entry->value->cache_shard = (int) si;
    entry->value->cache_slot = (size_t) (entry - s->table);
    s->hits++;
    pthread_mutex_unlock(&s->lock);

    return entry->value;
}

void cache_release(query_cache* cache, const scored_completions* sc) {
    if (!cache || !sc)
        return;

    int si = sc->cache_shard;
    if (si < 0 || si >= CACHE_NUM_SHARDS)
        return;

    cache_shard* s = &cache->shards[si];
    pthread_mutex_lock(&s->lock);
    if (sc->cache_slot < s->capacity) {
        cache_entry* entry = &s->table[sc->cache_slot];
        if (entry->occupied && entry->value == sc) {
            int old = atomic_fetch_sub(&entry->refs, 1);
            if (old == 1 && entry->deleted) {
                shard_free_entry(entry);
                entry->occupied = false;
                entry->deleted = true; /* tombstone */
                /* already detached when marked deleted */
                if (s->count > 0) {
                    /* count was already decremented at delete time; nothing */
                }
            }
        }
    }
    pthread_mutex_unlock(&s->lock);
}

void cache_put(query_cache* cache, const char* prefix, const scored_completions* sc) {
    if (!cache || !prefix || !sc)
        return;
    if (prefix[0] == '\0')
        return;
    if (strlen(prefix) >= (size_t) CACHE_MAX_KEY_LEN)
        return; /* bound disk/memory: skip absurd keys */

    size_t si = shard_for_key(prefix);
    cache_shard* s = &cache->shards[si];

    pthread_mutex_lock(&s->lock);

    cache_entry* existing = shard_find(s, prefix);
    if (existing) {
        scored_completions* copy = deep_copy_scored(sc);
        if (copy) {
            shard_free_entry_value(existing);
            existing->value = copy;
            existing->timestamp = now_seconds();
            shard_lru_move_to_front(s, existing);
        }
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
                shard_free_entry(lru);
                lru->occupied = false;
                lru->deleted = true; /* tombstone */
                shard_lru_detach(lru);
            }
            if (s->count > 0)
                s->count--;
            s->evictions++;
        }
    }

    uint64_t h = hash_key(prefix);
    size_t idx = (size_t) (h % (uint64_t) s->capacity);
    cache_entry* slot = NULL;

    /* Reuse only truly free slots (empty or tombstone), never pending-delete. */
    for (size_t i = 0; i < s->capacity; i++) {
        size_t pos = (idx + i) % s->capacity;
        cache_entry* entry = &s->table[pos];
        if (!entry->occupied) {
            slot = entry;
            break;
        }
    }

    if (!slot) {
        pthread_mutex_unlock(&s->lock);
        return;
    }

    char* keycopy = strdup(prefix);
    if (!keycopy) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    scored_completions* valcopy = deep_copy_scored(sc);
    if (!valcopy) {
        free(keycopy);
        pthread_mutex_unlock(&s->lock);
        return;
    }
    /* slot is free (empty or tombstone): key must be NULL here */
    if (slot->key) {
        free(slot->key);
        slot->key = NULL;
    }
    slot->key = keycopy;
    slot->value = valcopy;
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
                    shard_free_entry(entry);
                    entry->occupied = false;
                    entry->deleted = false;
                }
            } else if (entry->deleted) {
                /* clear tombstones too */
                shard_free_entry(entry);
                entry->occupied = false;
                entry->deleted = false;
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

/* ── Hot-cache disk persistence ──────────────────────────────────────
 * Saves top live entries so a daemon restart doesn't start with a cold
 * Tab cache. Format:
 *   u32 magic ("AHCH"), u32 version (1), u32 entry_count
 *   per entry: u32 key_len, key[key_len], u32 result_count
 *     per result: u32 path_len, path[path_len], double score,
 *                 u64 freq, u64 last_access, u8 is_dir
 * Bounds: <=256 entries, <=50 results each, key/path <=4096.
 */
#define HOTCACHE_MAGIC 0x41484348U /* "AHCH" */
#define HOTCACHE_VERSION 1U
#define HOTCACHE_MAX_ENTRIES 256
#define HOTCACHE_MAX_RESULTS 50

int cache_save_to_file(query_cache* cache, const char* path) {
    if (!cache || !path || path[0] == '\0')
        return -1;

    char tmp[4096];
    if (strlen(path) + 5 >= sizeof(tmp))
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* f = fopen(tmp, "wb");
    if (!f)
        return -1;

    uint32_t magic = HOTCACHE_MAGIC, version = HOTCACHE_VERSION, nentries = 0;
    /* Reserve header; patch entry count at the end. */
    if (fwrite(&magic, sizeof(magic), 1, f) != 1)
        goto werr;
    if (fwrite(&version, sizeof(version), 1, f) != 1)
        goto werr;
    long count_pos = ftell(f);
    if (count_pos < 0)
        goto werr;
    if (fwrite(&nentries, sizeof(nentries), 1, f) != 1)
        goto werr;

    uint32_t saved = 0;
    for (int si = 0; si < CACHE_NUM_SHARDS && saved < HOTCACHE_MAX_ENTRIES; si++) {
        cache_shard* s = &cache->shards[si];
        pthread_mutex_lock(&s->lock);
        for (size_t j = 0; j < s->capacity && saved < HOTCACHE_MAX_ENTRIES; j++) {
            cache_entry* e = &s->table[j];
            if (!e->occupied || e->deleted || !e->key || !e->value || e->value->count == 0)
                continue;
            size_t klen = strlen(e->key);
            if (klen == 0 || klen >= (size_t) CACHE_MAX_KEY_LEN)
                continue;
            uint32_t rc = (uint32_t) e->value->count;
            if (rc > HOTCACHE_MAX_RESULTS)
                rc = HOTCACHE_MAX_RESULTS;
            /* Validate paths before writing anything for this entry. */
            bool ok = true;
            for (uint32_t r = 0; r < rc; r++) {
                const char* p = e->value->entries[r].path;
                if (!p || p[0] == '\0' || strlen(p) >= 4096) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;

            uint32_t kl = (uint32_t) klen;
            if (fwrite(&kl, sizeof(kl), 1, f) != 1)
                break;
            if (fwrite(e->key, 1, klen, f) != klen)
                break;
            if (fwrite(&rc, sizeof(rc), 1, f) != 1)
                break;
            for (uint32_t r = 0; r < rc; r++) {
                scored_entry* se = &e->value->entries[r];
                uint32_t pl = (uint32_t) strlen(se->path);
                if (fwrite(&pl, sizeof(pl), 1, f) != 1) {
                    ok = false;
                    break;
                }
                if (fwrite(se->path, 1, pl, f) != pl) {
                    ok = false;
                    break;
                }
                double score = se->score;
                uint64_t freq = se->freq, last = se->last_access;
                uint8_t is_dir = se->is_dir ? 1 : 0;
                if (fwrite(&score, sizeof(score), 1, f) != 1 ||
                    fwrite(&freq, sizeof(freq), 1, f) != 1 ||
                    fwrite(&last, sizeof(last), 1, f) != 1 ||
                    fwrite(&is_dir, sizeof(is_dir), 1, f) != 1) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                break;
            saved++;
        }
        pthread_mutex_unlock(&s->lock);
    }

    /* Patch entry count. */
    if (fseek(f, count_pos, SEEK_SET) != 0)
        goto werr;
    if (fwrite(&saved, sizeof(saved), 1, f) != 1)
        goto werr;
    fclose(f);
    if (rename(tmp, path) != 0)
        return -1;
    return (int) saved;

werr:
    fclose(f);
    return -1;
}

int cache_load_from_file(query_cache* cache, const char* path) {
    if (!cache || !path || path[0] == '\0')
        return -1;

    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;

    uint32_t magic = 0, version = 0, nentries = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1)
        goto rerr;
    if (fread(&version, sizeof(version), 1, f) != 1)
        goto rerr;
    if (fread(&nentries, sizeof(nentries), 1, f) != 1)
        goto rerr;
    if (magic != HOTCACHE_MAGIC || version != HOTCACHE_VERSION)
        goto rerr;
    if (nentries > HOTCACHE_MAX_ENTRIES)
        goto rerr;

    int loaded = 0;
    for (uint32_t i = 0; i < nentries; i++) {
        uint32_t kl = 0;
        if (fread(&kl, sizeof(kl), 1, f) != 1)
            break;
        if (kl == 0 || kl >= (uint32_t) CACHE_MAX_KEY_LEN)
            break;
        char* key = malloc(kl + 1);
        if (!key)
            break;
        if (fread(key, 1, kl, f) != kl) {
            free(key);
            break;
        }
        key[kl] = '\0';

        uint32_t rc = 0;
        if (fread(&rc, sizeof(rc), 1, f) != 1) {
            free(key);
            break;
        }
        if (rc > HOTCACHE_MAX_RESULTS) {
            free(key);
            break;
        }
        scored_completions* sc = scored_completions_create(rc > 0 ? rc : 1);
        if (!sc) {
            free(key);
            break;
        }
        bool ok = true;
        for (uint32_t r = 0; r < rc; r++) {
            uint32_t pl = 0;
            if (fread(&pl, sizeof(pl), 1, f) != 1) {
                ok = false;
                break;
            }
            if (pl == 0 || pl >= 4096) {
                ok = false;
                break;
            }
            char* p = malloc(pl + 1);
            if (!p) {
                ok = false;
                break;
            }
            if (fread(p, 1, pl, f) != pl) {
                free(p);
                ok = false;
                break;
            }
            p[pl] = '\0';
            double score = 0;
            uint64_t freq = 0, last = 0;
            uint8_t is_dir = 0;
            if (fread(&score, sizeof(score), 1, f) != 1 ||
                fread(&freq, sizeof(freq), 1, f) != 1 ||
                fread(&last, sizeof(last), 1, f) != 1 ||
                fread(&is_dir, sizeof(is_dir), 1, f) != 1) {
                free(p);
                ok = false;
                break;
            }
            if (sc->count < sc->capacity) {
                scored_entry* d = &sc->entries[sc->count++];
                d->path = p;
                d->score = score;
                d->freq = freq;
                d->last_access = last;
                d->is_dir = is_dir ? true : false;
            } else {
                free(p);
            }
        }
        if (ok) {
            cache_put(cache, key, sc);
            loaded++;
        }
        scored_completions_free(sc);
        free(key);
        if (!ok)
            break;
    }
    fclose(f);
    return loaded;

rerr:
    fclose(f);
    return -1;
}
