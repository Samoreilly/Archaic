#include "hashset.h"
#include <stdlib.h>
#include <string.h>

#define HASHSET_MIN_CAPACITY 16
#define FNV_OFFSET_BASIS 0xCBF29CE484222325ULL
#define FNV_PRIME 0x100000001B3ULL

static size_t fnv1a_hash(const char* str) {
    size_t hash = FNV_OFFSET_BASIS;
    while (*str) {
        hash ^= (size_t) (unsigned char) *str++;
        hash *= FNV_PRIME;
    }
    return hash;
}

void hashset_init(hashset* hs, size_t capacity) {
    if (capacity < HASHSET_MIN_CAPACITY)
        capacity = HASHSET_MIN_CAPACITY;
    size_t actual = 1;
    while (actual < capacity)
        actual <<= 1;
    hs->entries = calloc(actual, sizeof(char*));
    hs->capacity = actual;
    hs->count = 0;
}

void hashset_insert(hashset* hs, const char* str) {
    if (!str || !hs || !hs->entries)
        return;
    size_t idx = fnv1a_hash(str) & (hs->capacity - 1);
    for (size_t i = 0; i < hs->capacity; i++) {
        size_t pos = (idx + i) & (hs->capacity - 1);
        if (!hs->entries[pos]) {
            hs->entries[pos] = strdup(str);
            hs->count++;
            return;
        }
        if (strcmp(hs->entries[pos], str) == 0)
            return;
    }
}

bool hashset_contains(hashset* hs, const char* str) {
    if (!str || !hs || !hs->entries || hs->count == 0)
        return false;
    size_t idx = fnv1a_hash(str) & (hs->capacity - 1);
    for (size_t i = 0; i < hs->capacity; i++) {
        size_t pos = (idx + i) & (hs->capacity - 1);
        if (!hs->entries[pos])
            return false;
        if (strcmp(hs->entries[pos], str) == 0)
            return true;
    }
    return false;
}

void hashset_free(hashset* hs) {
    if (!hs || !hs->entries)
        return;
    for (size_t i = 0; i < hs->capacity; i++) {
        free(hs->entries[i]);
    }
    free(hs->entries);
    hs->entries = NULL;
    hs->capacity = 0;
    hs->count = 0;
}