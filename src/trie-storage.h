#pragma once

#include "trie.h"

#define BUCKETS 250


/*

*/

typedef struct {
    
    char* dir_name;
    Trie* dir_trie;
    uint32_t dir_count;//N of files/folders in trie

} t_bucket;

/*
    LFU to keep memory limited and fast
*/
typedef struct {
    t_bucket* buckets[BUCKETS];
    uint32_t freq[BUCKETS];//frequency of each bucket
    uint32_t min_freq;//bucket with mininum frequency;
} t_lfu;


t_bucket* find_bucket(t_lfu* lfu, char* curr_dir);
char* normalise_dir(const char* str);
void remove_char(char* str, int s_index);
void add_char(char* str, char c, int s_index);



