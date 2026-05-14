#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "trie.h"
#include <string.h>
#include <unistd.h>
#include <pthread.h>

Trie* create_trie() {
    Trie* node = (Trie*) malloc(sizeof(Trie));
    node->freq = 1;
    node->is_leaf = true;
    return node;
}

Trie* search(Trie* root, char* str) {
    Trie* trie = root;
    bool is_new = false;

    for (size_t i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        int idx = c - 'a';

        //no path for current char
        if (!trie->children[idx]) {
            
            //FIX: also need to add chars to trie (spin up worker thread?)

            //remaining unfound chars in trie
            char* leftover = malloc(strlen(str) - i + 1); 
            strcpy(leftover, str + i);
            printf("\nLeftover: %s\n", leftover);
            
            //spin-up a thread to extend trie
            t_args* args = (t_args*) malloc(sizeof(t_args));
            args->str = leftover;
            args->curr_node = trie;
            spin_threads(args);
            
            //FIX: return options, based on weight or some other metric
            return NULL;
            
       }else {
            trie->children[idx]->freq++;
        }
       
        trie = trie->children[idx];
    }

    //word complete, no auto-complete for now

    return trie;
}

/*
   Handles background processes such as adding nodes in trie
   NOTE: May extend with function pointers and union in t_args to re-use this method
*/
void spin_threads(t_args* args) {
    pthread_t thread_id;
    pthread_create(&thread_id, NULL, add_leftover, (void*) args);
    pthread_detach(thread_id);
}

void* add_leftover(void* args) {
    
    Trie* curr = ((t_args*) args)->curr_node;
    char* str = ((t_args*) args)->str;
    
    for(size_t i = 0;str[i] != '\0';i++) {
        char c = str[i];
        int idx = c - 'a';

        curr->children[idx] = create_trie();
        curr->is_leaf = false;
        curr = curr->children[idx];
    }

    curr->is_leaf = true;

    printf("\nBACKGROUND TASK\n");

}
