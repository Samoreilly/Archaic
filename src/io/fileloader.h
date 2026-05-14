#pragma once

#include <unistd.h>
#include "../trie.h"

/*
   DISK OPERATIONS
*/
void load_trie();
void save_trie(Trie* trie);


/*
   WORKER THREADS INSERTING FOLDER/FILES NAMES INTO TRIE
*/
void scan_curr_dir(char* path);




