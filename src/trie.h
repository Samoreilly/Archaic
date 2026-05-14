#pragma once

#include <inttypes.h>
#include <stdbool.h>

#include "threadmanager.h"

#define CHILDREN 26;
#define RESULTS 10

typedef struct Trie {
    struct Trie* children[26];
    uint64_t freq;
    bool is_leaf;
} Trie;


Trie* create_trie();
void insert(Trie* root, const char* str);
Trie* search(Trie* root, scanner* scan, char* str);


/*
    Auxilliary methods
*/

typedef struct {
    char* str;
    Trie* curr_node;
    scanner* scan;
} t_args;

void spin_threads(t_args* args, scanner* scan);
void* add_leftover(void* args);





