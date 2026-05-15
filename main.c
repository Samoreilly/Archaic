#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "src/threadmanager.h"
#include "src/trie-storage.h"
#include "src/trie.h"
#include "src/io/fileloader.h"

#include <pthread.h>

int main(int argc, char* argv[]) {

    t_lfu* lfu = calloc(1, sizeof(t_lfu));

    char* norm_dir = normalise_dir(argv[1]);
    printf("\nNormalised Directory: %s", norm_dir);
    
    file_thread* f_thread = (file_thread*) calloc(1, sizeof(file_thread));
    f_thread->lfu = lfu;
    spin_scan_thread(f_thread, norm_dir);//argv[1] is temporary until IPC is setup
    
    //wait until scanning worker thread has finished
    pthread_join(f_thread->worker, NULL);
    
    int dir_depth = get_dir_depth(norm_dir);
    printf("\nDir depth: %i\n", dir_depth);
    
    char* cut_dir = cutoff_dir(norm_dir);
    printf("\nCutoff directory: %s\n", cut_dir);


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
