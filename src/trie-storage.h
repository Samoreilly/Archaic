#pragma once

#include "trie.h"

#define BUCKETS 250
#define MIN_DEPTH 4//when searching for a ancestor directory, only search down to 4 level e.g. /home/sam/project/archaic

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
    //sorted lexographically
    t_bucket* buckets[BUCKETS];
    t_bucket* lru;//bucket with mininum frequency;
    size_t right_index;//dynamically keeps tracks of right border

} t_lfu;


t_bucket* find_bucket(t_lfu* lfu, char* curr_dir, int depth, bool cutoff);

char* normalise_dir(const char* str);
char* cutoff_dir(char* str);
int get_dir_depth(char* str);

void remove_char(char* str, int s_index);
void add_char(char* str, char c, int s_index);



