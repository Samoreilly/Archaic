#pragma once

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include "../threadmanager.h"
#include "../scanner.h"
#include "../metrics.h"
#include "../trie.h"
#include "../trie-storage.h"
#include "../cache.h"

struct ipc_server;

typedef struct {
    bool scanning;
    size_t buckets_so_far;
} scan_status;

typedef struct {
    t_bucket_store* store;
    struct node* parent;
    parallel_scanner scanner;
    struct ipc_server* ipc;
    metrics_t metrics;
    query_cache* cache;
    pthread_t scan_thread;
    atomic_bool scanning;
    atomic_size_t scan_bucket_count;
} daemon_state;

int load_trie(daemon_state* state, const char* path);
int save_trie(daemon_state* state, const char* path);

void daemon_save_state(daemon_state* state, const char* path);

path_validation process_input(t_bucket_store* store, const char* cwd, const char* input);

daemon_state* daemon_init(void);
void daemon_shutdown(daemon_state* state);
void daemon_run_scan(daemon_state* state, const char* path);
scan_status daemon_scan_status(daemon_state* state);
path_validation daemon_process_query(daemon_state* state, const char* cwd, const char* input);

completions* daemon_get_completions(daemon_state* state, const char* prefix, size_t limit);

scored_completions* daemon_get_scored_completions(daemon_state* state, const char* prefix, size_t limit, uint64_t now);

int daemon_start_ipc(daemon_state* state, const char* sock_path);
