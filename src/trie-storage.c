#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "trie-storage.h"
#include "trie.h"

t_bucket* find_bucket(t_lfu* lfu, char* curr_dir, int depth, bool cut) {
    int left = 0, right = BUCKETS - 1;

    while(left < right) {
        int mid = left + (right - left) / 2;

        int cmp = strcmp(lfu->buckets[mid]->dir_name, curr_dir);
        
        if(cmp < 0) {
            left = mid + 1;
        }else if(cmp > 0) {
            right = mid;
        }else {
            return lfu->buckets[mid];
        }
    }

    if(!cut && get_dir_depth(curr_dir) <= MIN_DEPTH) {
        return NULL;
    }
 
    /*
        Cutoff directory to try a parent directory Trie, to minimise memory usage
    */
    char* cutoff = cutoff_dir(curr_dir);
    
    if(cut) {
        free(curr_dir);
    }

    return find_bucket(lfu, cutoff, depth - 1, true);
}

char* cutoff_dir(char* str) {
    
    int i = strlen(str) - 1;    
    for(;i >= 0 && str[i] != '/';i--);

    size_t new_len = (i < 0) ? 0 : i + 1; 
    char* new_dir = (char*) malloc(new_len + 1);

    strncpy(new_dir, str, new_len);
    new_dir[new_len] = '\0';

    return new_dir;
}

int get_dir_depth(char* str) {
    int depth = 0;

    for(int i = 0;str[i] != '\0';i++) {
        if(str[i] == '/') {
            depth++;
        }
    }
    
    return depth;
}

char* normalise_dir(const char* original_str) {
    size_t len = strlen(original_str);
    if (len == 0) {
       
        char* empty = (char*) malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    bool needs_leading_slash = (original_str[0] != '/');
    bool has_trailing_slash = (len > 1 && original_str[len - 1] == '/');

   
    //final string length
    size_t new_len = len + (needs_leading_slash ? 1 : 0) - (has_trailing_slash ? 1 : 0);

    char* new_str = (char*) malloc(new_len + 1);
    if (new_str == NULL) {
        return NULL;
    }

    char* dst = new_str;
    
    if (needs_leading_slash) {
        *dst++ = '/';
    }
    
    //amount of bytes to copy from src

    size_t copy_len = len - (has_trailing_slash ? 1 : 0);
    memcpy(dst, original_str, copy_len);
    
    dst += copy_len;
    *dst = '\0';

    return new_str;
}
