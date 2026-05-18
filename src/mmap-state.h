#pragma once

#include <stddef.h>

struct daemon_state;

int mmap_state_save(struct daemon_state* state, const char* path);
int mmap_state_load(struct daemon_state* state, const char* path);
