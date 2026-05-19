#pragma once

#include <stddef.h>

size_t path_normalize(char* dst, const char* src, size_t dst_size);
size_t path_expand_tilde(char* dst, const char* src, size_t dst_size);
size_t path_expand_dash(char* dst, const char* src, size_t dst_size);
size_t path_expand_abbrev(char* dst, const char* src, size_t dst_size);