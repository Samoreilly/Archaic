#pragma once

#include "trie.h"

#define BUCKETS 250
#define MIN_DEPTH 4

typedef struct t_bucket {
    
    char* dir_name;
    Trie* dir_trie;
    uint32_t dir_count;

    pthread_mutex_t lock;

    size_t id;
    size_t array_index;

} t_bucket;


typedef struct t_bucket_store {
    t_bucket* buckets[BUCKETS];
    t_bucket* lru;
    size_t right_index;

    struct node* parent;
    struct node* by_id[BUCKETS];
    size_t lru_size;

} t_bucket_store;

struct node;

typedef struct {
    bool exists;
    bool is_dir;
    bool is_file;
    char* full_path;
} path_validation;

t_bucket* find_bucket(t_bucket_store* lfu, char* original_dir, char* curr_dir, int depth, bool cutoff);
t_bucket* insert_bucket(t_bucket_store* lfu, char* curr_dir);
t_bucket* create_bucket(char* dir_name);
void destroy_bucket(t_bucket* bucket);
size_t find_insertion_point(t_bucket_store* lfu, char* curr_dir);
void shift_left(t_bucket_store* lfu, size_t removal_index, size_t last_index);
void shift_right(t_bucket_store* lfu, size_t insertion_index, size_t last_index);

void shift_left(t_bucket_store* lfu, size_t insertion_index, size_t removal_index);
void shift_right(t_bucket_store* lfu, size_t insertion_index, size_t removal_index);

char* normalise_dir(const char* str);
char* cutoff_dir(char* str);
int get_dir_depth(char* str);

path_validation validate_input_path(const char* cwd, const char* input);
void free_path_validation(path_validation* result);

static inline void trie_lock(t_bucket* bucket) {
    if (bucket) {
        pthread_mutex_lock(&bucket->lock);
    }
}

static inline void trie_unlock(t_bucket* bucket) {
    if (bucket) {
        pthread_mutex_unlock(&bucket->lock);
    }
}

void remove_char(char* str, int s_index);
void add_char(char* str, char c, int s_index);
