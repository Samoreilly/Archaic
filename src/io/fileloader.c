#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../ipc/server.h"
#include "../cache.h"
#include "../config.h"
#include "../log.h"
#include "../lru.h"
#include "../metrics.h"
#include "../scanner.h"
#include "../threadmanager.h"
#include "../trie-storage.h"
#include "fileloader.h"

/* ── Binary state persistence ──────────────────────────────────────
 * Format:
 *   Header (16 bytes):
 *     magic      uint32 = 0x41525354 ("ARST")
 *     version    uint32 = 1
 *     bucket_cnt uint32
 *     _reserved  uint32
 *
 *   Per bucket:
 *     dir_name_len  uint32
 *     dir_name      char[dir_name_len]
 *     dir_count     uint32
 *     node_count    uint32
 *
 *     Per node (DFS pre-order, index = serialization order):
 *       key_len      uint32
 *       key          char[key_len]
 *       child_count  uint8
 *       freq         uint64
 *       last_access  uint64
 *       is_leaf      uint8
 *       is_dir       uint8
 *       children[]   child_count × {edge_char: uint8, child_index: uint32}
 * ────────────────────────────────────────────────────────────────── */

#define STATE_MAGIC 0x41525354U
#define STATE_VERSION 2U

/* Simple FNV-1a hash for state file integrity checking */
__attribute__((unused)) static uint32_t fnv1a_hash(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*) data;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static size_t count_trie_nodes(const RadixNode* node) {
    if (!node)
        return 0;
    size_t count = 1;
    for (uint8_t i = 0; i < node->child_count; i++)
        count += count_trie_nodes(node->children[i].node);
    return count;
}

typedef struct {
    const RadixNode* node;
    uint32_t index;
} IndexedNode;

static void assign_indices_dfs(const RadixNode* node, IndexedNode* arr, uint32_t* idx) {
    if (!node)
        return;
    arr[*idx].node = node;
    arr[*idx].index = *idx;
    (*idx)++;
    for (uint8_t i = 0; i < node->child_count; i++)
        assign_indices_dfs(node->children[i].node, arr, idx);
}

static int find_child_index(const IndexedNode* arr, uint32_t n, const RadixNode* child) {
    for (uint32_t i = 0; i < n; i++)
        if (arr[i].node == child)
            return (int) i;
    return -1;
}

int save_trie(daemon_state* state, const char* path) {
    if (!state || !state->store || !path)
        return -1;

    FILE* f = fopen(path, "wb");
    if (!f)
        return -1;

    store_lock(state->store);
    uint32_t bucket_count = (uint32_t) state->store->right_index;

    uint32_t magic = STATE_MAGIC, version = STATE_VERSION, reserved = 0;
    if (fwrite(&magic, sizeof(magic), 1, f) != 1)
        goto werr;
    if (fwrite(&version, sizeof(version), 1, f) != 1)
        goto werr;
    if (fwrite(&bucket_count, sizeof(bucket_count), 1, f) != 1)
        goto werr;
    if (fwrite(&reserved, sizeof(reserved), 1, f) != 1)
        goto werr;

    for (uint32_t b = 0; b < bucket_count; b++) {
        t_bucket* bucket = state->store->buckets[b];
        if (!bucket || !bucket->dir_trie)
            continue;

        trie_lock(bucket);

        uint32_t dn_len = (uint32_t) strlen(bucket->dir_name);
        if (fwrite(&dn_len, sizeof(dn_len), 1, f) != 1) {
            trie_unlock(bucket);
            goto werr;
        }
        if (fwrite(bucket->dir_name, 1, dn_len, f) != dn_len) {
            trie_unlock(bucket);
            goto werr;
        }
        if (fwrite(&bucket->dir_count, sizeof(bucket->dir_count), 1, f) != 1) {
            trie_unlock(bucket);
            goto werr;
        }

        uint32_t nc = (uint32_t) count_trie_nodes(bucket->dir_trie);
        if (fwrite(&nc, sizeof(nc), 1, f) != 1) {
            trie_unlock(bucket);
            goto werr;
        }

        if (nc > 0) {
            IndexedNode* indexed = malloc(nc * sizeof(IndexedNode));
            if (!indexed) {
                trie_unlock(bucket);
                goto werr;
            }
            uint32_t idx = 0;
            assign_indices_dfs(bucket->dir_trie, indexed, &idx);

            for (uint32_t i = 0; i < nc; i++) {
                const RadixNode* nd = indexed[i].node;

                uint32_t kl = (uint32_t) nd->key_len;
                if (fwrite(&kl, sizeof(kl), 1, f) != 1) {
                    free(indexed);
                    trie_unlock(bucket);
                    goto werr;
                }
                if (kl > 0 && fwrite(nd->key, 1, kl, f) != kl) {
                    free(indexed);
                    trie_unlock(bucket);
                    goto werr;
                }

                uint8_t cc = nd->child_count;
                if (fwrite(&cc, sizeof(cc), 1, f) != 1) {
                    free(indexed);
                    trie_unlock(bucket);
                    goto werr;
                }
                if (fwrite(&nd->freq, sizeof(nd->freq), 1, f) != 1) {
                    free(indexed);
                    trie_unlock(bucket);
                    goto werr;
                }
                if (fwrite(&nd->last_access, sizeof(nd->last_access), 1, f) != 1) {
                    free(indexed);
                    trie_unlock(bucket);
                    goto werr;
                }

                uint8_t il = (uint8_t) nd->is_leaf, id = (uint8_t) nd->is_dir;
                if (fwrite(&il, sizeof(il), 1, f) != 1) {
                    free(indexed);
                    trie_unlock(bucket);
                    goto werr;
                }
                if (fwrite(&id, sizeof(id), 1, f) != 1) {
                    free(indexed);
                    trie_unlock(bucket);
                    goto werr;
                }

                for (uint8_t c = 0; c < nd->child_count; c++) {
                    uint8_t ec = (uint8_t) nd->children[c].edge_char;
                    if (fwrite(&ec, sizeof(ec), 1, f) != 1) {
                        free(indexed);
                        trie_unlock(bucket);
                        goto werr;
                    }
                    int ci = find_child_index(indexed, nc, nd->children[c].node);
                    uint32_t ci32 = (uint32_t) ci;
                    if (fwrite(&ci32, sizeof(ci32), 1, f) != 1) {
                        free(indexed);
                        trie_unlock(bucket);
                        goto werr;
                    }
                }
            }
            free(indexed);
        }
        trie_unlock(bucket);
    }
    store_unlock(state->store);
    fclose(f);
    return 0;

werr:
    store_unlock(state->store);
    fclose(f);
    return -1;
}

typedef struct {
    uint8_t edge_char;
    uint32_t child_index;
} ChildRef;

typedef struct {
    ChildRef* refs;
    uint8_t count;
} ChildRefs;

int load_trie(daemon_state* state, const char* path) {
    if (!state || !state->store || !path)
        return -1;

    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;

    uint32_t magic, version, bucket_count, reserved;
    if (fread(&magic, sizeof(magic), 1, f) != 1)
        goto rerr;
    if (fread(&version, sizeof(version), 1, f) != 1)
        goto rerr;
    if (fread(&bucket_count, sizeof(bucket_count), 1, f) != 1)
        goto rerr;
    if (fread(&reserved, sizeof(reserved), 1, f) != 1)
        goto rerr;

    if (magic != STATE_MAGIC || version != STATE_VERSION) {
        if (magic == STATE_MAGIC && version != STATE_VERSION) {
            LOG_WARN("daemon", "state file version %u != expected %u, discarding old state",
                     version, STATE_VERSION);
        }
        goto rerr;
    }

    for (uint32_t b = 0; b < bucket_count; b++) {
        uint32_t dn_len;
        if (fread(&dn_len, sizeof(dn_len), 1, f) != 1)
            goto rerr;

        char* dir_name = malloc(dn_len + 1);
        if (!dir_name)
            goto rerr;
        if (fread(dir_name, 1, dn_len, f) != dn_len) {
            free(dir_name);
            goto rerr;
        }
        dir_name[dn_len] = '\0';

        uint32_t dir_count, node_count;
        if (fread(&dir_count, sizeof(dir_count), 1, f) != 1) {
            free(dir_name);
            goto rerr;
        }
        if (fread(&node_count, sizeof(node_count), 1, f) != 1) {
            free(dir_name);
            goto rerr;
        }

        store_lock(state->store);
        t_bucket* bucket = insert_bucket(state->store, dir_name);
        store_unlock(state->store);
        free(dir_name);
        if (!bucket)
            goto rerr;
        bucket->dir_count = dir_count;

        if (node_count == 0)
            continue;

        RadixNode** nodes = calloc(node_count, sizeof(RadixNode*));
        ChildRefs* crefs = calloc(node_count, sizeof(ChildRefs));
        if (!nodes || !crefs) {
            free(nodes);
            free(crefs);
            goto rerr;
        }

        for (uint32_t i = 0; i < node_count; i++) {
            uint32_t kl;
            if (fread(&kl, sizeof(kl), 1, f) != 1)
                goto bucket_err;

            RadixNode* nd = calloc(1, sizeof(RadixNode));
            if (!nd)
                goto bucket_err;

            if (kl > 0) {
                nd->key = malloc(kl + 1);
                if (!nd->key) {
                    free(nd);
                    goto bucket_err;
                }
                if (fread(nd->key, 1, kl, f) != kl) {
                    free(nd->key);
                    free(nd);
                    goto bucket_err;
                }
                nd->key[kl] = '\0';
            }
            nd->key_len = kl;

            uint8_t cc;
            if (fread(&cc, sizeof(cc), 1, f) != 1)
                goto bucket_err;
            nd->child_count = cc;

            if (fread(&nd->freq, sizeof(nd->freq), 1, f) != 1)
                goto bucket_err;
            if (fread(&nd->last_access, sizeof(nd->last_access), 1, f) != 1)
                goto bucket_err;

            uint8_t il, id;
            if (fread(&il, sizeof(il), 1, f) != 1)
                goto bucket_err;
            if (fread(&id, sizeof(id), 1, f) != 1)
                goto bucket_err;
            nd->is_leaf = (bool) il;
            nd->is_dir = (bool) id;

            if (cc > 0) {
                nd->children = malloc(cc * sizeof(RadixChild));
                if (!nd->children)
                    goto bucket_err;
                nd->child_capacity = cc;

                crefs[i].refs = malloc(cc * sizeof(ChildRef));
                if (!crefs[i].refs)
                    goto bucket_err;
                crefs[i].count = cc;

                for (uint8_t c = 0; c < cc; c++) {
                    uint8_t ec;
                    uint32_t ci;
                    if (fread(&ec, sizeof(ec), 1, f) != 1)
                        goto bucket_err;
                    if (fread(&ci, sizeof(ci), 1, f) != 1)
                        goto bucket_err;
                    nd->children[c].edge_char = (char) ec;
                    nd->children[c].node = NULL;
                    crefs[i].refs[c].edge_char = ec;
                    crefs[i].refs[c].child_index = ci;
                }
            } else {
                nd->children = nd->inline_storage;
                nd->child_capacity = RADIX_INLINE_CHILDREN;
            }

            nodes[i] = nd;
        }

        for (uint32_t i = 0; i < node_count; i++) {
            for (uint8_t c = 0; c < crefs[i].count; c++) {
                uint32_t ci = crefs[i].refs[c].child_index;
                if (ci < node_count)
                    nodes[i]->children[c].node = nodes[ci];
            }
        }

        trie_lock(bucket);
        if (bucket->dir_trie) {
            if (bucket->dir_trie->children != bucket->dir_trie->inline_storage)
                free(bucket->dir_trie->children);
            free(bucket->dir_trie->key);
            free(bucket->dir_trie);
        }
        bucket->dir_trie = nodes[0];
        trie_unlock(bucket);

        for (uint32_t i = 0; i < node_count; i++) {
            if (crefs[i].refs)
                free(crefs[i].refs);
        }
        free(crefs);
        free(nodes);
        continue;

    bucket_err:
        for (uint32_t i = 0; i < node_count; i++) {
            if (nodes[i]) {
                if (nodes[i]->children != nodes[i]->inline_storage)
                    free(nodes[i]->children);
                free(nodes[i]->key);
                free(nodes[i]);
            }
            if (crefs[i].refs)
                free(crefs[i].refs);
        }
        free(nodes);
        free(crefs);
        goto rerr;
    }

    fclose(f);
    return 0;

rerr:
    fclose(f);
    return -1;
}

void daemon_save_state(daemon_state* state, const char* path) {
    if (!path || path[0] == '\0')
        return;
    char tmp_path[4100];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, getpid());
    save_trie(state, tmp_path);
    rename(tmp_path, path);
}

static void collect_frequencies_dfs(RadixNode* node, char* buffer, size_t depth, FILE* f) {
    if (!node || !f)
        return;

    if (node->key && node->key_len > 0) {
        if (depth + node->key_len < 4096) {
            memcpy(buffer + depth, node->key, node->key_len);
            depth += node->key_len;
        }
    }

    if (node->is_leaf && depth > 0) {
        buffer[depth] = '\0';
        fprintf(f, "%s\t%llu\t%llu\t%d\n", buffer, (unsigned long long) node->freq,
                (unsigned long long) node->last_access, node->is_dir);
    }

    for (uint8_t i = 0; i < node->child_count; i++) {
        collect_frequencies_dfs(node->children[i].node, buffer, depth, f);
    }
}

int daemon_export_frequencies(daemon_state* state, const char* path) {
    if (!state || !state->store || !path)
        return -1;

    FILE* f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "# Archaic frequency export\n");
    fprintf(f, "# Format: path<tab>freq<tab>last_access<tab>is_dir\n");

    store_lock(state->store);
    size_t bucket_count = state->store->right_index;

    for (size_t b = 0; b < bucket_count; b++) {
        t_bucket* bucket = state->store->buckets[b];
        if (!bucket || !bucket->dir_trie)
            continue;

        trie_lock(bucket);
        char buffer[4096];
        if (bucket->dir_name) {
            size_t dn_len = strlen(bucket->dir_name);
            if (dn_len < 4096) {
                memcpy(buffer, bucket->dir_name, dn_len);
                collect_frequencies_dfs(bucket->dir_trie, buffer, dn_len, f);
            }
        }
        trie_unlock(bucket);
    }

    store_unlock(state->store);
    fclose(f);
    return 0;
}

int daemon_import_frequencies(daemon_state* state, const char* path) {
    if (!state || !state->store || !path)
        return -1;

    FILE* f = fopen(path, "r");
    if (!f)
        return -1;

    char line[8192];
    int imported = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;

        char file_path[4096];
        uint64_t freq, last_access;
        int is_dir;

        if (sscanf(line, "%4095[^\t]\t%llu\t%llu\t%d", file_path, (unsigned long long*) &freq,
                   (unsigned long long*) &last_access, &is_dir) >= 2) {
            path_validation val = validate_input_path("", file_path);
            if (val.exists && val.full_path) {
                store_lock(state->store);
                t_bucket* bucket =
                    find_bucket(state->store, val.full_path, val.full_path, 3, false);
                if (bucket) {
                    trie_lock(bucket);
                    RadixNode* node = search(bucket->dir_trie, NULL, val.full_path);
                    if (node) {
                        node->freq = freq;
                        node->last_access = last_access;
                    }
                    trie_unlock(bucket);
                }
                store_unlock(state->store);
                imported++;
            }
            free_path_validation(&val);
        }
    }

    fclose(f);
    return imported;
}

typedef struct {
    FILE* f;
    int first;
} json_ctx;

static void collect_json_dfs(RadixNode* node, char* buf, size_t pos, json_ctx* ctx) {
    if (!node)
        return;
    if (pos + node->key_len >= 4095)
        return;

    memcpy(buf + pos, node->key, node->key_len);
    pos += node->key_len;
    buf[pos] = '\0';

    if (node->is_leaf && node->freq > 0) {
        if (!ctx->first)
            fprintf(ctx->f, ",\n");
        ctx->first = 0;
        fprintf(ctx->f, "    {\"path\": \"");
        for (size_t i = 0; i < pos; i++) {
            if (buf[i] == '"')
                fprintf(ctx->f, "\\\"");
            else if (buf[i] == '\\')
                fprintf(ctx->f, "\\\\");
            else
                fputc(buf[i], ctx->f);
        }
        fprintf(ctx->f, "\", \"freq\": %llu, \"last_access\": %llu, \"is_dir\": %s}",
                (unsigned long long) node->freq, (unsigned long long) node->last_access,
                node->is_dir ? "true" : "false");
    }

    for (uint8_t i = 0; i < node->child_count; i++)
        collect_json_dfs(node->children[i].node, buf, pos, ctx);
}

int daemon_export_json(daemon_state* state, const char* path) {
    if (!state || !state->store || !path)
        return -1;

    FILE* f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "{\n  \"version\": 2,\n  \"entries\": [\n");

    json_ctx ctx = {.f = f, .first = 1};
    char buffer[4096];

    store_lock(state->store);
    size_t bucket_count = state->store->right_index;

    for (size_t b = 0; b < bucket_count; b++) {
        t_bucket* bucket = state->store->buckets[b];
        if (!bucket || !bucket->dir_trie)
            continue;

        trie_lock(bucket);
        if (bucket->dir_name) {
            size_t dn_len = strlen(bucket->dir_name);
            if (dn_len < 4096) {
                memcpy(buffer, bucket->dir_name, dn_len);
                buffer[dn_len] = '\0';
                collect_json_dfs(bucket->dir_trie, buffer, dn_len, &ctx);
            }
        }
        trie_unlock(bucket);
    }

    store_unlock(state->store);
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    return 0;
}

path_validation process_input(t_bucket_store* store, const char* cwd, const char* input) {
    path_validation validation = validate_input_path(cwd, input);

    if (!validation.exists || !validation.full_path) {
        return validation;
    }

    store_lock(store);
    t_bucket* bucket = find_bucket(store, validation.full_path, validation.full_path, 3, false);
    if (bucket) {
        if (bucket->dir_count >= store->max_nodes_per_bucket) {
            trie_unlock(bucket);
            store_unlock(store);
            return validation;
        }
        trie_lock(bucket);
        if (validation.is_dir) {
            size_t len = strlen(validation.full_path);
            char dir_path[4096];
            memcpy(dir_path, validation.full_path, len);
            dir_path[len] = '/';
            dir_path[len + 1] = '\0';
            insert(bucket->dir_trie, dir_path);
        } else {
            insert(bucket->dir_trie, validation.full_path);
        }
        bucket->dir_count++;
        atomic_fetch_add(&store->total_nodes, 1);
        trie_unlock(bucket);
    }
    store_unlock(store);

    return validation;
}

daemon_state* daemon_init(void) {
    daemon_state* state = (daemon_state*) calloc(1, sizeof(daemon_state));
    if (!state) {
        return NULL;
    }

    archaic_config cfg;
    config_init_defaults(&cfg);
    config_load_default(&cfg);

    state->store = (t_bucket_store*) calloc(1, sizeof(t_bucket_store));
    if (!state->store) {
        free(state);
        return NULL;
    }

    pthread_mutex_init(&state->store->store_lock, NULL);

    state->parent = (struct node*) calloc(1, sizeof(struct node));
    if (!state->parent) {
        free(state->store);
        free(state);
        return NULL;
    }

    state->parent->is_parent = true;
    state->store->parent = state->parent;

    state->store->max_buckets = BUCKETS;
    state->store->max_nodes_per_bucket = cfg.storage.max_nodes_per_bucket;
    state->store->max_total_nodes = cfg.storage.max_total_nodes;
    atomic_store(&state->store->total_nodes, 0);
    atomic_store(&state->store->estimated_memory_bytes, 0);

    parallel_scanner_init(&state->scanner, state->store, state->parent, cfg.daemon.max_depth,
                          cfg.daemon.scan_threads);
    atomic_store(&state->scanner_healthy, true);
    atomic_store(&state->scanning, false);
    atomic_store(&state->scan_bucket_count, 0);

    const char* ignore_dirs[64];
    for (int i = 0; i < cfg.scanner.ignore_dir_count && i < 64; i++)
        ignore_dirs[i] = cfg.scanner.ignore_dirs[i];
    const char* ignore_files[64];
    for (int i = 0; i < cfg.scanner.ignore_file_count && i < 64; i++)
        ignore_files[i] = cfg.scanner.ignore_files[i];
    parallel_scanner_set_ignores(&state->scanner, ignore_dirs, cfg.scanner.ignore_dir_count,
                                 ignore_files, cfg.scanner.ignore_file_count);

    metrics_init(&state->metrics);

    state->cache = cache_create(cfg.storage.cache_max_entries, cfg.storage.cache_ttl_seconds);

    recent_files_init(&state->recent, (int) cfg.storage.recent_files_capacity);

    incremental_init(&state->incremental);

    state->case_insensitive = cfg.storage.case_insensitive;

    state->bookmark_count =
        cfg.bookmarks.count < CONFIG_MAX_BOOKMARKS ? cfg.bookmarks.count : CONFIG_MAX_BOOKMARKS;
    for (int i = 0; i < state->bookmark_count; i++) {
        strncpy(state->bookmarks[i], cfg.bookmarks.paths[i], CONFIG_MAX_STRING - 1);
        state->bookmarks[i][CONFIG_MAX_STRING - 1] = '\0';
    }

    state->rescan_interval_seconds = cfg.daemon.rescan_interval_seconds;
    atomic_store(&state->rescan_timer_running, false);
    atomic_store(&state->config_reload_requested, false);

    state->last_scan_path_count = 0;
    if (cfg.daemon.scan_path_count > 0) {
        state->last_scan_path_count = cfg.daemon.scan_path_count;
        for (int i = 0; i < cfg.daemon.scan_path_count; i++) {
            strncpy(state->last_scan_paths[i], cfg.daemon.scan_paths[i],
                    sizeof(state->last_scan_paths[i]) - 1);
            state->last_scan_paths[i][sizeof(state->last_scan_paths[i]) - 1] = '\0';
        }
    } else if (cfg.daemon.scan_path[0] != '\0') {
        state->last_scan_path_count = 1;
        strncpy(state->last_scan_paths[0], cfg.daemon.scan_path,
                sizeof(state->last_scan_paths[0]) - 1);
        state->last_scan_paths[0][sizeof(state->last_scan_paths[0]) - 1] = '\0';
    }

    const char* state_path = "/tmp/archaic-state.bin";
    FILE* sf = fopen(state_path, "rb");
    if (sf) {
        fclose(sf);
        LOG_INFO("daemon", "loading state from %s...", state_path);
        if (load_trie(state, state_path) == 0)
            LOG_INFO("daemon", "state loaded. %zu buckets restored.", state->store->right_index);
    }

    return state;
}

void daemon_shutdown(daemon_state* state) {
    if (!state) {
        return;
    }

    daemon_stop_rescan_timer(state);

    if (state->ipc) {
        ipc_server_stop(state->ipc);
        state->ipc = NULL;
    }

    /* Wait for background scan to finish */
    if (atomic_load(&state->scanning)) {
        pthread_join(state->scan_thread, NULL);
    }

    parallel_scanner_stop(&state->scanner);
    if (state->scanner.queue) {
        pthread_mutex_destroy(&state->scanner.queue->queue_lock);
        pthread_cond_destroy(&state->scanner.queue->queue_not_empty);
        free(state->scanner.queue);
    }

    if (state->cache) {
        cache_destroy(state->cache);
        state->cache = NULL;
    }

    const char* state_path = "/tmp/archaic-state.bin";
    LOG_INFO("daemon", "saving state to %s...", state_path);
    save_trie(state, state_path);

    if (state->store) {
        for (size_t i = 0; i < state->store->right_index; i++) {
            t_bucket* bucket = state->store->buckets[i];
            if (bucket) {
                destroy_bucket(bucket);
            }
        }
        pthread_mutex_destroy(&state->store->store_lock);
        free(state->store);
    }

    if (state->parent) {
        free(state->parent);
    }

    free(state);
}

typedef struct {
    daemon_state* state;
    char** paths;
    int path_count;
} scan_thread_ctx;

/* ── Periodic rescan timer thread ────────────────────────────────── */

static void* rescan_timer_func(void* arg) {
    daemon_state* state = (daemon_state*) arg;

    while (atomic_load(&state->rescan_timer_running)) {
        /* Sleep in 1-second increments so we can check running flag */
        for (int i = 0;
             i < state->rescan_interval_seconds && atomic_load(&state->rescan_timer_running); i++) {
            sleep(1);
        }

        if (!atomic_load(&state->rescan_timer_running))
            break;

        /* Trigger a rescan if not already scanning */
        if (!atomic_load(&state->scanning) && state->last_scan_path_count > 0) {
            LOG_INFO("scanner", "periodic rescan triggered (%d roots)",
                     state->last_scan_path_count);
            const char* roots[CONFIG_MAX_ROOTS];
            for (int i = 0; i < state->last_scan_path_count; i++) {
                roots[i] = state->last_scan_paths[i];
            }
            daemon_run_scan_multi(state, roots, state->last_scan_path_count);
        }
    }
    return NULL;
}

static void* scan_thread_func(void* arg) {
    scan_thread_ctx* ctx = (scan_thread_ctx*) arg;
    daemon_state* state = ctx->state;
    char** paths = ctx->paths;
    int path_count = ctx->path_count;
    free(ctx);

    atomic_store(&state->scanning, true);
    atomic_store(&state->scan_bucket_count, 0);

    metrics_record_scan(&state->metrics);

    archaic_config cfg;
    config_init_defaults(&cfg);
    config_load_default(&cfg);
    if (path_count > 0) {
        config_load_archaicignore(&cfg, paths[0]);
    }

    const char* ignore_dirs[64];
    for (int i = 0; i < cfg.scanner.ignore_dir_count && i < 64; i++)
        ignore_dirs[i] = cfg.scanner.ignore_dirs[i];
    const char* ignore_files[64];
    for (int i = 0; i < cfg.scanner.ignore_file_count && i < 64; i++)
        ignore_files[i] = cfg.scanner.ignore_files[i];
    parallel_scanner_set_ignores(&state->scanner, ignore_dirs, cfg.scanner.ignore_dir_count,
                                 ignore_files, cfg.scanner.ignore_file_count);

    if (path_count == 1) {
        parallel_scanner_start(&state->scanner, paths[0]);
    } else {
        parallel_scanner_start_multi(&state->scanner, (const char**) paths, path_count);
    }
    parallel_scanner_wait(&state->scanner);

    atomic_store(&state->scan_bucket_count, state->store->right_index);
    atomic_store(&state->scanning, false);

    update_memory_estimate(state->store);
    if (state->cache)
        cache_invalidate(state->cache);

    for (int i = 0; i < path_count; i++) {
        free(paths[i]);
    }
    free(paths);
    return NULL;
}

void daemon_run_scan(daemon_state* state, const char* path) {
    if (!state || !path)
        return;

    if (atomic_load(&state->scanning))
        return;

    state->last_scan_path_count = 1;
    strncpy(state->last_scan_paths[0], path, sizeof(state->last_scan_paths[0]) - 1);
    state->last_scan_paths[0][sizeof(state->last_scan_paths[0]) - 1] = '\0';

    scan_thread_ctx* ctx = malloc(sizeof(scan_thread_ctx));
    if (!ctx)
        return;
    ctx->state = state;
    ctx->path_count = 1;
    ctx->paths = malloc(sizeof(char*));
    if (!ctx->paths) {
        free(ctx);
        return;
    }
    ctx->paths[0] = strdup(path);
    if (!ctx->paths[0]) {
        free(ctx->paths);
        free(ctx);
        return;
    }

    if (pthread_create(&state->scan_thread, NULL, scan_thread_func, ctx) != 0) {
        free(ctx->paths[0]);
        free(ctx->paths);
        free(ctx);
    }
}

void daemon_run_scan_multi(daemon_state* state, const char** paths, int path_count) {
    if (!state || !paths || path_count <= 0)
        return;

    if (atomic_load(&state->scanning))
        return;

    int count = path_count < CONFIG_MAX_ROOTS ? path_count : CONFIG_MAX_ROOTS;

    state->last_scan_path_count = count;
    for (int i = 0; i < count; i++) {
        strncpy(state->last_scan_paths[i], paths[i], sizeof(state->last_scan_paths[i]) - 1);
        state->last_scan_paths[i][sizeof(state->last_scan_paths[i]) - 1] = '\0';
    }

    scan_thread_ctx* ctx = malloc(sizeof(scan_thread_ctx));
    if (!ctx)
        return;
    ctx->state = state;
    ctx->path_count = count;
    ctx->paths = malloc(count * sizeof(char*));
    if (!ctx->paths) {
        free(ctx);
        return;
    }
    for (int i = 0; i < count; i++) {
        ctx->paths[i] = strdup(paths[i]);
        if (!ctx->paths[i]) {
            for (int j = 0; j < i; j++)
                free(ctx->paths[j]);
            free(ctx->paths);
            free(ctx);
            return;
        }
    }

    if (pthread_create(&state->scan_thread, NULL, scan_thread_func, ctx) != 0) {
        for (int i = 0; i < count; i++)
            free(ctx->paths[i]);
        free(ctx->paths);
        free(ctx);
    }
}

void daemon_start_rescan_timer(daemon_state* state) {
    if (!state || state->rescan_interval_seconds <= 0)
        return;
    atomic_store(&state->rescan_timer_running, true);
    pthread_create(&state->rescan_timer_thread, NULL, rescan_timer_func, state);
}

void daemon_stop_rescan_timer(daemon_state* state) {
    if (!state || !atomic_load(&state->rescan_timer_running))
        return;
    atomic_store(&state->rescan_timer_running, false);
    pthread_join(state->rescan_timer_thread, NULL);
}

scan_status daemon_scan_status(daemon_state* state) {
    scan_status s = {0};
    if (!state)
        return s;
    s.scanning = atomic_load(&state->scanning);
    s.buckets_so_far = atomic_load(&state->scan_bucket_count);
    if (s.scanning && state->store) {
        s.buckets_so_far = state->store->right_index;
    }
    return s;
}

path_validation daemon_process_query(daemon_state* state, const char* cwd, const char* input) {
    if (!state || !state->store) {
        path_validation empty = {0};
        return empty;
    }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    path_validation result = process_input(state->store, cwd, input);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    uint64_t latency_ns = (uint64_t) (ts_end.tv_sec - ts_start.tv_sec) * 1000000000ULL +
                          (uint64_t) (ts_end.tv_nsec - ts_start.tv_nsec);
    metrics_record_query(&state->metrics, latency_ns);

    return result;
}

completions* daemon_get_completions(daemon_state* state, const char* prefix, size_t limit) {
    if (!state || !state->store || !prefix) {
        return NULL;
    }

    metrics_record_completion(&state->metrics);

    completions* out = completions_create(limit > 0 ? limit : 50);
    if (!out)
        return NULL;

    /* Step 1: Snapshot bucket pointers under store_lock, increment refcounts */
    store_lock(state->store);
    size_t count = state->store->right_index;
    t_bucket** snapshot = (t_bucket**) malloc(count * sizeof(t_bucket*));
    if (!snapshot) {
        store_unlock(state->store);
        completions_free(out);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        t_bucket* bucket = state->store->buckets[i];
        if (bucket && bucket->dir_trie) {
            atomic_fetch_add(&bucket->refcount, 1);
            snapshot[i] = bucket;
        } else {
            snapshot[i] = NULL;
        }
    }
    store_unlock(state->store);

    /* Step 2: Iterate snapshot without store_lock */
    for (size_t i = 0; i < count && out->count < out->capacity; i++) {
        t_bucket* bucket = snapshot[i];
        if (!bucket)
            continue;

        trie_lock(bucket);
        completions_collect(bucket->dir_trie, prefix, out);
        trie_unlock(bucket);
        bucket_release(bucket);
    }

    free(snapshot);
    return out;
}

scored_result daemon_get_scored_completions(daemon_state* state, const char* prefix, size_t limit,
                                            uint64_t now, const char* cwd) {
    scored_result empty = {NULL, false};
    if (!state || !state->store || !prefix) {
        return empty;
    }

    const scored_completions* cached = cache_get(state->cache, prefix);
    if (cached) {
        metrics_record_cache_hit(&state->metrics);
        scored_result result;
        result.data = cached;
        result.from_cache = true;
        return result;
    }
    metrics_record_cache_miss(&state->metrics);

    metrics_record_completion(&state->metrics);

    scored_completions* out = scored_completions_create(limit > 0 ? limit : 50);
    if (!out)
        return empty;

    store_lock(state->store);
    size_t count = state->store->right_index;
    t_bucket** snapshot = (t_bucket**) malloc(count * sizeof(t_bucket*));
    if (!snapshot) {
        store_unlock(state->store);
        scored_completions_free(out);
        return empty;
    }
    for (size_t i = 0; i < count; i++) {
        t_bucket* bucket = state->store->buckets[i];
        if (bucket && bucket->dir_trie) {
            atomic_fetch_add(&bucket->refcount, 1);
            snapshot[i] = bucket;
        } else {
            snapshot[i] = NULL;
        }
    }
    store_unlock(state->store);

    for (size_t i = 0; i < count && out->count < out->capacity; i++) {
        t_bucket* bucket = snapshot[i];
        if (!bucket)
            continue;

        trie_lock(bucket);
        scored_completions_collect(bucket->dir_trie, prefix, out, now, cwd);
        trie_unlock(bucket);
        bucket_release(bucket);
    }

    free(snapshot);
    cache_put(state->cache, prefix, out);
    scored_result result;
    result.data = out;
    result.from_cache = false;
    return result;
}

void daemon_release_scored(daemon_state* state, scored_result result) {
    if (!state || !result.data)
        return;
    if (result.from_cache) {
        cache_release(state->cache, result.data);
    } else {
        scored_completions_free((scored_completions*) result.data);
    }
}

completions* daemon_get_fuzzy_completions(daemon_state* state, const char* query, size_t limit) {
    if (!state || !state->store || !query || query[0] == '\0') {
        return NULL;
    }

    completions* out = completions_create(limit > 0 ? limit : 50);
    if (!out)
        return NULL;

    const char* qbase = query;
    for (const char* p = query; *p; p++) {
        if (*p == '/')
            qbase = p + 1;
    }
    if (qbase[0] == '\0') {
        completions_free(out);
        return NULL;
    }

    store_lock(state->store);
    size_t bucket_count = state->store->right_index;
    t_bucket** snapshot = (t_bucket**) malloc(bucket_count * sizeof(t_bucket*));
    if (!snapshot) {
        store_unlock(state->store);
        completions_free(out);
        return NULL;
    }
    for (size_t i = 0; i < bucket_count; i++) {
        t_bucket* bucket = state->store->buckets[i];
        if (bucket && bucket->dir_trie) {
            atomic_fetch_add(&bucket->refcount, 1);
            snapshot[i] = bucket;
        } else {
            snapshot[i] = NULL;
        }
    }
    store_unlock(state->store);

    char** bucket_paths = calloc(limit > 0 ? limit : 50, sizeof(char*));
    if (!bucket_paths) {
        free(snapshot);
        completions_free(out);
        return NULL;
    }
    bool* bucket_is_dirs = calloc(limit > 0 ? limit : 50, sizeof(bool));
    if (!bucket_is_dirs) {
        free(bucket_paths);
        free(snapshot);
        completions_free(out);
        return NULL;
    }
    int bucket_cap = (int) (limit > 0 ? limit : 50);

    for (size_t i = 0; i < bucket_count && out->count < out->capacity; i++) {
        t_bucket* bucket = snapshot[i];
        if (!bucket)
            continue;

        trie_lock(bucket);
        int n =
            trie_fuzzy_collect(bucket->dir_trie, qbase, bucket_paths, bucket_is_dirs, bucket_cap);
        trie_unlock(bucket);

        for (int j = 0; j < n && out->count < out->capacity; j++) {
            if (bucket_paths[j]) {
                out->paths[out->count] = bucket_paths[j];
                out->is_dirs[out->count] = bucket_is_dirs[j];
                out->count++;
                bucket_paths[j] = NULL;
            }
        }

        bucket_release(bucket);
    }

    for (int j = 0; j < bucket_cap; j++) {
        free(bucket_paths[j]);
    }
    free(bucket_is_dirs);
    free(bucket_paths);
    free(snapshot);
    return out;
}

int daemon_get_recent_files(daemon_state* state, char** paths, bool* is_dirs, int n) {
    if (!state || !paths || !is_dirs || n <= 0)
        return 0;
    return recent_files_get(&state->recent, paths, is_dirs, n);
}

void daemon_touch_recent(daemon_state* state, const char* path, bool is_dir) {
    if (!state || !path)
        return;
    recent_files_touch(&state->recent, path, is_dir);
}

int daemon_start_ipc(daemon_state* state, const char* sock_path) {
    if (!state)
        return -1;
    state->ipc = ipc_server_start(state, sock_path);
    return state->ipc ? 0 : -1;
}
