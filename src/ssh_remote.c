#include "ssh_remote.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REMOTE_CACHE_MAX_KEY_LEN 4096
#define REMOTE_CACHE_MAX_ENTRIES_PER_ENTRY 50
#define REMOTE_CACHE_DEFAULT_TTL 10
#define REMOTE_CACHE_DEFAULT_MAX 256

/* ── djb2 hash ────────────────────────────────────────────────────── */
static uint64_t hash_key(const char* str) {
    uint64_t hash = 5381;
    int c;
    while ((c = (unsigned char) *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

/* ── Cache entry ──────────────────────────────────────────────────── */
typedef struct {
    char key[REMOTE_CACHE_MAX_KEY_LEN];
    remote_completions* value;
    uint64_t timestamp;
    bool occupied;
} rc_entry;

/* ── Remote cache ─────────────────────────────────────────────────── */
struct remote_cache {
    rc_entry* table;
    size_t capacity;
    size_t count;
    int ttl_seconds;
    pthread_mutex_t lock;
};

static uint64_t now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec;
}

static rc_entry* rc_find(remote_cache* cache, const char* key) {
    uint64_t h = hash_key(key);
    size_t idx = h % cache->capacity;

    for (size_t i = 0; i < cache->capacity; i++) {
        size_t pos = (idx + i) % cache->capacity;
        rc_entry* entry = &cache->table[pos];
        if (!entry->occupied)
            return NULL;
        if (strcmp(entry->key, key) == 0)
            return entry;
    }
    return NULL;
}

static remote_completions* rc_deep_copy(const remote_completions* src) {
    if (!src)
        return NULL;
    remote_completions* dst = calloc(1, sizeof(remote_completions));
    if (!dst)
        return NULL;
    dst->capacity = src->capacity;
    dst->count = src->count;
    dst->paths = calloc(src->capacity, sizeof(char*));
    if (!dst->paths) {
        free(dst);
        return NULL;
    }
    for (size_t i = 0; i < src->count; i++) {
        dst->paths[i] = strdup(src->paths[i]);
    }
    return dst;
}

static void rc_free_entry_value(rc_entry* entry) {
    if (entry->value) {
        remote_completions_free(entry->value);
        entry->value = NULL;
    }
}

remote_cache* remote_cache_create(size_t max_entries, int ttl_seconds) {
    if (max_entries == 0)
        max_entries = REMOTE_CACHE_DEFAULT_MAX;
    if (ttl_seconds <= 0)
        ttl_seconds = REMOTE_CACHE_DEFAULT_TTL;

    remote_cache* cache = calloc(1, sizeof(remote_cache));
    if (!cache)
        return NULL;

    cache->capacity = max_entries;
    cache->ttl_seconds = ttl_seconds;
    cache->table = calloc(max_entries, sizeof(rc_entry));
    if (!cache->table) {
        free(cache);
        return NULL;
    }

    pthread_mutex_init(&cache->lock, NULL);
    return cache;
}

void remote_cache_destroy(remote_cache* cache) {
    if (!cache)
        return;
    for (size_t i = 0; i < cache->capacity; i++) {
        rc_free_entry_value(&cache->table[i]);
    }
    free(cache->table);
    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

const remote_completions* remote_cache_get(remote_cache* cache, const char* key) {
    if (!cache || !key)
        return NULL;

    pthread_mutex_lock(&cache->lock);

    rc_entry* entry = rc_find(cache, key);
    if (!entry) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    uint64_t now = now_seconds();
    if ((int) (now - entry->timestamp) > cache->ttl_seconds) {
        rc_free_entry_value(entry);
        entry->occupied = false;
        cache->count--;
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    pthread_mutex_unlock(&cache->lock);
    return entry->value;
}

void remote_cache_put(remote_cache* cache, const char* key, const remote_completions* rc) {
    if (!cache || !key || !rc)
        return;

    pthread_mutex_lock(&cache->lock);

    rc_entry* existing = rc_find(cache, key);
    if (existing) {
        rc_free_entry_value(existing);
        existing->value = rc_deep_copy(rc);
        existing->timestamp = now_seconds();
        pthread_mutex_unlock(&cache->lock);
        return;
    }

    if (cache->count >= cache->capacity) {
        for (size_t i = 0; i < cache->capacity; i++) {
            size_t pos = (hash_key(key) + i) % cache->capacity;
            rc_entry* entry = &cache->table[pos];
            if (entry->occupied) {
                rc_free_entry_value(entry);
                entry->occupied = false;
                cache->count--;
                break;
            }
        }
    }

    uint64_t h = hash_key(key);
    size_t idx = h % cache->capacity;
    rc_entry* slot = NULL;

    for (size_t i = 0; i < cache->capacity; i++) {
        size_t pos = (idx + i) % cache->capacity;
        rc_entry* entry = &cache->table[pos];
        if (!entry->occupied) {
            slot = entry;
            break;
        }
    }

    if (!slot) {
        pthread_mutex_unlock(&cache->lock);
        return;
    }

    strncpy(slot->key, key, REMOTE_CACHE_MAX_KEY_LEN - 1);
    slot->key[REMOTE_CACHE_MAX_KEY_LEN - 1] = '\0';
    slot->value = rc_deep_copy(rc);
    slot->timestamp = now_seconds();
    slot->occupied = true;
    cache->count++;

    pthread_mutex_unlock(&cache->lock);
}

/* ── SSH path detection ───────────────────────────────────────────── */

bool is_ssh_path(const char* prefix) {
    if (!prefix || prefix[0] == '\0')
        return false;

    const char* first_slash = strchr(prefix, '/');
    size_t limit = first_slash ? (size_t) (first_slash - prefix) : strlen(prefix);

    for (size_t i = 0; i < limit; i++) {
        if (prefix[i] == '@')
            return true;
    }

    for (size_t i = 0; i < limit; i++) {
        if (prefix[i] == ':') {
            if (i == 1 && prefix[0] >= 'A' && prefix[0] <= 'Z')
                continue;
            return true;
        }
    }

    return false;
}

int parse_ssh_path(const char* prefix, char* user_buf, size_t user_len, char* host_buf,
                   size_t host_len, char* path_buf, size_t path_len) {
    if (!prefix)
        return -1;

    const char* at_sign = strchr(prefix, '@');
    const char* colon = NULL;

    if (at_sign) {
        const char* search_start = at_sign + 1;
        colon = strchr(search_start, ':');
    } else {
        colon = strchr(prefix, ':');
        if (colon && colon == prefix + 1 && prefix[0] >= 'A' && prefix[0] <= 'Z') {
            colon = NULL;
        }
    }

    if (!colon)
        return -1;

    const char* remote_path = colon + 1;

    if (at_sign && at_sign < colon) {
        size_t ulen = (size_t) (at_sign - prefix);
        if (user_buf && user_len > 0) {
            if (ulen >= user_len)
                ulen = user_len - 1;
            memcpy(user_buf, prefix, ulen);
            user_buf[ulen] = '\0';
        }
        size_t hlen = (size_t) (colon - at_sign - 1);
        if (host_buf && host_len > 0) {
            if (hlen >= host_len)
                hlen = host_len - 1;
            memcpy(host_buf, at_sign + 1, hlen);
            host_buf[hlen] = '\0';
        }
    } else {
        if (user_buf && user_len > 0)
            user_buf[0] = '\0';
        size_t hlen = (size_t) (colon - prefix);
        if (host_buf && host_len > 0) {
            if (hlen >= host_len)
                hlen = host_len - 1;
            memcpy(host_buf, prefix, hlen);
            host_buf[hlen] = '\0';
        }
    }

    if (path_buf && path_len > 0) {
        size_t plen = strlen(remote_path);
        if (plen >= path_len)
            plen = path_len - 1;
        memcpy(path_buf, remote_path, plen);
        path_buf[plen] = '\0';
    }

    return 0;
}

/* ── Non-blocking SSH execution ───────────────────────────────────── */

static remote_completions* parse_ls_output(FILE* pipe, const char* remote_prefix) {
    remote_completions* rc = calloc(1, sizeof(remote_completions));
    if (!rc)
        return NULL;

    rc->capacity = REMOTE_CACHE_MAX_ENTRIES_PER_ENTRY;
    rc->paths = calloc(rc->capacity, sizeof(char*));
    if (!rc->paths) {
        free(rc);
        return NULL;
    }

    char line[4096];
    size_t prefix_len = strlen(remote_prefix);

    while (fgets(line, sizeof(line), pipe)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0)
            continue;

        if (rc->count >= rc->capacity)
            break;

        char* full = malloc(prefix_len + len + 2);
        if (!full)
            continue;

        memcpy(full, remote_prefix, prefix_len);
        if (prefix_len > 0 && remote_prefix[prefix_len - 1] != '/' && line[0] != '/') {
            full[prefix_len] = '/';
            memcpy(full + prefix_len + 1, line, len + 1);
        } else {
            memcpy(full + prefix_len, line, len + 1);
        }

        rc->paths[rc->count] = full;
        rc->count++;
    }

    return rc;
}

remote_completions* ssh_get_completions(const char* prefix, int timeout_seconds,
                                        const char* default_user, remote_cache* cache) {
    if (!prefix || !is_ssh_path(prefix))
        return NULL;

    if (cache) {
        const remote_completions* cached = remote_cache_get(cache, prefix);
        if (cached)
            return rc_deep_copy(cached);
    }

    char user[256] = {0};
    char host[256] = {0};
    char remote_path[4096] = {0};

    if (parse_ssh_path(prefix, user, sizeof(user), host, sizeof(host), remote_path,
                       sizeof(remote_path)) != 0) {
        return NULL;
    }

    char target[256];
    if (user[0] != '\0') {
        snprintf(target, sizeof(target), "%s@%s", user, host);
    } else if (default_user && default_user[0] != '\0') {
        snprintf(target, sizeof(target), "%s@%s", default_user, host);
    } else {
        snprintf(target, sizeof(target), "%s", host);
    }

    char cmd[8192];
    if (remote_path[0] == '\0' || (remote_path[0] == '*' && remote_path[1] == '\0')) {
        snprintf(cmd, sizeof(cmd),
                 "ssh -o ConnectTimeout=%d -o BatchMode=yes -o StrictHostKeyChecking=no "
                 "-o UserKnownHostsFile=/dev/null %s \"ls -1 / 2>/dev/null\"",
                 timeout_seconds, target);
    } else {
        char dir_part[4096];
        char base_part[4096];
        strncpy(dir_part, remote_path, sizeof(dir_part) - 1);
        dir_part[sizeof(dir_part) - 1] = '\0';

        char* last_slash = strrchr(dir_part, '/');
        if (last_slash && last_slash != dir_part) {
            *last_slash = '\0';
            strncpy(base_part, last_slash + 1, sizeof(base_part) - 1);
        } else {
            base_part[0] = '\0';
            if (last_slash)
                dir_part[0] = '\0';
        }

        if (dir_part[0] == '\0')
            strcpy(dir_part, "/");

        char escaped_base[8192] = {0};
        size_t bi = 0;
        for (size_t i = 0; base_part[i] && bi < sizeof(escaped_base) - 1; i++) {
            if (base_part[i] == '*' || base_part[i] == '?' || base_part[i] == '[' ||
                base_part[i] == ']' || base_part[i] == ' ' || base_part[i] == '$' ||
                base_part[i] == '`' || base_part[i] == '"' || base_part[i] == '\\') {
                if (bi + 1 < sizeof(escaped_base) - 1)
                    escaped_base[bi++] = '\\';
            }
            escaped_base[bi++] = base_part[i];
        }
        escaped_base[bi] = '\0';

        char glob[8192];
        if (escaped_base[0] != '\0') {
            snprintf(glob, sizeof(glob), "%s/%s*", dir_part, escaped_base);
        } else {
            snprintf(glob, sizeof(glob), "%s/*", dir_part);
        }

        snprintf(cmd, sizeof(cmd),
                 "ssh -o ConnectTimeout=%d -o BatchMode=yes -o StrictHostKeyChecking=no "
                 "-o UserKnownHostsFile=/dev/null %s \"ls -1d %s 2>/dev/null\"",
                 timeout_seconds, target, glob);
    }

    FILE* pipe = popen(cmd, "r");
    if (!pipe)
        return NULL;

    int fd = fileno(pipe);
    if (fd < 0) {
        pclose(pipe);
        return NULL;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_seconds * 1000);

    remote_completions* result = NULL;

    if (ret > 0 && (pfd.revents & POLLIN)) {
        char remote_prefix[4096];
        if (remote_path[0] == '\0' || (remote_path[0] == '*' && remote_path[1] == '\0')) {
            snprintf(remote_prefix, sizeof(remote_prefix), "%s:", target);
        } else {
            char* last_slash = strrchr(remote_path, '/');
            if (last_slash && last_slash != remote_path) {
                *last_slash = '\0';
            } else if (last_slash) {
                remote_path[0] = '/';
                remote_path[1] = '\0';
            }
            snprintf(remote_prefix, sizeof(remote_prefix), "%s:%s", target, remote_path);
        }

        result = parse_ls_output(pipe, remote_prefix);
    }

    pclose(pipe);

    if (result && result->count > 0 && cache) {
        remote_cache_put(cache, prefix, result);
    }

    return result;
}

void remote_completions_free(remote_completions* rc) {
    if (!rc)
        return;
    if (rc->paths) {
        for (size_t i = 0; i < rc->count; i++) {
            free(rc->paths[i]);
        }
        free(rc->paths);
    }
    free(rc);
}
