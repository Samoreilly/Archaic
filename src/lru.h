#pragma once

#include "trie-storage.h"
#include <stdbool.h>


typedef struct node {
    
    t_bucket* bucket;
    struct node* next;
    struct node* prev;

    struct node* first;
    struct node* tail;

    bool is_parent;

    size_t capacity;

} node;

node* create_node(t_lfu* lfu, t_bucket* bucket);

void to_front(t_lfu* luf, node* new_node);
void to_back(t_lfu* lfu, node* new_node);

void create_or_to_back(t_lfu* lfu, t_bucket* bucket);
void create_or_to_front(t_lfu* lfu, t_bucket* bucket);

t_bucket* get_last(node* nde);
t_bucket* get_first(node* nde);

void remove_last(t_lfu* lfu);
node* traverse_nodes(t_lfu* lfu, t_bucket* bucket);
