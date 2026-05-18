#pragma once

#include <stddef.h>

struct daemon_state;

int mmap_state_save(daemon_state* state, const char* path);
int mmap_state_load(daemon_state* state, const char* path);
