#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/trie.h"

int main(int argc, char* argv[]) {

    if(argc <= 1) return 1;
    
    Trie* root = (Trie*) malloc(sizeof(Trie));

    for(int i = 0;i < 2;i++) {
        printf("\n\n...\n\n");
        search(root, argv[1]);
    }

    printf("\n\n");
    
    Trie* result = search(root, strcat(argv[1], "test"));
    if(!result) return 1;

    /*
       SEND BACK TO
    */
    
    return 0;
}
