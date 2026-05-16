#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "trie-storage.h"
#include "trie.h"
#include "lru.h"

struct node;

t_bucket* find_bucket(t_bucket_store* lfu, char* original_dir, char* curr_dir, int depth, bool cut) {
    int left = 0, right = lfu->right_index;

    while(left < right) {
        int mid = left + (right - left) / 2;

        int cmp = strcmp(lfu->buckets[mid]->dir_name, curr_dir);
        
        if(cmp < 0) {
            left = mid + 1;
        }else if(cmp > 0) {
            right = mid;
        }else {
            //accessed so move to front
            move_to_front(lfu, lfu->buckets[mid]);
            return lfu->buckets[mid];
        }
    }

    /*
       If path has no ancestor paths, insert it
       e.g. ancestor path /home/sam/samdev
       e.g. path /home/sam/samdev/projects/archaic/src
    */
    if((!cut && get_dir_depth(curr_dir) <= MIN_DEPTH) || (cut && depth == 0)) {
        t_bucket* inserted = insert_bucket(lfu, original_dir); 
        return inserted;
    }
 
    /*
       Cutoff directory to try a parent directory Trie, to minimise memory usage
    */
    char* cutoff = cutoff_dir(curr_dir);

    if (cut) {
        free(curr_dir);
    }

    if (depth <= 0) {
        free(cutoff);
        return NULL;
    }

    return find_bucket(lfu, original_dir, cutoff, depth - 1, true);
}

t_bucket* insert_bucket(t_bucket_store* lfu, char* curr_dir) {
    
    size_t insertion = find_insertion_point(lfu, curr_dir);
    if(insertion == SIZE_MAX) {
        printf("Directory already exists in buckets - [insert_bucket()]");
        return NULL;
    }

    if(lfu->right_index + 1 < BUCKETS) {
        shift_right(lfu, insertion, lfu->right_index);
        lfu->buckets[insertion] = create_bucket(curr_dir);
        lfu->buckets[insertion]->id = insertion;
        lfu->buckets[insertion]->array_index = insertion;
        create_or_to_front(lfu, lfu->buckets[insertion]);

        lfu->right_index++;
        
        return lfu->buckets[insertion];

    }else {
        //NOTE: insertion var is the index in buckets array
        //    .       ^         
        //1,2,3,4,5,6,7,8,9,10
         

        /*
            Removes least recently used bucket and node from lru
            Inserts a new node(curr_dir) into buckets and to front of lru
        */

        t_bucket* removed = remove_last(lfu);
        if (!removed) {
            return NULL;
        }

        size_t removal_index = removed->array_index;
        if (removal_index < insertion) {
            insertion--;
        }

        shift_left(lfu, removal_index, lfu->right_index - 1);
        lfu->buckets[lfu->right_index - 1] = NULL;

        shift_right(lfu, insertion, lfu->right_index - 1);
        
        lfu->buckets[insertion] = create_bucket(curr_dir);
        lfu->buckets[insertion]->id = insertion;
        lfu->buckets[insertion]->array_index = insertion;

        create_or_to_front(lfu, lfu->buckets[insertion]);

    }

    return lfu->buckets[insertion];

}


void shift_left(t_bucket_store* lfu, size_t removal_index, size_t last_index) {
    for (size_t i = removal_index; i < last_index; i++) {
        lfu->buckets[i] = lfu->buckets[i + 1];
        if (lfu->buckets[i]) {
            lfu->buckets[i]->array_index = i;
        }
    }
}

void shift_right(t_bucket_store* lfu, size_t insertion_index, size_t last_index) {
    for (size_t i = last_index; i > insertion_index; i--) {
        lfu->buckets[i] = lfu->buckets[i - 1];
        if (lfu->buckets[i]) {
            lfu->buckets[i]->array_index = i;
        }
    }
}

size_t find_insertion_point(t_bucket_store* lfu, char* curr_dir) {

    int left = 0, right = lfu->right_index - 1;

    while(left <= right) {
        int mid = left + (right - left) / 2;

        int cmp = strcmp(lfu->buckets[mid]->dir_name, curr_dir);

        if(cmp < 0) {
            left = mid + 1;
        }else if(cmp > 0) {
            right = mid - 1;
        }else {
            return SIZE_MAX;
        }
    }
    return left;
}


/*
    UTILITY FUNCTIONS
*/
t_bucket* create_bucket(char* dir_name) {
    t_bucket* bucket = (t_bucket*) malloc(sizeof(t_bucket));

    bucket->dir_name  = strdup(dir_name);
    bucket->dir_count = 0;
    bucket->dir_trie  = create_trie();
    pthread_mutex_init(&bucket->lock, NULL);

    return bucket;
}

void destroy_bucket(t_bucket* bucket) {
    if (!bucket) {
        return;
    }
    pthread_mutex_destroy(&bucket->lock);
    free(bucket->dir_name);
    trie_free_recursive(bucket->dir_trie);
    free(bucket);
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

static char* join_path(const char* cwd, const char* input) {
    if (!cwd || !input) {
        return NULL;
    }

    if (input[0] == '/') {
        return normalise_dir(input);
    }

    size_t cwd_len = strlen(cwd);
    size_t input_len = strlen(input);
    bool add_slash = (cwd_len > 0 && cwd[cwd_len - 1] != '/');
    size_t len = cwd_len + (add_slash ? 1 : 0) + input_len;
    char* out = (char*) malloc(len + 1);
    if (!out) {
        return NULL;
    }

    memcpy(out, cwd, cwd_len);
    size_t pos = cwd_len;
    if (add_slash) {
        out[pos++] = '/';
    }
    memcpy(out + pos, input, input_len);
    out[len] = '\0';

    char* norm = normalise_dir(out);
    free(out);
    return norm;
}

path_validation validate_input_path(const char* cwd, const char* input) {
    path_validation result = {0};
    if (!cwd || !input) {
        return result;
    }

    char* full = join_path(cwd, input);
    if (!full) {
        return result;
    }

    struct stat st;
    if (stat(full, &st) == 0) {
        result.exists = true;
        result.is_dir = S_ISDIR(st.st_mode);
        result.is_file = S_ISREG(st.st_mode);
        result.full_path = full;
        return result;
    }

    free(full);
    return result;
}

void free_path_validation(path_validation* result) {
    if (!result) {
        return;
    }
    free(result->full_path);
    result->full_path = NULL;
    result->exists = false;
    result->is_dir = false;
    result->is_file = false;
}
