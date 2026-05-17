#pragma once

#include <unistd.h>
#include <stdint.h>
#include "../threadmanager.h"
#include "../scanner.h"
#include "../metrics.h"
#include "../trie.h"
#include "../trie-storage.h"

struct ipc_server;

void load_trie();
void save_trie(Trie* trie);

typedef struct {
    t_bucket_store* store;
    struct node* parent;
    parallel_scanner scanner;
    struct ipc_server* ipc;
    metrics_t metrics;
} daemon_state;

void daemon_save_state(daemon_state* state, const char* path);

path_validation process_input(t_bucket_store* store, const char* cwd, const char* input);

daemon_state* daemon_init(void);
void daemon_shutdown(daemon_state* state);
void daemon_run_scan(daemon_state* state, const char* path);
path_validation daemon_process_query(daemon_state* state, const char* cwd, const char* input);

completions* daemon_get_completions(daemon_state* state, const char* prefix, size_t limit);

scored_completions* daemon_get_scored_completions(daemon_state* state, const char* prefix, size_t limit, uint64_t now);

int daemon_start_ipc(daemon_state* state, const char* sock_path);
