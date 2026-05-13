#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "trie.h"
#include <unistd.h>

Trie* create_trie() {
    Trie* node = (Trie*) malloc(sizeof(Trie));
    node->freq = 1;
    node->is_leaf = true;
    return node;
}

Trie* search(Trie* root, const char* str) {
    Trie* trie = root;
    bool is_new = false;

    for (size_t i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        int idx = c - 'a';

        //no path for current char
        if (!trie->children[idx]) {
            //FIX: also need to return current options (spin up worker thread?)

            trie->children[idx] = create_trie();
            
            //set parent is_leaf to false
            trie->is_leaf = false;
            is_new = true;

            printf("Created new: %c\n", c);
        
        }else {
            trie->children[idx]->freq++;
        }

        trie = trie->children[idx];
    }

    if (is_new)
        return NULL;

    return trie;
}
