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

    /* Auto-resize if load factor > 0.75 */
    if (hs->count > 0 && (size_t) hs->count * 4 > hs->capacity * 3) {
        hashset_resize(hs);
    }

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

int hashset_resize(hashset* hs) {
    if (!hs || !hs->entries)
        return -1;

    size_t new_capacity = hs->capacity;
    while (new_capacity < (hs->count + 1) * 2) {
        new_capacity *= 2;
    }
    if (new_capacity == hs->capacity)
        return 0;

    char** new_entries = calloc(new_capacity, sizeof(char*));
    if (!new_entries)
        return -1;

    size_t new_mask = new_capacity - 1;
    for (size_t i = 0; i < hs->capacity; i++) {
        if (hs->entries[i]) {
            size_t idx = fnv1a_hash(hs->entries[i]) & new_mask;
            for (size_t j = 0; j < new_capacity; j++) {
                size_t pos = (idx + j) & new_mask;
                if (!new_entries[pos]) {
                    new_entries[pos] = hs->entries[i];
                    break;
                }
            }
        }
    }

    free(hs->entries);
    hs->entries = new_entries;
    hs->capacity = new_capacity;
    return 0;
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