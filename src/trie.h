#pragma once

#include <inttypes.h>
#include <stdbool.h>

#define CHILDREN 26;
#define RESULTS 10

typedef struct Trie {
    struct Trie* children[26];
    uint64_t freq;
    bool is_leaf;
} Trie;


void insert(Trie* root, const char* str);
Trie* search(Trie* root, char* str);


/*
    Auxilliary methods
*/

typedef struct {
    char* str;
    Trie* curr_node;
} t_args;

void spin_threads(t_args* args);
void* add_leftover(void* args);





