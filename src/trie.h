#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#include "threadmanager.h"

#define RADIX_INLINE_CHILDREN 4
#define RESULTS 10

typedef struct RadixChild {
    char edge_char;
    struct RadixNode* node;
} RadixChild;

typedef struct RadixNode {
    char* key;
    size_t key_len;
    RadixChild inline_storage[RADIX_INLINE_CHILDREN];
    RadixChild* children;
    uint8_t child_count;
    uint8_t child_capacity;
    uint64_t freq;
    bool is_leaf;
} RadixNode;

typedef RadixNode Trie;

Trie* create_trie(void);
void insert(Trie* root, const char* str);
Trie* search(Trie* root, state* scan, char* str);
void trie_free_recursive(Trie* node);

/*
   Completion collection
*/
typedef struct {
    char** paths;
    size_t count;
    size_t capacity;
} completions;

completions* completions_create(size_t capacity);
void completions_free(completions* c);
void completions_collect(Trie* root, const char* prefix, completions* out);
