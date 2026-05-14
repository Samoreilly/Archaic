#pragma once
#include <semaphore.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

/*
    Manages thread that handles background insertions
    Ensures one thread runs at a time
*/

typedef struct {
    pthread_t worker; 
    atomic_bool stop;
    atomic_bool running;
} state;

