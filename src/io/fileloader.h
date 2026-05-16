#pragma once

#include <unistd.h>
#include "../threadmanager.h"
#include "../trie.h"
#include "../trie-storage.h"

struct ipc_server;

/*
   DISK OPERATIONS
*/
void load_trie();
void save_trie(Trie* trie);


/*
   WORKER THREADS INSERTING FOLDER/FILES NAMES INTO TRIE
*/
void spin_scan_thread(file_thread* f_thread, char* path);
void* scan_curr_dir(void* args);

/*
   DAEMON INPUT PROCESSING
   Validates cwd + input against filesystem before inserting into trie
*/
path_validation process_input(t_bucket_store* store, const char* cwd, const char* input);

/*
   DAEMON STATE
   Owns the bucket store, scan state, and lifecycle
*/
typedef struct {
    t_bucket_store* store;
    struct node* parent;
    file_thread* scanner;
    struct ipc_server* ipc;
} daemon_state;

daemon_state* daemon_init(void);
void daemon_shutdown(daemon_state* state);
void daemon_run_scan(daemon_state* state, const char* path);
path_validation daemon_process_query(daemon_state* state, const char* cwd, const char* input);

/*
   Query the daemon for completions matching a prefix.
   Returns a completions struct that must be freed with completions_free().
*/
completions* daemon_get_completions(daemon_state* state, const char* prefix, size_t limit);

/*
   Query the daemon for scored completions matching a prefix.
   Returns a scored_completions struct sorted by score descending.
*/
scored_completions* daemon_get_scored_completions(daemon_state* state, const char* prefix, size_t limit, uint64_t now);

/*
   Start the IPC server. Call after daemon_init().
*/
int daemon_start_ipc(daemon_state* state, const char* sock_path);



