#pragma once
#include <semaphore.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

//struct t_bucket_store;
/*
    Manages thread that handles background insertions
    Ensures one thread runs at a time
*/

typedef struct {
    pthread_t worker; 
    atomic_bool stop;
    atomic_bool running;
} state;

typedef struct {
    pthread_t worker;
    char* path;
    atomic_bool stop;
    atomic_bool running;

    struct t_bucket_store* lfu;
    struct node* parent;

} file_thread;
