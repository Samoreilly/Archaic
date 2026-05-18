#include "mmap-state.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "io/fileloader.h"
#include "log.h"
#include "trie-storage.h"
#include "trie.h"

#define STATE_MAGIC 0x41525354U
#define STATE_VERSION 2U

typedef struct {
    uint8_t* base;
    size_t size;
    size_t pos;
} mmap_cursor;

static int cursor_write(mmap_cursor* c, const void* data, size_t len) {
    if (c->pos + len > c->size)
        return -1;
    memcpy(c->base + c->pos, data, len);
    c->pos += len;
    return 0;
}

static int cursor_read(mmap_cursor* c, void* out, size_t len) {
    if (c->pos + len > c->size)
        return -1;
    memcpy(out, c->base + c->pos, len);
    c->pos += len;
    return 0;
}

typedef struct {
    const RadixNode* node;
    uint32_t index;
} IndexedNode;

static size_t count_trie_nodes(const RadixNode* node) {
    if (!node)
        return 0;
    size_t count = 1;
    for (uint8_t i = 0; i < node->child_count; i++)
        count += count_trie_nodes(node->children[i].node);
    return count;
}

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

static size_t bucket_serialized_size(const t_bucket* bucket) {
    if (!bucket || !bucket->dir_trie)
        return 0;

    size_t sz = sizeof(uint32_t);
    sz += strlen(bucket->dir_name);
    sz += sizeof(uint32_t);

    uint32_t nc = (uint32_t) count_trie_nodes(bucket->dir_trie);
    sz += sizeof(uint32_t);

    IndexedNode* indexed = malloc(nc * sizeof(IndexedNode));
    if (!indexed)
        return 0;
    uint32_t idx = 0;
    assign_indices_dfs(bucket->dir_trie, indexed, &idx);

    for (uint32_t i = 0; i < nc; i++) {
        const RadixNode* nd = indexed[i].node;
        sz += sizeof(uint32_t);
        sz += nd->key_len;
        sz += sizeof(uint8_t);
        sz += sizeof(uint64_t);
        sz += sizeof(uint64_t);
        sz += sizeof(uint8_t);
        sz += sizeof(uint8_t);
        sz += nd->child_count * (sizeof(uint8_t) + sizeof(uint32_t));
    }
    free(indexed);
    return sz;
}

static int serialize_bucket(mmap_cursor* c, const t_bucket* bucket) {
    uint32_t dn_len = (uint32_t) strlen(bucket->dir_name);
    if (cursor_write(c, &dn_len, sizeof(dn_len)) != 0)
        return -1;
    if (cursor_write(c, bucket->dir_name, dn_len) != 0)
        return -1;
    if (cursor_write(c, &bucket->dir_count, sizeof(bucket->dir_count)) != 0)
        return -1;

    uint32_t nc = (uint32_t) count_trie_nodes(bucket->dir_trie);
    if (cursor_write(c, &nc, sizeof(nc)) != 0)
        return -1;

    if (nc == 0)
        return 0;

    IndexedNode* indexed = malloc(nc * sizeof(IndexedNode));
    if (!indexed)
        return -1;
    uint32_t idx = 0;
    assign_indices_dfs(bucket->dir_trie, indexed, &idx);

    for (uint32_t i = 0; i < nc; i++) {
        const RadixNode* nd = indexed[i].node;

        uint32_t kl = (uint32_t) nd->key_len;
        if (cursor_write(c, &kl, sizeof(kl)) != 0) {
            free(indexed);
            return -1;
        }
        if (kl > 0 && cursor_write(c, nd->key, kl) != 0) {
            free(indexed);
            return -1;
        }

        uint8_t cc = nd->child_count;
        if (cursor_write(c, &cc, sizeof(cc)) != 0) {
            free(indexed);
            return -1;
        }
        if (cursor_write(c, &nd->freq, sizeof(nd->freq)) != 0) {
            free(indexed);
            return -1;
        }
        if (cursor_write(c, &nd->last_access, sizeof(nd->last_access)) != 0) {
            free(indexed);
            return -1;
        }

        uint8_t il = (uint8_t) nd->is_leaf, id = (uint8_t) nd->is_dir;
        if (cursor_write(c, &il, sizeof(il)) != 0) {
            free(indexed);
            return -1;
        }
        if (cursor_write(c, &id, sizeof(id)) != 0) {
            free(indexed);
            return -1;
        }

        for (uint8_t cc2 = 0; cc2 < nd->child_count; cc2++) {
            uint8_t ec = (uint8_t) nd->children[cc2].edge_char;
            if (cursor_write(c, &ec, sizeof(ec)) != 0) {
                free(indexed);
                return -1;
            }
            int ci = find_child_index(indexed, nc, nd->children[cc2].node);
            uint32_t ci32 = (uint32_t) ci;
            if (cursor_write(c, &ci32, sizeof(ci32)) != 0) {
                free(indexed);
                return -1;
            }
        }
    }
    free(indexed);
    return 0;
}

typedef struct {
    uint8_t edge_char;
    uint32_t child_index;
} ChildRef;

typedef struct {
    ChildRef* refs;
    uint8_t count;
} ChildRefs;

static int deserialize_bucket(mmap_cursor* c, daemon_state* state) {
    uint32_t dn_len;
    if (cursor_read(c, &dn_len, sizeof(dn_len)) != 0)
        return -1;

    char* dir_name = malloc(dn_len + 1);
    if (!dir_name)
        return -1;
    if (cursor_read(c, dir_name, dn_len) != 0) {
        free(dir_name);
        return -1;
    }
    dir_name[dn_len] = '\0';

    uint32_t dir_count, node_count;
    if (cursor_read(c, &dir_count, sizeof(dir_count)) != 0) {
        free(dir_name);
        return -1;
    }
    if (cursor_read(c, &node_count, sizeof(node_count)) != 0) {
        free(dir_name);
        return -1;
    }

    store_lock(state->store);
    t_bucket* bucket = insert_bucket(state->store, dir_name);
    store_unlock(state->store);
    free(dir_name);
    if (!bucket)
        return -1;
    bucket->dir_count = dir_count;

    if (node_count == 0)
        return 0;

    RadixNode** nodes = calloc(node_count, sizeof(RadixNode*));
    ChildRefs* crefs = calloc(node_count, sizeof(ChildRefs));
    if (!nodes || !crefs) {
        free(nodes);
        free(crefs);
        return -1;
    }

    for (uint32_t i = 0; i < node_count; i++) {
        uint32_t kl;
        if (cursor_read(c, &kl, sizeof(kl)) != 0)
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
            if (cursor_read(c, nd->key, kl) != 0) {
                free(nd->key);
                free(nd);
                goto bucket_err;
            }
            nd->key[kl] = '\0';
        }
        nd->key_len = kl;

        uint8_t cc;
        if (cursor_read(c, &cc, sizeof(cc)) != 0)
            goto bucket_err;
        nd->child_count = cc;

        if (cursor_read(c, &nd->freq, sizeof(nd->freq)) != 0)
            goto bucket_err;
        if (cursor_read(c, &nd->last_access, sizeof(nd->last_access)) != 0)
            goto bucket_err;

        uint8_t il, id;
        if (cursor_read(c, &il, sizeof(il)) != 0)
            goto bucket_err;
        if (cursor_read(c, &id, sizeof(id)) != 0)
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

            for (uint8_t cc2 = 0; cc2 < cc; cc2++) {
                uint8_t ec;
                uint32_t ci;
                if (cursor_read(c, &ec, sizeof(ec)) != 0)
                    goto bucket_err;
                if (cursor_read(c, &ci, sizeof(ci)) != 0)
                    goto bucket_err;
                nd->children[cc2].edge_char = (char) ec;
                nd->children[cc2].node = NULL;
                crefs[i].refs[cc2].edge_char = ec;
                crefs[i].refs[cc2].child_index = ci;
            }
        } else {
            nd->children = nd->inline_storage;
            nd->child_capacity = RADIX_INLINE_CHILDREN;
        }

        nodes[i] = nd;
    }

    for (uint32_t i = 0; i < node_count; i++) {
        for (uint8_t cc2 = 0; cc2 < crefs[i].count; cc2++) {
            uint32_t ci = crefs[i].refs[cc2].child_index;
            if (ci < node_count)
                nodes[i]->children[cc2].node = nodes[ci];
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
    return 0;

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
    return -1;
}

int mmap_state_save(daemon_state* state, const char* path) {
    if (!state || !state->store || !path)
        return -1;

    store_lock(state->store);
    uint32_t bucket_count = (uint32_t) state->store->right_index;

    size_t total = 4 * sizeof(uint32_t);
    for (uint32_t b = 0; b < bucket_count; b++) {
        t_bucket* bucket = state->store->buckets[b];
        if (!bucket || !bucket->dir_trie)
            continue;
        total += bucket_serialized_size(bucket);
    }
    store_unlock(state->store);

    if (total < 4 * sizeof(uint32_t))
        return -1;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t) total) != 0) {
        close(fd);
        return -1;
    }

    void* mapped = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return -1;
    }

    mmap_cursor c = {.base = mapped, .size = total, .pos = 0};

    uint32_t magic = STATE_MAGIC, version = STATE_VERSION, reserved = 0;
    if (cursor_write(&c, &magic, sizeof(magic)) != 0)
        goto err;
    if (cursor_write(&c, &version, sizeof(version)) != 0)
        goto err;
    if (cursor_write(&c, &bucket_count, sizeof(bucket_count)) != 0)
        goto err;
    if (cursor_write(&c, &reserved, sizeof(reserved)) != 0)
        goto err;

    store_lock(state->store);
    for (uint32_t b = 0; b < bucket_count; b++) {
        t_bucket* bucket = state->store->buckets[b];
        if (!bucket || !bucket->dir_trie)
            continue;

        trie_lock(bucket);
        if (serialize_bucket(&c, bucket) != 0) {
            trie_unlock(bucket);
            store_unlock(state->store);
            goto err;
        }
        trie_unlock(bucket);
    }
    store_unlock(state->store);

    if (msync(mapped, total, MS_SYNC) != 0)
        goto err;
    munmap(mapped, total);
    close(fd);
    return 0;

err:
    munmap(mapped, total);
    close(fd);
    return -1;
}

int mmap_state_load(daemon_state* state, const char* path) {
    if (!state || !state->store || !path)
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    if (st.st_size < (off_t) (4 * sizeof(uint32_t))) {
        close(fd);
        return -1;
    }

    void* mapped = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return -1;
    }

    mmap_cursor c = {.base = mapped, .size = (size_t) st.st_size, .pos = 0};

    uint32_t magic, version, bucket_count, reserved;
    if (cursor_read(&c, &magic, sizeof(magic)) != 0)
        goto err;
    if (cursor_read(&c, &version, sizeof(version)) != 0)
        goto err;
    if (cursor_read(&c, &bucket_count, sizeof(bucket_count)) != 0)
        goto err;
    if (cursor_read(&c, &reserved, sizeof(reserved)) != 0)
        goto err;

    if (magic != STATE_MAGIC || version != STATE_VERSION) {
        if (magic == STATE_MAGIC && version != STATE_VERSION) {
            LOG_WARN("mmap", "state file version %u != expected %u, discarding", version,
                     STATE_VERSION);
        }
        goto err;
    }

    for (uint32_t b = 0; b < bucket_count; b++) {
        if (deserialize_bucket(&c, state) != 0)
            goto err;
    }

    munmap(mapped, (size_t) st.st_size);
    close(fd);
    return 0;

err:
    munmap(mapped, (size_t) st.st_size);
    close(fd);
    return -1;
}
