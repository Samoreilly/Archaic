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

    for(size_t i = 0;i < TRIE_CHILDREN;i++) {
        node->children[i] = 0;
    }
    node->freq = 1;
    node->is_leaf = true;
    return node;
}

static int char_index(char c) {
    if (c >= 'a' && c <= 'z') {
        return c - 'a';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= '0' && c <= '9') {
        return 26 + (c - '0');
    }
    if (c == '/') {
        return 36;
    }
    if (c == '.') {
        return 37;
    }
    if (c == '_') {
        return 38;
    }
    if (c == '-') {
        return 39;
    }
    return -1;
}

void insert(Trie* root, const char* str) {
    Trie* curr = root;
    bool advanced = false;

    for (size_t i = 0; str[i] != '\0'; i++) {
        int idx = char_index(str[i]);
        if (idx < 0 || idx >= TRIE_CHILDREN) {
            continue;
        }
        advanced = true;
        if (!curr->children[idx]) {
            curr->children[idx] = create_trie();
            curr->children[idx]->is_leaf = false;
        }
        curr = curr->children[idx];
    }

    if (advanced) {
        curr->is_leaf = true;
    }
}

Trie* search(Trie* root, state* scan, char* str) {
    Trie* trie = root;
    bool is_new = false;
    
    char* leftover = malloc(strlen(str) + 1);

    for (size_t i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        int idx = char_index(c);

        if (idx < 0 || idx >= TRIE_CHILDREN) {
            continue;
        }

        //no path for current char
        if (!trie->children[idx]) {
            
            //FIX: also need to add chars to trie (spin up worker thread?)
            
            leftover = (char*) realloc(leftover, strlen(str) - i + 1);
            
            //remaining unfound chars in trie
            strcpy(leftover, str + i); 
            is_new = true;
            
            
            break;        

            //FIX: return options, based on weight or some other metric
           
        }else {
            trie->children[idx]->freq++;
        }
        trie = trie->children[idx];
    }

    if(is_new) {
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

    Trie* curr = ((t_args*) args)->curr_node;
    char* str = ((t_args*) args)->str;

    for(size_t i = 0;str[i] != '\0';i++) {

        //another thread entered, return early
        if(((t_args*) args)->scan->stop) {
            
            free(str);
            free(args);

            return NULL;
        }   

        char c = str[i];
        int idx = char_index(c);

        if (idx < 0 || idx >= TRIE_CHILDREN) {
            continue;
        }
        
        if (!curr->children[idx]) {
            curr->children[idx] = create_trie();
        }
        curr = curr->children[idx];
    }

    curr->is_leaf = true;

    free(args);
    free(str);

    return NULL;
}
