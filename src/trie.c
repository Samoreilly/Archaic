#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "trie.h"


Trie* create_trie() {
    Trie* node = (Trie*) malloc(sizeof(Trie));
    node->freq = 1;
    node->is_leaf = true;
    return node;
}

void insert(Trie* root, const char* str) {

    //gets the correct node to insert
    Trie* child = search(root, str);

}

Trie* search(Trie* root, const char* str) {
    Trie* trie = root;
    bool is_new = false;

    for (size_t i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        int idx = c - 'a';

        if (!trie->children[idx]) {
            trie->children[idx] = create_trie();
            trie->is_leaf = false;
            is_new = true;

            printf("Created new: %c\n", c);
        } else {
            trie->children[idx]->freq++;
        }

        trie = trie->children[idx];
    }

    if (is_new)
        return NULL;

    return trie;
}
