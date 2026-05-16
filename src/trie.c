#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "trie.h"
#include <unistd.h>
#include <pthread.h>

#include "threadmanager.h"

static RadixChild* find_child(RadixNode* node, char c) {
    for (uint8_t i = 0; i < node->child_count; i++) {
        if (node->children[i].edge_char == c) {
            return &node->children[i];
        }
    }
    return NULL;
}

static void add_child(RadixNode* node, char c, RadixNode* child) {
    if (node->child_count >= node->child_capacity) {
        size_t new_cap = node->child_capacity * 2;
        RadixChild* new_children = malloc(new_cap * sizeof(RadixChild));
        if (!new_children) return;
        memcpy(new_children, node->children, node->child_count * sizeof(RadixChild));
        node->children = new_children;
        node->child_capacity = (uint8_t)new_cap;
    }
    node->children[node->child_count].edge_char = c;
    node->children[node->child_count].node = child;
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
                add_child(split, str[i + match], new_node);
            }
            return;
        }
    }
}

Trie* search(Trie* root, state* scan, char* str) {
    (void)scan;
    if (!root || !str || str[0] == '\0') return NULL;

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

    if (node != root && node->key && node->key_len > 0 && matched_in_node > 0) {
        size_t remaining_key = node->key_len - matched_in_node;
        if (remaining_key > 0 && remaining_key < 2048) {
            memcpy(buffer, node->key + matched_in_node, remaining_key);
            depth = remaining_key;

            if (node->is_leaf) {
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
