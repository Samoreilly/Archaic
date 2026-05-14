#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "trie.h"
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "threadmanager.h"

Trie* create_trie() {
    Trie* node = (Trie*) malloc(sizeof(Trie));

    for(size_t i = 0;i < 26;i++) {
        node->children[i] = 0;
    }
    node->freq = 1;
    node->is_leaf = true;
    return node;
}

Trie* search(Trie* root, state* scan, char* str) {
    Trie* trie = root;
    bool is_new = false;
    
    char* leftover = malloc(strlen(str) + 1);

    for (size_t i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        int idx = c - 'a';

        //no path for current char
        if (!trie->children[idx]) {
            
            //FIX: also need to add chars to trie (spin up worker thread?)
            
            leftover = (char*) realloc(leftover, strlen(str) - i + 1);
            
            //remaining unfound chars in trie
            strcpy(leftover, str + i); 
            is_new = true;
            
            printf("\nBREAK in search: %s\n", leftover);
            break;        

            //FIX: return options, based on weight or some other metric
           
        }else {
            trie->children[idx]->freq++;
        }
        trie = trie->children[idx];
    }

    if(is_new) {
        printf("\nIS_NEW in search: %s\n", leftover);

        //spin-up a thread to extend trie
        t_args* args = (t_args*) malloc(sizeof(t_args));
        args->str = leftover;
        args->curr_node = trie;
        args->scan = scan;
        
        spin_threads(args, scan);    
    }else {

        //TODO: pass to results handling
        free(leftover);
    }

    //word complete, no auto-complete for now
    return trie;
}

/*
   Handles background processes such as adding nodes in trie
   NOTE: May extend with function pointers and union in t_args to re-use this method
*/
void spin_threads(t_args* args, state* scan) {
    if (scan->running) {
        scan->stop = true;
        //wait for thread to finish
        pthread_join(scan->worker, NULL);
        scan->running = false;
    }
    scan->stop = false;
    scan->running = true;
    pthread_create(&scan->worker, NULL, add_leftover, (void*)args);
}

void* add_leftover(void* args) {

    printf("\nEntered leftover\n");

    Trie* curr = ((t_args*) args)->curr_node;
    char* str = ((t_args*) args)->str;

    for(size_t i = 0;str[i] != '\0';i++) {

        //another thread entered, return early
        if(((t_args*) args)->scan->stop) {
            
            printf("\nCanceled pthread_t in add_leftover %s\n", str);
            free(str);
            free(args);

            return NULL;
        }   

        char c = str[i];
        int idx = c - 'a';
        
        printf("\nChar in leftover: %c\n", c);

        curr->children[idx] = create_trie();
        curr->is_leaf = false;
        curr = curr->children[idx];
    }

    curr->is_leaf = true;

    free(args);
    free(str);

    printf("End of leftover");
  
    return NULL;
}



