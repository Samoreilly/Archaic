#pragma once

#include "../cache.h"
#include "../config.h"
#include "../incremental.h"
#include "../metrics.h"
#include "../recent-files.h"
#include "../scanner.h"
#include "../threadmanager.h"
#include "../trie-storage.h"
#include "../trie.h"
#include "../watcher.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

struct ipc_server;

typedef struct {
    bool scanning;
    size_t buckets_so_far;
    size_t project_count;
    project_info projects[MAX_PROJECTS];
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
    atomic_bool scanner_healthy;
    atomic_size_t scan_bucket_count;
    char last_scan_paths[CONFIG_MAX_ROOTS][CONFIG_MAX_STRING];
    int last_scan_path_count;
    int rescan_interval_seconds;
    pthread_t rescan_timer_thread;
    atomic_bool rescan_timer_running;
    atomic_bool config_reload_requested;
    recent_files recent;
    incremental_state incremental;
    bool case_insensitive;
    char bookmarks[CONFIG_MAX_BOOKMARKS][CONFIG_MAX_STRING];
    int bookmark_count;
    fs_watcher* watcher;
    atomic_bool watcher_dirty;
} daemon_state;

int load_trie(daemon_state* state, const char* path);
int save_trie(daemon_state* state, const char* path);

void daemon_save_state(daemon_state* state, const char* path);

path_validation process_input(t_bucket_store* store, const char* cwd, const char* input);

daemon_state* daemon_init(void);
void daemon_shutdown(daemon_state* state);
void daemon_run_scan(daemon_state* state, const char* path);
void daemon_run_scan_multi(daemon_state* state, const char** paths, int path_count);
void daemon_start_rescan_timer(daemon_state* state);
void daemon_stop_rescan_timer(daemon_state* state);
scan_status daemon_scan_status(daemon_state* state);
path_validation daemon_process_query(daemon_state* state, const char* cwd, const char* input);

completions* daemon_get_completions(daemon_state* state, const char* prefix, size_t limit);

typedef struct {
    const scored_completions* data;
    bool from_cache;
} scored_result;

scored_result daemon_get_scored_completions(daemon_state* state, const char* prefix, size_t limit,
                                            uint64_t now, const char* cwd);
void daemon_release_scored(daemon_state* state, scored_result result);

completions* daemon_get_fuzzy_completions(daemon_state* state, const char* query, size_t limit);

int daemon_get_recent_files(daemon_state* state, char** paths, bool* is_dirs, int n);

void daemon_touch_recent(daemon_state* state, const char* path, bool is_dir);

int daemon_start_ipc(daemon_state* state, const char* sock_path);

int daemon_export_frequencies(daemon_state* state, const char* path);
int daemon_import_frequencies(daemon_state* state, const char* path);
int daemon_export_json(daemon_state* state, const char* path);

void daemon_prefetch_common_prefixes(daemon_state* state);

void daemon_log_query(daemon_state* state, const char* prefix, const char* cwd, size_t result_count);
