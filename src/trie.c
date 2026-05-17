#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "trie.h"
#include <unistd.h>
#include <pthread.h>

#include "threadmanager.h"

static RadixChild* find_child(RadixNode* node, char c) {
    int lo = 0, hi = node->child_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        char mc = node->children[mid].edge_char;
        if (mc == c) return &node->children[mid];
        if (mc < c) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static void add_child(RadixNode* node, char c, RadixNode* child) {
    if (node->child_count >= node->child_capacity) {
        size_t new_cap = node->child_capacity * 2;
        RadixChild* new_children = malloc(new_cap * sizeof(RadixChild));
        if (!new_children) return;
        memcpy(new_children, node->children, node->child_count * sizeof(RadixChild));
        if (node->children != node->inline_storage) {
            free(node->children);
        }
        node->children = new_children;
        node->child_capacity = (uint8_t)new_cap;
    }

    /* Find insertion position to maintain sorted order by edge_char */
    int lo = 0, hi = node->child_count - 1;
    int pos = node->child_count;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (node->children[mid].edge_char < c) {
            lo = mid + 1;
        } else {
            pos = mid;
            hi = mid - 1;
        }
    }

    /* Shift elements right to make room */
    if (pos < node->child_count) {
        memmove(&node->children[pos + 1], &node->children[pos],
                (node->child_count - pos) * sizeof(RadixChild));
    }

    node->children[pos].edge_char = c;
    node->children[pos].node = child;
    node->child_count++;
}

static void remove_child_at(RadixNode* node, uint8_t idx) {
    if (idx >= node->child_count) return;
    for (uint8_t i = idx; i < node->child_count - 1; i++) {
        node->children[i] = node->children[i + 1];
    }
    node->child_count--;
}

Trie* create_trie(void) {
    RadixNode* node = calloc(1, sizeof(RadixNode));
    if (!node) return NULL;
    node->children = node->inline_storage;
    node->child_capacity = RADIX_INLINE_CHILDREN;
    node->child_count = 0;
    node->freq = 1;
    node->is_leaf = false;
    node->key = NULL;
    node->key_len = 0;
    return node;
}

void trie_free_recursive(Trie* node) {
    if (!node) return;
    for (uint8_t i = 0; i < node->child_count; i++) {
        trie_free_recursive(node->children[i].node);
    }
    if (node->children != node->inline_storage) {
        free(node->children);
    }
    free(node->key);
    free(node);
}

void insert(Trie* root, const char* str) {
    if (!root || !str || str[0] == '\0') return;

    uint64_t now = (uint64_t)time(NULL);

    RadixNode* curr = root;
    size_t i = 0;
    size_t len = strlen(str);

    while (i < len) {
        char c = str[i];
        RadixChild* child = find_child(curr, c);

        if (!child) {
            RadixNode* new_node = calloc(1, sizeof(RadixNode));
            if (!new_node) return;
            new_node->key = strdup(str + i);
            new_node->key_len = len - i;
            new_node->children = new_node->inline_storage;
            new_node->child_capacity = RADIX_INLINE_CHILDREN;
            new_node->is_leaf = true;
            new_node->freq = 1;
            new_node->last_access = now;
            new_node->is_dir = (str[len - 1] == '/');
            add_child(curr, c, new_node);
            return;
        }

        RadixNode* child_node = child->node;
        size_t match = 0;
        size_t edge_len = child_node->key_len;

        while (match < edge_len && i + match < len &&
               child_node->key[match] == str[i + match]) {
            match++;
        }

        if (match == edge_len) {
            curr = child_node;
            curr->freq++;
            curr->last_access = now;
            i += match;

            if (i == len) {
                curr->is_leaf = true;
                return;
            }
        } else if (match == 0) {
            RadixNode* new_node = calloc(1, sizeof(RadixNode));
            if (!new_node) return;
            new_node->key = strdup(str + i);
            new_node->key_len = len - i;
            new_node->children = new_node->inline_storage;
            new_node->child_capacity = RADIX_INLINE_CHILDREN;
            new_node->is_leaf = true;
            new_node->freq = 1;
            new_node->last_access = now;
            new_node->is_dir = (str[len - 1] == '/');
            add_child(curr, str[i], new_node);
            return;
        } else {
            RadixNode* split = calloc(1, sizeof(RadixNode));
            if (!split) return;
            split->key = strndup(child_node->key, match);
            split->key_len = match;
            split->children = split->inline_storage;
            split->child_capacity = RADIX_INLINE_CHILDREN;
            split->is_leaf = false;
            split->freq = child_node->freq;
            split->last_access = child_node->last_access;
            split->is_dir = false;

            memmove(child_node->key, child_node->key + match, child_node->key_len - match + 1);
            child_node->key_len -= match;

            uint8_t child_idx = 0;
            for (; child_idx < curr->child_count; child_idx++) {
                if (&curr->children[child_idx] == child) break;
            }
            curr->children[child_idx].edge_char = split->key[0];
            curr->children[child_idx].node = split;

            add_child(split, child_node->key[0], child_node);

            if (i + match < len) {
                RadixNode* new_node = calloc(1, sizeof(RadixNode));
                if (!new_node) return;
                new_node->key = strdup(str + i + match);
                new_node->key_len = len - i - match;
                new_node->children = new_node->inline_storage;
                new_node->child_capacity = RADIX_INLINE_CHILDREN;
                new_node->is_leaf = true;
                new_node->freq = 1;
                new_node->last_access = now;
                new_node->is_dir = (str[len - 1] == '/');
                add_child(split, str[i + match], new_node);
            }
            return;
        }
    }
}

Trie* search(Trie* root, state* scan, char* str) {
    (void)scan;
    if (!root || !str || str[0] == '\0') return NULL;

    uint64_t now = (uint64_t)time(NULL);

    RadixNode* curr = root;
    size_t i = 0;
    size_t len = strlen(str);

    while (i < len) {
        RadixChild* child = find_child(curr, str[i]);
        if (!child) return NULL;

        RadixNode* child_node = child->node;
        size_t edge_len = child_node->key_len;

        if (i + edge_len > len) return NULL;

        if (memcmp(child_node->key, str + i, edge_len) != 0) {
            return NULL;
        }

        curr = child_node;
        curr->freq++;
        curr->last_access = now;
        i += edge_len;
    }

    return curr;
}

/*
   Completion collection
*/

completions* completions_create(size_t capacity) {
    completions* c = calloc(1, sizeof(completions));
    if (!c) return NULL;
    c->paths = calloc(capacity, sizeof(char*));
    if (!c->paths) { free(c); return NULL; }
    c->capacity = capacity;
    c->count = 0;
    return c;
}

void completions_free(completions* c) {
    if (!c) return;
    for (size_t i = 0; i < c->count; i++) {
        free(c->paths[i]);
    }
    free(c->paths);
    free(c);
}

static RadixNode* find_prefix_node(Trie* root, const char* prefix, size_t* out_matched_in_node) {
    if (!root || !prefix || prefix[0] == '\0') {
        if (out_matched_in_node) *out_matched_in_node = 0;
        return root;
    }

    RadixNode* curr = root;
    size_t i = 0;
    size_t len = strlen(prefix);

    while (i < len) {
        RadixChild* child = find_child(curr, prefix[i]);
        if (!child) {
            if (out_matched_in_node) *out_matched_in_node = 0;
            return NULL;
        }

        RadixNode* child_node = child->node;
        size_t edge_len = child_node->key_len;

        if (i + edge_len > len) {
            size_t match_in_key = len - i;
            if (memcmp(child_node->key, prefix + i, match_in_key) == 0) {
                if (out_matched_in_node) *out_matched_in_node = match_in_key;
                return child_node;
            }
            if (out_matched_in_node) *out_matched_in_node = 0;
            return NULL;
        }

        if (memcmp(child_node->key, prefix + i, edge_len) != 0) {
            if (out_matched_in_node) *out_matched_in_node = 0;
            return NULL;
        }

        curr = child_node;
        i += edge_len;
    }

    if (out_matched_in_node) *out_matched_in_node = 0;
    return curr;
}

static void collect_dfs(RadixNode* node, char* buffer, size_t depth,
                        const char* prefix, completions* out) {
    if (!node || out->count >= out->capacity) return;

    if (node->is_leaf && depth > 0) {
        buffer[depth] = '\0';
        size_t plen = strlen(prefix);
        size_t slen = strlen(buffer);
        char* full = malloc(plen + slen + 1);
        if (full) {
            memcpy(full, prefix, plen);
            memcpy(full + plen, buffer, slen + 1);
            out->paths[out->count++] = full;
        }
    }

    for (uint8_t i = 0; i < node->child_count; i++) {
        RadixChild* child = &node->children[i];
        RadixNode* child_node = child->node;
        size_t klen = child_node->key_len;

        if (depth + klen >= 2048) continue;

        memcpy(buffer + depth, child_node->key, klen);
        collect_dfs(child_node, buffer, depth + klen, prefix, out);
        if (out->count >= out->capacity) return;
    }
}

void completions_collect(Trie* root, const char* prefix, completions* out) {
    if (!root || !out) return;

    size_t matched_in_node = 0;
    RadixNode* node = find_prefix_node(root, prefix, &matched_in_node);
    if (!node) return;

    char buffer[2048];
    size_t depth = 0;

    /* Skip if prefix ends with / - user wants contents, not the dir itself */
    size_t prefix_len = strlen(prefix);
    int prefix_is_dir = (prefix_len > 0 && prefix[prefix_len - 1] == '/') ? 1 : 0;

    if (node->is_leaf && matched_in_node == 0 && !prefix_is_dir) {
        size_t plen = strlen(prefix);
        char* full = malloc(plen + 1);
        if (full && out->count < out->capacity) {
            memcpy(full, prefix, plen + 1);
            out->paths[out->count++] = full;
        }
    }

    if (node != root && node->key && node->key_len > 0 && matched_in_node > 0) {
        size_t remaining_key = node->key_len - matched_in_node;
        if (remaining_key > 0 && remaining_key < 2048) {
            memcpy(buffer, node->key + matched_in_node, remaining_key);
            depth = remaining_key;

            if (node->is_leaf && !prefix_is_dir) {
                buffer[depth] = '\0';
                size_t plen = strlen(prefix);
                char* full = malloc(plen + depth + 1);
                if (full && out->count < out->capacity) {
                    memcpy(full, prefix, plen);
                    memcpy(full + plen, buffer, depth + 1);
                    out->paths[out->count++] = full;
                }
            }
        }
    }

    collect_dfs(node, buffer, depth, prefix, out);
}

/*
   Scored completion collection
*/

scored_completions* scored_completions_create(size_t capacity) {
    scored_completions* sc = calloc(1, sizeof(scored_completions));
    if (!sc) return NULL;
    sc->entries = calloc(capacity, sizeof(scored_entry));
    if (!sc->entries) { free(sc); return NULL; }
    sc->capacity = capacity;
    sc->count = 0;
    return sc;
}

void scored_completions_free(scored_completions* sc) {
    if (!sc) return;
    free(sc->entries);
    free(sc);
}

static int path_depth(const char* path) {
    int d = 0;
    for (const char* p = path; *p; p++) {
        if (*p == '/') d++;
    }
    return d;
}

static double compute_score(const char* path, uint64_t freq, uint64_t last_access, bool is_dir, uint64_t now, int max_depth) {
    double score = 0.0;

    /* Frequency: normalize to 0-1 range using log scale */
    double freq_norm = (freq > 0) ? (1.0 - 1.0 / (1.0 + (double)freq)) : 0.0;
    score += SCORE_WEIGHT_FREQ * freq_norm;

    /* Recency: exponential decay, half-life = 3600 seconds */
    double recency_norm = 0.0;
    if (last_access > 0 && now > last_access) {
        double age = (double)(now - last_access);
        recency_norm = 1.0 / (1.0 + age / 3600.0);
    } else if (last_access > 0) {
        recency_norm = 1.0;
    }
    score += SCORE_WEIGHT_RECENCY * recency_norm;

    /* Depth: shallower paths rank higher */
    int depth = path_depth(path);
    double depth_norm = (max_depth > 0) ? (1.0 - (double)depth / (double)max_depth) : 0.5;
    if (depth_norm < 0.0) depth_norm = 0.0;
    if (depth_norm > 1.0) depth_norm = 1.0;
    score += SCORE_WEIGHT_DEPTH * depth_norm;

    /* Type: files rank above directories */
    score += SCORE_WEIGHT_TYPE * (is_dir ? 0.0 : 1.0);

    return score;
}

static int cmp_path(const void* a, const void* b) {
    return strcmp(((const scored_entry*)a)->path, ((const scored_entry*)b)->path);
}

static void scored_insert(scored_completions* sc, const char* path, double score, uint64_t freq, uint64_t last_access, bool is_dir) {
    /* Binary search for duplicate */
    scored_entry key;
    memset(&key, 0, sizeof(key));
    strncpy(key.path, path, sizeof(key.path) - 1);
    key.path[sizeof(key.path) - 1] = '\0';
    scored_entry* found = bsearch(&key, sc->entries, sc->count, sizeof(scored_entry), cmp_path);
    if (found) return;

    if (sc->count < sc->capacity) {
        scored_entry* e = &sc->entries[sc->count];
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        e->score = score;
        e->freq = freq;
        e->last_access = last_access;
        e->is_dir = is_dir;
        sc->count++;
        size_t idx = sc->count - 1;
        while (idx > 0 && cmp_path(&sc->entries[idx - 1], &sc->entries[idx]) > 0) {
            scored_entry tmp = sc->entries[idx - 1];
            sc->entries[idx - 1] = sc->entries[idx];
            sc->entries[idx] = tmp;
            idx--;
        }
    } else if (score > sc->entries[0].score) {
        scored_entry* e = &sc->entries[0];
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        e->score = score;
        e->freq = freq;
        e->last_access = last_access;
        e->is_dir = is_dir;
        qsort(sc->entries, sc->count, sizeof(scored_entry), cmp_path);
    }
}

typedef struct {
    char buffer[2048];
    size_t depth;
    const char* prefix;
    scored_completions* out;
    uint64_t now;
    int max_depth;
} scored_dfs_ctx;

static void scored_collect_dfs(RadixNode* node, scored_dfs_ctx* ctx) {
    if (!node || ctx->out->count >= ctx->out->capacity * 2) return;

    if (node->is_leaf && ctx->depth > 0) {
        ctx->buffer[ctx->depth] = '\0';
        size_t plen = strlen(ctx->prefix);
        if (plen + ctx->depth < 4096) {
            char full[4096];
            memcpy(full, ctx->prefix, plen);
            memcpy(full + plen, ctx->buffer, ctx->depth + 1);

            double score = compute_score(full, node->freq, node->last_access, node->is_dir, ctx->now, ctx->max_depth);
            scored_insert(ctx->out, full, score, node->freq, node->last_access, node->is_dir);
        }
    }

    for (uint8_t i = 0; i < node->child_count; i++) {
        RadixChild* child = &node->children[i];
        RadixNode* child_node = child->node;
        size_t klen = child_node->key_len;

        if (ctx->depth + klen >= 2048) continue;

        memcpy(ctx->buffer + ctx->depth, child_node->key, klen);
        size_t prev_depth = ctx->depth;
        ctx->depth += klen;
        scored_collect_dfs(child_node, ctx);
        ctx->depth = prev_depth;
    }
}

static int cmp_score_desc(const void* a, const void* b) {
    const scored_entry* ea = (const scored_entry*)a;
    const scored_entry* eb = (const scored_entry*)b;
    if (eb->score > ea->score) return 1;
    if (eb->score < ea->score) return -1;
    return strcmp(ea->path, eb->path);
}

void scored_completions_collect(Trie* root, const char* prefix, scored_completions* out, uint64_t now) {
    if (!root || !out) return;

    size_t matched_in_node = 0;
    RadixNode* node = find_prefix_node(root, prefix, &matched_in_node);
    if (!node) return;

    scored_dfs_ctx ctx;
    ctx.depth = 0;
    ctx.prefix = prefix;
    ctx.out = out;
    ctx.now = now;
    ctx.max_depth = 0;

    /* Estimate max depth from prefix */
    ctx.max_depth = path_depth(prefix) + 10;

    /* Check if the prefix node itself is a leaf */
    /* Skip if prefix ends with / - user wants contents, not the dir itself */
    size_t prefix_len = strlen(prefix);
    int prefix_is_dir = (prefix_len > 0 && prefix[prefix_len - 1] == '/') ? 1 : 0;
    if (node->is_leaf && matched_in_node == 0 && !prefix_is_dir) {
        double score = compute_score(prefix, node->freq, node->last_access, node->is_dir, now, ctx.max_depth);
        scored_insert(out, prefix, score, node->freq, node->last_access, node->is_dir);
    }

    if (node != root && node->key && node->key_len > 0 && matched_in_node > 0) {
        size_t remaining_key = node->key_len - matched_in_node;
        if (remaining_key > 0 && remaining_key < 2048) {
            memcpy(ctx.buffer, node->key + matched_in_node, remaining_key);
            ctx.depth = remaining_key;

            if (node->is_leaf && !prefix_is_dir) {
                ctx.buffer[ctx.depth] = '\0';
                size_t plen = strlen(prefix);
                if (plen + ctx.depth < 4096) {
                    char full[4096];
                    memcpy(full, prefix, plen);
                    memcpy(full + plen, ctx.buffer, ctx.depth + 1);
                    double score = compute_score(full, node->freq, node->last_access, node->is_dir, now, ctx.max_depth);
                    scored_insert(out, full, score, node->freq, node->last_access, node->is_dir);
                }
            }
        }
    }

    scored_collect_dfs(node, &ctx);

    /* Sort by score descending */
    qsort(out->entries, out->count, sizeof(scored_entry), cmp_score_desc);
}

/*
   Fuzzy matching: subsequence match against all leaf paths
*/

typedef struct {
    char path[4096];
    int match_quality;
    uint64_t freq;
    uint64_t last_access;
    bool is_dir;
} fuzzy_entry;

static int fuzzy_score(const char* path, const char* query) {
    int path_len = (int)strlen(path);
    int query_len = (int)strlen(query);
    if (query_len == 0 || path_len < query_len) return -1;

    const char* basename = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') basename = p + 1;
    }

    int matches = 0;
    int contiguous = 0;
    int best_contiguous = 0;
    int bn_len = (int)strlen(basename);

    int bi = 0;
    int qi = 0;
    while (qi < query_len && bi < bn_len) {
        if (tolower((unsigned char)basename[bi]) == tolower((unsigned char)query[qi])) {
            matches++;
            contiguous++;
            if (contiguous > best_contiguous) best_contiguous = contiguous;
            qi++;
        } else {
            contiguous = 0;
        }
        bi++;
    }

    if (matches != query_len) return -1;

    int skipped = bi - matches;
    return matches * 10 + best_contiguous * 5 - skipped;
}

static void fuzzy_collect_dfs(RadixNode* node, char* buffer, size_t depth,
                               const char* query, fuzzy_entry* entries,
                               size_t* count, size_t capacity, int* min_score) {
    if (!node || *count >= capacity * 2) return;

    if (node->is_leaf && depth > 0) {
        buffer[depth] = '\0';
        int score = fuzzy_score(buffer, query);
        if (score >= 0 && score >= *min_score) {
            if (*count < capacity) {
                fuzzy_entry* e = &entries[*count];
                strncpy(e->path, buffer, sizeof(e->path) - 1);
                e->path[sizeof(e->path) - 1] = '\0';
                e->match_quality = score;
                e->freq = node->freq;
                e->last_access = node->last_access;
                e->is_dir = node->is_dir;
                (*count)++;
            }
            if (*count >= capacity) {
                *min_score = entries[0].match_quality;
            }
        }
    }

    for (uint8_t i = 0; i < node->child_count; i++) {
        RadixChild* child = &node->children[i];
        RadixNode* child_node = child->node;
        size_t klen = child_node->key_len;
        if (depth + klen >= 2048) continue;
        memcpy(buffer + depth, child_node->key, klen);
        fuzzy_collect_dfs(child_node, buffer, depth + klen, query,
                          entries, count, capacity, min_score);
    }
}

static int cmp_fuzzy(const void* a, const void* b) {
    const fuzzy_entry* ea = (const fuzzy_entry*)a;
    const fuzzy_entry* eb = (const fuzzy_entry*)b;
    if (eb->match_quality != ea->match_quality)
        return eb->match_quality - ea->match_quality;
    return strcmp(ea->path, eb->path);
}

int trie_fuzzy_collect(Trie* root, const char* query, char** paths, int capacity) {
    if (!root || !query || query[0] == '\0' || !paths || capacity <= 0) return 0;

    fuzzy_entry* entries = calloc((size_t)capacity, sizeof(fuzzy_entry));
    if (!entries) return 0;

    size_t count = 0;
    int min_score = 0;
    char buffer[2048];

    fuzzy_collect_dfs(root, buffer, 0, query, entries, &count, (size_t)capacity, &min_score);

    qsort(entries, count, sizeof(fuzzy_entry), cmp_fuzzy);

    int n = (int)(count < (size_t)capacity ? count : (size_t)capacity);
    for (int i = 0; i < n; i++) {
        paths[i] = strdup(entries[i].path);
        if (!paths[i]) {
            for (int j = 0; j < i; j++) free(paths[j]);
            free(entries);
            return i;
        }
    }

    free(entries);
    return n;
}
