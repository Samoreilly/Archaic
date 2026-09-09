#include "ignore-file.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>

void ignore_file_clear(ignore_file* ig) {
    ig->count = 0;
}

int ignore_file_load(ignore_file* ig, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;

    int loaded = 0;
    char line[IGNORE_FILE_MAX_PATTERN_LEN];

    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1]))
            p[--len] = '\0';
        if (len == 0)
            continue;

        if (ig->count >= IGNORE_FILE_MAX_PATTERNS)
            break;

        ignore_pattern* pat = &ig->patterns[ig->count];
        pat->is_negation = false;
        pat->is_dir_only = false;

        if (*p == '!') {
            pat->is_negation = true;
            p++;
            len--;
        }

        if (len > 0 && p[len - 1] == '/') {
            pat->is_dir_only = true;
            p[len - 1] = '\0';
            len--;
        }

        if (len == 0)
            continue;

        strncpy(pat->pattern, p, IGNORE_FILE_MAX_PATTERN_LEN - 1);
        pat->pattern[IGNORE_FILE_MAX_PATTERN_LEN - 1] = '\0';
        ig->count++;
        loaded++;
    }

    fclose(f);
    return loaded;
}

bool ignore_file_should_ignore(const ignore_file* ig, const char* name, bool is_dir) {
    if (!ig || ig->count == 0)
        return false;

    bool ignored = false;

    for (int i = 0; i < ig->count; i++) {
        const ignore_pattern* pat = &ig->patterns[i];

        if (pat->is_dir_only && !is_dir)
            continue;

        if (fnmatch(pat->pattern, name, 0) == 0)
            ignored = !pat->is_negation;
    }

    return ignored;
}
