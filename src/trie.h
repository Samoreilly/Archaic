#pragma once

#include <inttypes.h>
#include <stdbool.h>

#define CHILDREN 26;

typedef struct Trie {
    struct Trie* children[26];
    uint64_t freq;
    bool is_leaf;
} Trie;

void insert(Trie* root, const char* str);
Trie* search(Trie* root, const char* str);



