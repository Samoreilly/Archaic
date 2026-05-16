#include "lru.h"
#include "trie-storage.h"
#include <stdlib.h>

static void detach_node(t_lfu* lfu, node* nde) {
    node* parent = lfu ? lfu->parent : NULL;
    if (!parent || !nde) {
        return;
    }

    if (nde->prev) {
        nde->prev->next = nde->next;
    } else {
        parent->first = nde->next;
    }

    if (nde->next) {
        nde->next->prev = nde->prev;
    } else {
        parent->tail = nde->prev;
    }

    parent->next = parent->first;
    nde->prev = NULL;
    nde->next = NULL;
}

node* create_node(t_lfu* lfu, t_bucket* bucket) {
    node* nde = calloc(1, sizeof(node));
    nde->bucket = bucket;
    return nde;
}



/*
    If node of bucket exists, move to front otherwise
    create node(bucket) and move to back
    
    This will be used for HOT buckets
    e.g. user enters a file (high chance of it being used again)
*/
t_bucket* create_or_to_front(t_lfu* lfu, t_bucket* bucket) {
    node* parent = lfu ? lfu->parent : NULL;
    if (!lfu || !parent || !bucket) {
        return NULL;
    }

    if (bucket->id < BUCKETS) {
        node* existing = lfu->by_id[bucket->id];
        if (existing) {
            detach_node(lfu, existing);
            to_front(lfu, existing);
            return NULL;
        }
    }

    node* nde = traverse_nodes(lfu, bucket);
    if (nde) {
        if (bucket->id < BUCKETS) {
            lfu->by_id[bucket->id] = nde;
        }
        detach_node(lfu, nde);
        to_front(lfu, nde);
        return NULL;
    }

    if (lfu->lru_size >= BUCKETS) {
        remove_last(lfu);
    }

    node* new_node = create_node(lfu, bucket);
    to_front(lfu, new_node);
    lfu->lru_size++;
    if (bucket->id < BUCKETS) {
        lfu->by_id[bucket->id] = new_node;
    }

}

t_bucket* remove_last(t_lfu* lfu) {
    node* parent = lfu ? lfu->parent : NULL;
    if (!lfu || !parent || !parent->tail) {
        return NULL;
    }

    node* victim = parent->tail;
    detach_node(lfu, victim);
    if (lfu->lru_size > 0) {
        lfu->lru_size--;
    }
    if (victim->bucket && victim->bucket->id < BUCKETS) {
        lfu->by_id[victim->bucket->id] = NULL;
    }

    return victim->bucket;
}

/*
    If node of bucket exists, move to back otherwise
    create node(bucket) and move to back

    This will only be used when a bucket is not HOT 
    e.g. my background folder/file scanner
    
*/
t_bucket* create_or_to_back(t_lfu* lfu, t_bucket* bucket) {
    node* parent = lfu ? lfu->parent : NULL;
    if (!lfu || !parent || !bucket) {
        return NULL;
    }

    if (bucket->id < BUCKETS) {
        node* existing = lfu->by_id[bucket->id];
        if (existing) {
            detach_node(lfu, existing);
            to_back(lfu, existing);
            return NULL;
        }
    }

    node* nde = traverse_nodes(lfu, bucket);
    if (nde) {
        if (bucket->id < BUCKETS) {
            lfu->by_id[bucket->id] = nde;
        }
        detach_node(lfu, nde);
        to_back(lfu, nde);
        return NULL;
    }

    if (lfu->lru_size >= BUCKETS) {
        remove_last(lfu);
    }

    node* new_node = create_node(lfu, bucket);
    to_back(lfu, new_node);
    lfu->lru_size++;
    if (bucket->id < BUCKETS) {
        lfu->by_id[bucket->id] = new_node;
    }
}

void to_back(t_lfu* lfu, node* new_node) {
    node* parent = lfu ? lfu->parent : NULL;
    if (!parent || !new_node) {
        return;
    }

    new_node->next = NULL;
    new_node->prev = parent->tail;
    if (parent->tail) {
        parent->tail->next = new_node;
    } else {
        parent->first = new_node;
    }
    parent->tail = new_node;
    parent->next = parent->first;
}

void to_front(t_lfu* lfu, node* new_node) {
    node* parent = lfu ? lfu->parent : NULL;
    if (!parent || !new_node) {
        return;
    }

    new_node->prev = NULL;
    new_node->next = parent->first;
    if (parent->first) {
        parent->first->prev = new_node;
    } else {
        parent->tail = new_node;
    }
    parent->first = new_node;
    parent->next = new_node;
}



/*
   Auxilliary methods
*/

node* traverse_nodes(t_lfu* lfu, t_bucket* bucket) {
    node* parent = lfu ? lfu->parent : NULL;
    if (!parent) {
        return NULL;
    }

    node* root = parent->first;

    while (root) {
        if (root->bucket == bucket) {
            return root;
        }
        root = root->next;
    }

    return NULL;
}


























