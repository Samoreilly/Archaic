#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

#include "../src/threadmanager.h"
#include "../src/trie-storage.h"
#include "../src/trie.h"
#include "../src/io/fileloader.h"
#include "../src/lru.h"

static const char* test_scan_path = NULL;

void set_test_scan_path(const char* path) {
    test_scan_path = path;
}

static char* path_join(const char* a, const char* b) {
    size_t len = strlen(a) + 1 + strlen(b) + 1;
    char* out = malloc(len);
    snprintf(out, len, "%s/%s", a, b);
    return out;
}

/*
    Helper: check if a string is in a completions result
*/
static bool completions_contains(completions* c, const char* str) {
    if (!c || !str) return false;
    for (size_t i = 0; i < c->count; i++) {
        if (strcmp(c->paths[i], str) == 0) return true;
    }
    return false;
}

static void print_completions(completions* c, const char* label) {
    printf("  [%s] %zu completions:", label, c->count);
    for (size_t i = 0; i < c->count && i < 10; i++) {
        printf("\n    %s", c->paths[i]);
    }
    if (c->count > 10) printf("\n    ... and %zu more", c->count - 10);
    printf("\n");
}

/*
    TEST 1: Scan population
    Verifies background scan actually inserts paths into the trie.
*/
static bool test_scan_population(daemon_state* daemon, const char* scan_root) {
    printf("\n[Test 1] Scan population\n");

    char* prefix = path_join(scan_root, "");
    completions* c = daemon_get_completions(daemon, prefix, 50);
    free(prefix);

    if (!c) {
        printf("  FAIL: no completions returned\n");
        return false;
    }

    bool pass = c->count > 0;

    printf("  Total completions: %zu\n", c->count);
    if (c->count > 0) {
        printf("  Sample: %s\n", c->paths[0]);
    }

    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    completions_free(c);
    return pass;
}

/*
    TEST 2: Valid path insertion
    Query a valid path, verify it gets inserted and appears in completions.
*/
static bool test_valid_insert(daemon_state* daemon, const char* scan_root) {
    printf("\n[Test 2] Valid path insertion\n");

    char* test_file = path_join(scan_root, "testfile_probe.tmp");
    FILE* f = fopen(test_file, "w");
    if (f) {
        fclose(f);
    } else {
        free(test_file);
        printf("  SKIP: cannot create temp file\n");
        return true;
    }

    path_validation v = daemon_process_query(daemon, scan_root, "testfile_probe.tmp");
    bool inserted = v.exists;
    free_path_validation(&v);

    char* comp_prefix = path_join(scan_root, "testfile");
    completions* c = daemon_get_completions(daemon, comp_prefix, 10);
    bool found = completions_contains(c, test_file);

    printf("  Path exists: %s\n", inserted ? "yes" : "no");
    printf("  Found in completions: %s\n", found ? "yes" : "no");

    bool pass = inserted && found;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    completions_free(c);
    free(comp_prefix);
    free(test_file);
    unlink(test_file);
    return pass;
}

/*
    TEST 3: Invalid path rejection
    Query a non-existent path, verify it does NOT get inserted.
*/
static bool test_invalid_reject(daemon_state* daemon, const char* scan_root) {
    printf("\n[Test 3] Invalid path rejection\n");

    const char* cwd = scan_root;
    const char* input = "nonexistent_garbage_xyz_12345";

    path_validation v = daemon_process_query(daemon, cwd, input);
    bool rejected = !v.exists;
    free_path_validation(&v);

    char* comp_prefix = path_join(scan_root, "nonexistent");
    completions* c = daemon_get_completions(daemon, comp_prefix, 10);
    bool absent = (c->count == 0);
    free(comp_prefix);

    printf("  Path rejected: %s\n", rejected ? "yes" : "no");
    printf("  Absent from completions: %s\n", absent ? "yes" : "no");

    bool pass = rejected && absent;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    completions_free(c);
    return pass;
}

/*
    TEST 4: Partial prefix completion
    Query a partial prefix, verify matching completions are returned.
*/
static bool test_partial_prefix(daemon_state* daemon, const char* scan_root) {
    printf("\n[Test 4] Partial prefix completion\n");

    char* probe_file = path_join(scan_root, "probe_partial.txt");
    FILE* f = fopen(probe_file, "w");
    if (f) {
        fclose(f);
    } else {
        free(probe_file);
        printf("  SKIP: cannot create temp file\n");
        return true;
    }

    path_validation v = daemon_process_query(daemon, scan_root, "probe_partial.txt");
    free_path_validation(&v);

    char* comp_prefix = path_join(scan_root, "probe_part");
    completions* c = daemon_get_completions(daemon, comp_prefix, 10);
    bool found = completions_contains(c, probe_file);

    printf("  Prefix: %s\n", comp_prefix);
    printf("  Contains probe file: %s\n", found ? "yes" : "no");
    print_completions(c, "results");

    bool pass = found;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    completions_free(c);
    free(comp_prefix);
    free(probe_file);
    unlink(probe_file);
    return pass;
}

/*
    TEST 5: Idempotency
    Query the same valid path multiple times, verify trie state doesn't corrupt.
*/
static bool test_idempotency(daemon_state* daemon, const char* scan_root) {
    printf("\n[Test 5] Idempotency (10x same query)\n");

    char* probe_file = path_join(scan_root, "idempotency_probe.txt");
    FILE* f = fopen(probe_file, "w");
    if (f) {
        fclose(f);
    } else {
        free(probe_file);
        printf("  SKIP: cannot create temp file\n");
        return true;
    }

    for (int i = 0; i < 10; i++) {
        path_validation v = daemon_process_query(daemon, scan_root, "idempotency_probe.txt");
        free_path_validation(&v);
    }

    char* comp_prefix = path_join(scan_root, "idempotency");
    completions* c = daemon_get_completions(daemon, comp_prefix, 10);
    bool found = completions_contains(c, probe_file);

    printf("  Found after 10 queries: %s\n", found ? "yes" : "no");
    printf("  Completion count: %zu\n", c->count);

    bool pass = found && c->count <= 5;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    completions_free(c);
    free(comp_prefix);
    free(probe_file);
    unlink(probe_file);
    return pass;
}

/*
    TEST 6: Absolute path handling
    Query an absolute path (not relative to cwd), verify it works.
*/
static bool test_absolute_path(daemon_state* daemon, const char* scan_root) {
    printf("\n[Test 6] Absolute path handling\n");

    char* probe_file = path_join(scan_root, "abs_path_probe.txt");
    FILE* f = fopen(probe_file, "w");
    if (f) {
        fclose(f);
    } else {
        free(probe_file);
        printf("  SKIP: cannot create temp file\n");
        return true;
    }

    const char* cwd = "/tmp";
    const char* input = probe_file;

    path_validation v = daemon_process_query(daemon, cwd, input);
    bool exists = v.exists;
    free_path_validation(&v);

    char* comp_prefix = path_join(scan_root, "abs_path");
    completions* c = daemon_get_completions(daemon, comp_prefix, 10);
    bool found = completions_contains(c, probe_file);

    printf("  Absolute path exists: %s\n", exists ? "yes" : "no");
    printf("  Found in completions: %s\n", found ? "yes" : "no");

    bool pass = exists && found;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    completions_free(c);
    free(comp_prefix);
    free(probe_file);
    unlink(probe_file);
    return pass;
}

/*
    TEST 7: Empty input handling
    Query empty string, verify it doesn't crash or insert garbage.
*/
static bool test_empty_input(daemon_state* daemon, const char* scan_root) {
    printf("\n[Test 7] Empty input handling\n");

    const char* cwd = scan_root;
    const char* input = "";

    path_validation v = daemon_process_query(daemon, cwd, input);
    free_path_validation(&v);

    completions* c = daemon_get_completions(daemon, "", 5);
    bool no_crash = (c != NULL);

    printf("  No crash: %s\n", no_crash ? "yes" : "no");
    printf("  Completions returned: %zu\n", c ? c->count : 0);

    bool pass = no_crash;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    if (c) completions_free(c);
    return pass;
}

/*
    TEST 8: Concurrent query stress test
    Multiple threads querying simultaneously, verify no corruption.
*/
#define CONCURRENT_QUERY_THREADS 8
#define QUERIES_PER_THREAD 100

typedef struct {
    daemon_state* daemon;
    const char* scan_root;
    int thread_id;
    atomic_int* success;
    atomic_int* fail;
    pthread_barrier_t* barrier;
} query_ctx;

static void* query_worker(void* arg) {
    query_ctx* ctx = (query_ctx*) arg;
    pthread_barrier_wait(ctx->barrier);

    char* probe_prefix = path_join(ctx->scan_root, "concurrent_probe_");

    for (int i = 0; i < QUERIES_PER_THREAD; i++) {
        char filename[64];
        snprintf(filename, sizeof(filename), "concurrent_probe_%d.txt", i % 20);
        char* probe_file = path_join(ctx->scan_root, filename);

        FILE* f = fopen(probe_file, "w");
        if (f) {
            path_validation v = daemon_process_query(ctx->daemon, ctx->scan_root, filename);
            free_path_validation(&v);
            fclose(f);
            unlink(probe_file);
        }
        free(probe_file);

        char* comp_prefix = path_join(ctx->scan_root, "concurrent");
        completions* c = daemon_get_completions(ctx->daemon, comp_prefix, 10);
        free(comp_prefix);
        if (c) {
            atomic_fetch_add(ctx->success, 1);
            completions_free(c);
        } else {
            atomic_fetch_add(ctx->fail, 1);
        }
    }

    free(probe_prefix);
    return NULL;
}

static bool test_concurrent_queries(daemon_state* daemon, const char* scan_root) {
    printf("\n[Test 8] Concurrent query stress (%d threads x %d queries)\n",
           CONCURRENT_QUERY_THREADS, QUERIES_PER_THREAD);

    atomic_int success;
    atomic_int fail;
    atomic_init(&success, 0);
    atomic_init(&fail, 0);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, CONCURRENT_QUERY_THREADS);

    pthread_t threads[CONCURRENT_QUERY_THREADS];
    query_ctx ctx[CONCURRENT_QUERY_THREADS];

    for (int i = 0; i < CONCURRENT_QUERY_THREADS; i++) {
        ctx[i].daemon = daemon;
        ctx[i].scan_root = scan_root;
        ctx[i].thread_id = i;
        ctx[i].success = &success;
        ctx[i].fail = &fail;
        ctx[i].barrier = &barrier;
        pthread_create(&threads[i], NULL, query_worker, &ctx[i]);
    }

    time_t start = time(NULL);
    bool deadlock = false;
    int joined = 0;

    while (joined < CONCURRENT_QUERY_THREADS) {
        if (time(NULL) - start >= 10) {
            deadlock = true;
            break;
        }
        for (int i = 0; i < CONCURRENT_QUERY_THREADS; i++) {
            if (threads[i] == 0) continue;
            if (pthread_kill(threads[i], 0) != 0) {
                threads[i] = 0;
                joined++;
                continue;
            }
            if (pthread_join(threads[i], NULL) == 0) {
                threads[i] = 0;
                joined++;
            }
        }
        if (joined < CONCURRENT_QUERY_THREADS) usleep(10000);
    }

    pthread_barrier_destroy(&barrier);

    int total_success = atomic_load(&success);
    int total_fail = atomic_load(&fail);

    printf("  Completed: %d queries\n", total_success);
    printf("  Failed: %d queries\n", total_fail);
    printf("  Deadlock: %s\n", deadlock ? "YES" : "no");

    bool pass = !deadlock && total_fail == 0 && total_success == CONCURRENT_QUERY_THREADS * QUERIES_PER_THREAD;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    return pass;
}

/*
    TEST 9: Thread safety with lock verification
    Same as previous stress test but focused on trie integrity.
*/
#define LOCK_WRITERS 6
#define LOCK_READERS 4
#define LOCK_STRINGS 200

typedef struct {
    t_bucket* bucket;
    int thread_id;
    atomic_int* ops;
    pthread_barrier_t* barrier;
} lock_ctx;

static void* lock_writer(void* arg) {
    lock_ctx* ctx = (lock_ctx*) arg;
    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < LOCK_STRINGS; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "lock_test_%d_%d", ctx->thread_id, i);
        trie_lock(ctx->bucket);
        insert(ctx->bucket->dir_trie, buf);
        ctx->bucket->dir_count++;
        trie_unlock(ctx->bucket);
        atomic_fetch_add(ctx->ops, 1);
    }
    return NULL;
}

static void* lock_reader(void* arg) {
    lock_ctx* ctx = (lock_ctx*) arg;
    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < LOCK_STRINGS; i++) {
        int wid = i % LOCK_WRITERS;
        int sid = i % LOCK_STRINGS;
        char buf[64];
        snprintf(buf, sizeof(buf), "lock_test_%d_%d", wid, sid);

        trie_lock(ctx->bucket);
        Trie* curr = ctx->bucket->dir_trie;
        for (size_t j = 0; buf[j] != '\0'; ) {
            RadixChild* child = NULL;
            for (uint8_t k = 0; k < curr->child_count; k++) {
                if (curr->children[k].edge_char == buf[j]) {
                    child = &curr->children[k];
                    break;
                }
            }
            if (!child) break;
            Trie* child_node = child->node;
            size_t match = 0;
            while (match < child_node->key_len && buf[j + match] != '\0' &&
                   child_node->key[match] == buf[j + match]) {
                match++;
            }
            if (match == 0) break;
            j += match;
            curr = child_node;
        }
        trie_unlock(ctx->bucket);
        atomic_fetch_add(ctx->ops, 1);
    }
    return NULL;
}

static bool test_lock_integrity(void) {
    printf("\n[Test 9] Lock integrity (%d writers + %d readers)\n",
           LOCK_WRITERS, LOCK_READERS);

    t_bucket* bucket = create_bucket("lock_test");
    if (!bucket) {
        printf("  FAIL: could not create bucket\n");
        return false;
    }

    atomic_int ops;
    atomic_init(&ops, 0);

    pthread_barrier_t barrier;
    int total = LOCK_WRITERS + LOCK_READERS;
    pthread_barrier_init(&barrier, NULL, total);

    pthread_t writers[LOCK_WRITERS];
    pthread_t readers[LOCK_READERS];
    lock_ctx wctx[LOCK_WRITERS];
    lock_ctx rctx[LOCK_READERS];

    for (int i = 0; i < LOCK_WRITERS; i++) {
        wctx[i].bucket = bucket;
        wctx[i].thread_id = i;
        wctx[i].ops = &ops;
        wctx[i].barrier = &barrier;
        pthread_create(&writers[i], NULL, lock_writer, &wctx[i]);
    }

    for (int i = 0; i < LOCK_READERS; i++) {
        rctx[i].bucket = bucket;
        rctx[i].thread_id = i;
        rctx[i].ops = &ops;
        rctx[i].barrier = &barrier;
        pthread_create(&readers[i], NULL, lock_reader, &rctx[i]);
    }

    time_t start = time(NULL);
    bool deadlock = false;
    int joined = 0;
    pthread_t all[LOCK_WRITERS + LOCK_READERS];
    for (int i = 0; i < LOCK_WRITERS; i++) all[i] = writers[i];
    for (int i = 0; i < LOCK_READERS; i++) all[LOCK_WRITERS + i] = readers[i];

    while (joined < total) {
        if (time(NULL) - start >= 10) {
            deadlock = true;
            break;
        }
        for (int i = 0; i < total; i++) {
            if (all[i] == 0) continue;
            if (pthread_kill(all[i], 0) != 0) {
                all[i] = 0;
                joined++;
                continue;
            }
            if (pthread_join(all[i], NULL) == 0) {
                all[i] = 0;
                joined++;
            }
        }
        if (joined < total) usleep(10000);
    }

    pthread_barrier_destroy(&barrier);

    int expected_ops = (LOCK_WRITERS + LOCK_READERS) * LOCK_STRINGS;
    int actual_ops = atomic_load(&ops);

    int missing = 0;
    for (int w = 0; w < LOCK_WRITERS; w++) {
        for (int i = 0; i < LOCK_STRINGS; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "lock_test_%d_%d", w, i);
            trie_lock(bucket);
            Trie* curr = bucket->dir_trie;
            bool found = true;
            for (size_t j = 0; buf[j] != '\0'; ) {
                RadixChild* child = NULL;
                for (uint8_t k = 0; k < curr->child_count; k++) {
                    if (curr->children[k].edge_char == buf[j]) {
                        child = &curr->children[k];
                        break;
                    }
                }
                if (!child) { found = false; break; }
                Trie* child_node = child->node;
                size_t match = 0;
                while (match < child_node->key_len && buf[j + match] != '\0' &&
                       child_node->key[match] == buf[j + match]) {
                    match++;
                }
                if (match == 0) { found = false; break; }
                j += match;
                curr = child_node;
            }
            if (found && !curr->is_leaf) found = false;
            trie_unlock(bucket);
            if (!found) missing++;
        }
    }

    int lock_destroy_rc = pthread_mutex_destroy(&bucket->lock);

    printf("  Expected ops: %d, Actual: %d\n", expected_ops, actual_ops);
    printf("  Missing strings: %d\n", missing);
    printf("  Lock destroyed cleanly: %s\n", lock_destroy_rc == 0 ? "yes" : "no");
    printf("  Deadlock: %s\n", deadlock ? "YES" : "no");

    bool pass = !deadlock && missing == 0 && actual_ops == expected_ops && lock_destroy_rc == 0;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    destroy_bucket(bucket);

    return pass;
}

void test_main(void) {
    printf("\n========================================\n");
    printf("  Full Pipeline Integration Tests\n");
    printf("========================================\n");

    daemon_state* daemon = daemon_init();
    if (!daemon) {
        printf("\n[FAIL] Daemon init failed\n");
        return;
    }

    const char* base_path = test_scan_path ? test_scan_path : "/home/sam/samdev";
    printf("\n[Setup] Scanning: %s\n", base_path);
    daemon_run_scan(daemon, base_path);
    printf("[Setup] Scan complete\n");

    int total = 0;
    int passed = 0;

    #define RUN_TEST(fn) do { \
        total++; \
        if (fn) passed++; \
    } while(0)

    RUN_TEST(test_scan_population(daemon, base_path));
    RUN_TEST(test_valid_insert(daemon, base_path));
    RUN_TEST(test_invalid_reject(daemon, base_path));
    RUN_TEST(test_partial_prefix(daemon, base_path));
    RUN_TEST(test_idempotency(daemon, base_path));
    RUN_TEST(test_absolute_path(daemon, base_path));
    RUN_TEST(test_empty_input(daemon, base_path));
    RUN_TEST(test_concurrent_queries(daemon, base_path));
    RUN_TEST(test_lock_integrity());

    printf("\n========================================\n");
    printf("  Results: %d/%d tests passed\n", passed, total);
    printf("========================================\n");

    daemon_shutdown(daemon);

    if (passed != total) {
        printf("\n  OVERALL: FAIL\n\n");
    } else {
        printf("\n  OVERALL: PASS\n\n");
    }
}
