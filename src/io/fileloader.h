#pragma once

#include <unistd.h>
#include "../trie.h"

/*
   DISK OPERATIONS
*/
void load_trie();
void save_trie(Trie* trie);


/*
   WORKER THREADS INSERTING FILES INTO TRIE
*/

void spin_up();
void put();




