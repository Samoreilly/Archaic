#pragma once
#include <semaphore.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    pthread_t thread;
    bool running;
    atomic_bool cancel;
} scanner;

