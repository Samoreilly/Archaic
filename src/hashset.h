#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char** entries;
    size_t capacity;
    size_t count;
} hashset;

void hashset_init(hashset* hs, size_t capacity);
void hashset_insert(hashset* hs, const char* str);
bool hashset_contains(hashset* hs, const char* str);
void hashset_free(hashset* hs);
int hashset_resize(hashset* hs);
