#pragma once
#include <stdbool.h>

#define IGNORE_FILE_MAX_PATTERNS 256
#define IGNORE_FILE_MAX_PATTERN_LEN 512

typedef struct {
    char pattern[IGNORE_FILE_MAX_PATTERN_LEN];
    bool is_dir_only;
    bool is_negation;
} ignore_pattern;

typedef struct {
    ignore_pattern patterns[IGNORE_FILE_MAX_PATTERNS];
    int count;
} ignore_file;

int ignore_file_load(ignore_file* ig, const char* path);
bool ignore_file_should_ignore(const ignore_file* ig, const char* name, bool is_dir);
void ignore_file_clear(ignore_file* ig);
