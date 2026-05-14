#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "trie-storage.h"
#include "trie.h"

t_bucket* find_bucket(t_lfu* lfu, char* curr_dir) {
    int left = 0, right = BUCKETS - 1;

    normalise_dir(curr_dir);

    while(left < right) {
        int mid = right - left + 1;

        int cmp = strcmp(lfu->buckets[mid]->dir_name, curr_dir);
        
        if(cmp < 0) {
            left = mid + 1;
        }else if(cmp > 0) {
            right = mid;
        }else {
            return lfu->buckets[mid];
        }
    }

    return NULL;
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
