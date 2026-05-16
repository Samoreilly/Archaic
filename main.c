#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "test/test.h"

int main(int argc, char* argv[]) {

    if (argc > 1) {
        set_test_scan_path(argv[1]);
    }
    test_main();
    perf_main(argc > 1 ? argv[1] : "/home/sam/samdev");


    /* if(argc <= 1) return 1; */
    /*  */

    /* scanner* scan = (scanner*) calloc(1, sizeof(scanner));  */
    /* Trie* root = (Trie*) malloc(sizeof(Trie)); */
    /*  */
    /* for(size_t i = 0;i < 26;i++) { */
    /*     root->children[i] = create_trie(); */
    /*     root->freq = 1; */
    /*     root->is_leaf = true; */
    /* } */

    /* for(int i = 0;i < 2;i++) { */
    /*     printf("\n\n...\n\n"); */
    /*     search(root, scan, argv[1]); */
    /* } */

    /* char* str = (char*) malloc(sizeof(char)); */
    /* strcat(str, "hellotest"); */
    /*      */
    /* while(true) { */
    /*     //simulating wait for IPC */
    /*     printf("waiting.."); */
    /*     Trie* result = search(root, scan, str); */
    /*     sleep(1);         */
    /* } */
    /*     */
    /*     SEND BACK TO TERMINAL PROCESS */
    /*     */ 

    return 0;
}
