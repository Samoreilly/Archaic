#pragma once

#include <inttypes.h>
#include <stdbool.h>

#include "threadmanager.h"

#define TRIE_CHILDREN 40
#define RESULTS 10

typedef struct Trie {
    struct Trie* children[TRIE_CHILDREN];
    uint64_t freq;
    bool is_leaf;
} Trie;


Trie* create_trie();
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




