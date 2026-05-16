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

node* create_node(t_bucket_store* lfu, t_bucket* bucket);

void to_front(t_bucket_store* luf, node* new_node);
void to_back(t_bucket_store* lfu, node* new_node);

t_bucket* create_or_to_back(t_bucket_store* lfu, t_bucket* bucket);
t_bucket* create_or_to_front(t_bucket_store* lfu, t_bucket* bucket);
void move_to_front(t_bucket_store* lfu, t_bucket* bucket);

t_bucket* get_last(node* nde);
t_bucket* get_first(node* nde);

t_bucket* remove_last(t_bucket_store* lfu);
node* traverse_nodes(t_bucket_store* lfu, t_bucket* bucket);
