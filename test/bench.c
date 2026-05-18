#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/trie.h"

#define BENCH_ITERATIONS 10000
#define BENCH_PATHS 10000
#define BENCH_QUERY_COUNT 1000
#define BENCH_FUZZY_COUNT 500

static double bench_timer_ms(struct timespec* start, struct timespec* end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static void bench_trie_insert(void) {
    Trie* root = create_trie();
    char path[256];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < BENCH_PATHS; i++) {
        snprintf(path, sizeof(path), "/bench/path/%d/file_%d.c", i / 100, i);
        insert(root, path);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double ms = bench_timer_ms(&start, &end);
    printf("trie_insert,%d,%.3f,%.0f\n", BENCH_PATHS, ms, (double) BENCH_PATHS / (ms / 1000.0));
    trie_free_recursive(root);
}

static void bench_trie_query(void) {
    Trie* root = create_trie();
    char path[256];
    for (int i = 0; i < BENCH_PATHS; i++) {
        snprintf(path, sizeof(path), "/bench/path/%d/file_%d.c", i / 100, i);
        insert(root, path);
    }

    completions* c = completions_create(100);
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < BENCH_QUERY_COUNT; i++) {
        completions_collect(root, "/bench/path/", c);
        c->count = 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double ms = bench_timer_ms(&start, &end);
    printf("trie_query,%d,%.3f,%.0f\n", BENCH_QUERY_COUNT, ms,
           (double) BENCH_QUERY_COUNT / (ms / 1000.0));
    completions_free(c);
    trie_free_recursive(root);
}

static void bench_trie_fuzzy(void) {
    Trie* root = create_trie();
    char path[256];
    for (int i = 0; i < BENCH_PATHS; i++) {
        snprintf(path, sizeof(path), "/bench/src/%d/mod_%d.rs", i / 50, i);
        insert(root, path);
    }

    char* results[50];
    bool is_dirs[50];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < BENCH_FUZZY_COUNT; i++) {
        snprintf(path, sizeof(path), "src%d", i % 100);
        trie_fuzzy_collect(root, path, results, is_dirs, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double ms = bench_timer_ms(&start, &end);
    printf("trie_fuzzy,%d,%.3f,%.0f\n", BENCH_FUZZY_COUNT, ms,
           (double) BENCH_FUZZY_COUNT / (ms / 1000.0));
    trie_free_recursive(root);
}

int main(void) {
    printf("benchmark,iterations,time_ms,ops_per_sec\n");
    bench_trie_insert();
    bench_trie_query();
    bench_trie_fuzzy();
    return 0;
}
