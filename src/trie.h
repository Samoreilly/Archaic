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
    uint64_t last_access;
    bool is_leaf;
    bool is_dir;
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

typedef struct {
    char path[4096];
    double score;
    uint64_t freq;
    uint64_t last_access;
    bool is_dir;
} scored_entry;

typedef struct {
    scored_entry* entries;
    size_t count;
    size_t capacity;
} scored_completions;

/*
   Scoring weights (sum to 1.0)
*/
#define SCORE_WEIGHT_FREQ 0.40
#define SCORE_WEIGHT_RECENCY 0.30
#define SCORE_WEIGHT_DEPTH 0.20
#define SCORE_WEIGHT_TYPE 0.10

completions* completions_create(size_t capacity);
void completions_free(completions* c);
void completions_collect(Trie* root, const char* prefix, completions* out);

scored_completions* scored_completions_create(size_t capacity);
void scored_completions_free(scored_completions* sc);
void scored_completions_collect(Trie* root, const char* prefix, scored_completions* out,
                                uint64_t now);

int trie_fuzzy_collect(Trie* root, const char* query, char** paths, int capacity);
