#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "fileloader.h"
#include "../threadmanager.h"
#include "../trie-storage.h"

void load_trie() {

}

void save_trie(Trie* trie) {

}

void spin_scan_thread(file_thread* f_thread, char* path) {
    printf("\nEntered spin scan [spin_scan_thread]\n");
    //if another thread is running, wait until completed
    if(f_thread->running) {
        f_thread->stop = true;
        pthread_join(f_thread->worker, NULL);
        f_thread->running = false;
    }

    f_thread->running = true;
    f_thread->path = path;
    printf("\nCreating scan pthread_t\n");
    pthread_create(&f_thread->worker, NULL, scan_curr_dir, (void*) f_thread);
}

void* scan_curr_dir(void* args) {
    printf("\n[Scan_curr_dir]-----\n");

    file_thread* f_thread = (file_thread*) args;
    printf("\n\nScanning directory");

    DIR* dir = opendir(f_thread->path);
    
    if(!dir) {
        printf("\nNo directory found");
        return NULL;
    }

    struct dirent *entry;

    while((entry = readdir(dir))) {
        printf("\nEntries..\n");
        if(f_thread->stop) {
            return NULL;
        }
        
        if(entry->d_type == DT_REG) {
            printf("\nFile name: %s Full path: %s/%s\n", entry->d_name, f_thread->path, entry->d_name);
                
            size_t len = strlen(f_thread->path) + 1 + strlen(entry->d_name) + 1;
            char* path = (char*) malloc(len);
            
            snprintf(path, len, "%s/%s", f_thread->path, entry->d_name);

            find_bucket(f_thread->lfu, path, path, 3, false);
            free(path);

        }else if(entry->d_type == DT_DIR) {
            printf("\nFolder name: %s Full path: %s/%s\n", entry->d_name, f_thread->path, entry->d_name);             
            //TODO: Recurse through folders and add to trie
            printf("\nFolder: %s", entry->d_name);
        }
        
    }


}




