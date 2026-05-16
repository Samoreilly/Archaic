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


/*
    Auxilliary methods
*/

typedef struct {
    char* str;
    Trie* curr_node;
    state* scan;
} t_args;

void spin_threads(t_args* args, state* scan);
void* add_leftover(void* args);




