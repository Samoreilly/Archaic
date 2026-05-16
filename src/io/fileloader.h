#pragma once

#include <unistd.h>
#include "../threadmanager.h"
#include "../trie.h"
#include "../trie-storage.h"

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



