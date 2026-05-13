#include <stdio.h>
#include <stdlib.h>

#include "src/trie.h"

int main(int argc, char* argv[]) {

    if(argc <= 1) return 1;
    
    Trie* root = (Trie*) malloc(sizeof(Trie));

    for(int i = 0;i < 2;i++) {
        printf("\n\n...\n\n");
        insert(root, argv[1]);
    }
    return 0;
}
