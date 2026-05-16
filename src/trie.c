#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "trie.h"
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "threadmanager.h"

static char index_to_char(int idx) {
    if (idx >= 0 && idx < 26) return (char)('a' + idx);
    if (idx >= 26 && idx < 36) return (char)('0' + (idx - 26));
    if (idx == 36) return '/';
    if (idx == 37) return '.';
    if (idx == 38) return '_';
    if (idx == 39) return '-';
    return '?';
}

void trie_free_recursive(Trie* node) {
    if (!node) return;
    for (size_t i = 0; i < TRIE_CHILDREN; i++) {
        if (node->children[i]) {
            trie_free_recursive(node->children[i]);
        }
    }
    free(node);
}

Trie* create_trie() {
    Trie* node = (Trie*) malloc(sizeof(Trie));

    for(size_t i = 0;i < TRIE_CHILDREN;i++) {
        node->children[i] = 0;
    }
    node->freq = 1;
    node->is_leaf = false;
    return node;
}

static int char_index(char c) {
    if (c >= 'a' && c <= 'z') {
        return c - 'a';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= '0' && c <= '9') {
        return 26 + (c - '0');
    }
    if (c == '/') {
        return 36;
    }
    if (c == '.') {
        return 37;
    }
    if (c == '_') {
        return 38;
    }
    if (c == '-') {
        return 39;
    }
    return -1;
}

void insert(Trie* root, const char* str) {
    Trie* curr = root;
    bool advanced = false;

    for (size_t i = 0; str[i] != '\0'; i++) {
        int idx = char_index(str[i]);
        if (idx < 0 || idx >= TRIE_CHILDREN) {
            continue;
        }
        advanced = true;
        if (!curr->children[idx]) {
            curr->children[idx] = create_trie();
            curr->children[idx]->is_leaf = false;
        }
        curr = curr->children[idx];
    }

    if (advanced) {
        curr->is_leaf = true;
    }
}

Trie* search(Trie* root, state* scan, char* str) {
    (void)scan;

    if (!root || !str) {
        return NULL;
    }

    Trie* trie = root;
    bool found_all = true;

    for (size_t i = 0; str[i] != '\0'; i++) {
        int idx = char_index(str[i]);
        if (idx < 0 || idx >= TRIE_CHILDREN) {
            continue;
        }

        if (!trie->children[idx]) {
            found_all = false;
            break;
        }

        trie->children[idx]->freq++;
        trie = trie->children[idx];
    }

    if (found_all) {
        return trie;
    }

    return NULL;
}

/*
   Completion collection
*/

completions* completions_create(size_t capacity) {
    completions* c = (completions*) calloc(1, sizeof(completions));
    c->paths = (char**) calloc(capacity, sizeof(char*));
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

static void collect_dfs(Trie* node, char* buffer, size_t depth, const char* prefix, completions* out) {
    if (!node || out->count >= out->capacity) {
        return;
    }

    if (node->is_leaf && depth > 0) {
        buffer[depth] = '\0';
        size_t plen = strlen(prefix);
        size_t slen = strlen(buffer);
        char* full = (char*) malloc(plen + slen + 1);
        if (full) {
            memcpy(full, prefix, plen);
            memcpy(full + plen, buffer, slen + 1);
            out->paths[out->count++] = full;
        }
    }

    for (int i = 0; i < TRIE_CHILDREN; i++) {
        if (!node->children[i]) continue;
        buffer[depth] = index_to_char(i);
        collect_dfs(node->children[i], buffer, depth + 1, prefix, out);
        if (out->count >= out->capacity) return;
    }
}

static Trie* find_prefix_node(Trie* root, const char* prefix) {
    if (!root || !prefix) return NULL;
    Trie* curr = root;
    for (size_t i = 0; prefix[i] != '\0'; i++) {
        int idx = char_index(prefix[i]);
        if (idx < 0 || idx >= TRIE_CHILDREN) continue;
        if (!curr->children[idx]) return NULL;
        curr = curr->children[idx];
    }
    return curr;
}

void completions_collect(Trie* root, const char* prefix, completions* out) {
    if (!root || !out) return;

    Trie* node = find_prefix_node(root, prefix);
    if (!node) return;

    char buffer[2048];
    collect_dfs(node, buffer, 0, prefix, out);
}
