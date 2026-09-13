#pragma once

#include <stddef.h>

size_t path_normalize(char* dst, const char* src, size_t dst_size);
size_t path_expand_tilde(char* dst, const char* src, size_t dst_size);
size_t path_expand_dash(char* dst, const char* oldpwd, size_t dst_size);
size_t path_expand_abbrev(char* dst, const char* src, size_t dst_size);

/* True if path contains a ".." path component ("/../", leading "../",
 * trailing "/..", or exactly ".."). Substrings like "a..b" or "file..bak"
 * do NOT count. Use after expansion, before or after normalize. */
int path_has_dotdot_component(const char* path);