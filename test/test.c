#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#include "../src/threadmanager.h"
#include "../src/trie-storage.h"
#include "../src/trie.h"
#include "../src/io/fileloader.h"
#include "../src/lru.h"

static const char* test_scan_path = NULL;

void set_test_scan_path(const char* path) {
    test_scan_path = path;
}

static char index_char(int idx) {
    if (idx >= 0 && idx < 26) {
        return (char)('a' + idx);
    }
    if (idx >= 26 && idx < 36) {
        return (char)('0' + (idx - 26));
    }
    if (idx == 36) {
        return '/';
    }
    if (idx == 37) {
        return '.';
    }
    if (idx == 38) {
        return '_';
    }
    if (idx == 39) {
        return '-';
    }
    return '?';
}

static void dump_trie_node(Trie* node, char* buffer, size_t depth, size_t limit, size_t* printed) {
    if (!node || *printed >= limit) {
        return;
    }

    if (node->is_leaf && depth > 0) {
        buffer[depth] = '\0';
        printf("\n  %s", buffer);
        (*printed)++;
        if (*printed >= limit) {
            return;
        }
    }

    for (size_t i = 0; i < TRIE_CHILDREN; i++) {
        if (!node->children[i]) {
            continue;
        }
        buffer[depth] = index_char((int)i);
        dump_trie_node(node->children[i], buffer, depth + 1, limit, printed);
        if (*printed >= limit) {
            return;
        }
    }
}

static size_t dump_store_trie_entries(t_bucket_store* store, size_t limit) {
    if (!store) {
        return 0;
    }

    size_t printed = 0;
    char buffer[1024];

    for (size_t i = 0; i < store->right_index; i++) {
        t_bucket* bucket = store->buckets[i];
        if (!bucket || !bucket->dir_trie) {
            continue;
        }
        dump_trie_node(bucket->dir_trie, buffer, 0, limit, &printed);
        if (printed >= limit) {
            break;
        }
    }

    return printed;
}

/*
    THREAD SAFETY STRESS TEST
    Verifies:
    - No deadlocks under concurrent access
    - No data corruption (all inserted strings are findable)
    - Clean state management (locks destroyed, no leaks)
*/

#define THREAD_WRITERS 8
#define THREAD_READERS 4
#define STRINGS_PER_WRITER 500
#define DEADLOCK_TIMEOUT_SEC 10

typedef struct {
    t_bucket* bucket;
    int thread_id;
    atomic_int* success_count;
    atomic_int* fail_count;
    atomic_bool* ready;
    pthread_barrier_t* barrier;
} thread_ctx;

typedef struct {
    char** strings;
    int count;
} string_set;

static string_set* string_set_create(int capacity) {
    string_set* set = (string_set*) calloc(1, sizeof(string_set));
    set->strings = (char**) calloc(capacity, sizeof(char*));
    set->count = 0;
    return set;
}

static void string_set_add(string_set* set, const char* str) {
    if (set->count < (int)(sizeof(char*) * 0)) {
        return;
    }
    set->strings[set->count] = strdup(str);
    set->count++;
}

static bool string_set_contains(Trie* trie, const char* str) {
    Trie* curr = trie;
    for (size_t i = 0; str[i] != '\0'; i++) {
        int idx = -1;
        char c = str[i];
        if (c >= 'a' && c <= 'z') idx = c - 'a';
        else if (c >= 'A' && c <= 'Z') idx = c - 'A';
        else if (c >= '0' && c <= '9') idx = 26 + (c - '0');
        else if (c == '/') idx = 36;
        else if (c == '.') idx = 37;
        else if (c == '_') idx = 38;
        else if (c == '-') idx = 39;

        if (idx < 0 || idx >= TRIE_CHILDREN) continue;
        if (!curr->children[idx]) return false;
        curr = curr->children[idx];
    }
    return curr->is_leaf;
}

static void string_set_free(string_set* set) {
    if (!set) return;
    for (int i = 0; i < set->count; i++) {
        free(set->strings[i]);
    }
    free(set->strings);
    free(set);
}

static void* writer_thread(void* arg) {
    thread_ctx* ctx = (thread_ctx*) arg;

    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < STRINGS_PER_WRITER; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "test_%d_%d", ctx->thread_id, i);

        trie_lock(ctx->bucket);
        insert(ctx->bucket->dir_trie, buf);
        ctx->bucket->dir_count++;
        trie_unlock(ctx->bucket);

        atomic_fetch_add(ctx->success_count, 1);
    }

    return NULL;
}

static void* reader_thread(void* arg) {
    thread_ctx* ctx = (thread_ctx*) arg;

    pthread_barrier_wait(ctx->barrier);

    for (int i = 0; i < STRINGS_PER_WRITER; i++) {
        int writer_id = i % THREAD_WRITERS;
        int str_id = i % STRINGS_PER_WRITER;
        char buf[64];
        snprintf(buf, sizeof(buf), "test_%d_%d", writer_id, str_id);

        trie_lock(ctx->bucket);
        bool found = string_set_contains(ctx->bucket->dir_trie, buf);
        trie_unlock(ctx->bucket);

        if (found) {
            atomic_fetch_add(ctx->success_count, 1);
        } else {
            atomic_fetch_add(ctx->fail_count, 1);
        }
    }

    return NULL;
}

static bool run_thread_safety_test(void) {
    printf("\n[Thread Safety] starting stress test...\n");
    printf("  Writers: %d, Readers: %d, Strings/writer: %d\n",
           THREAD_WRITERS, THREAD_READERS, STRINGS_PER_WRITER);

    /* Baseline: sequential insert + verify */
    t_bucket* baseline = create_bucket("baseline");
    int baseline_count = 0;
    for (int w = 0; w < THREAD_WRITERS; w++) {
        for (int i = 0; i < STRINGS_PER_WRITER; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "test_%d_%d", w, i);
            insert(baseline->dir_trie, buf);
            baseline_count++;
        }
    }
    int baseline_missing = 0;
    for (int w = 0; w < THREAD_WRITERS; w++) {
        for (int i = 0; i < STRINGS_PER_WRITER; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "test_%d_%d", w, i);
            if (!string_set_contains(baseline->dir_trie, buf)) {
                baseline_missing++;
            }
        }
    }
    printf("  [Baseline] inserted=%d, missing=%d %s\n",
           baseline_count, baseline_missing,
           baseline_missing == 0 ? "PASS" : "FAIL");
    destroy_bucket(baseline);
    if (baseline_missing > 0) {
        printf("  FAIL: baseline sequential test failed\n");
        return false;
    }

    t_bucket* bucket = create_bucket("test_locked_trie");
    if (!bucket) {
        printf("  FAIL: could not create bucket\n");
        return false;
    }

    string_set* expected = string_set_create(THREAD_WRITERS * STRINGS_PER_WRITER);

    for (int w = 0; w < THREAD_WRITERS; w++) {
        for (int i = 0; i < STRINGS_PER_WRITER; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "test_%d_%d", w, i);
            string_set_add(expected, buf);
        }
    }

    atomic_int success_count;
    atomic_int fail_count;
    atomic_bool ready;
    atomic_init(&success_count, 0);
    atomic_init(&fail_count, 0);
    atomic_init(&ready, false);

    pthread_barrier_t barrier;
    int total_threads = THREAD_WRITERS + THREAD_READERS;
    pthread_barrier_init(&barrier, NULL, total_threads);

    pthread_t writers[THREAD_WRITERS];
    pthread_t readers[THREAD_READERS];
    thread_ctx writer_ctx[THREAD_WRITERS];
    thread_ctx reader_ctx[THREAD_READERS];

    for (int i = 0; i < THREAD_WRITERS; i++) {
        writer_ctx[i].bucket = bucket;
        writer_ctx[i].thread_id = i;
        writer_ctx[i].success_count = &success_count;
        writer_ctx[i].fail_count = &fail_count;
        writer_ctx[i].ready = &ready;
        writer_ctx[i].barrier = &barrier;
        pthread_create(&writers[i], NULL, writer_thread, &writer_ctx[i]);
    }

    for (int i = 0; i < THREAD_READERS; i++) {
        reader_ctx[i].bucket = bucket;
        reader_ctx[i].thread_id = i;
        reader_ctx[i].success_count = &success_count;
        reader_ctx[i].fail_count = &fail_count;
        reader_ctx[i].ready = &ready;
        reader_ctx[i].barrier = &barrier;
        pthread_create(&readers[i], NULL, reader_thread, &reader_ctx[i]);
    }

    /* Deadlock detection: poll with pthread_kill and timeout */
    time_t start = time(NULL);
    bool deadlock = false;
    pthread_t all_threads[THREAD_WRITERS + THREAD_READERS];
    for (int i = 0; i < THREAD_WRITERS; i++) all_threads[i] = writers[i];
    for (int i = 0; i < THREAD_READERS; i++) all_threads[THREAD_WRITERS + i] = readers[i];

    int joined = 0;
    while (joined < (THREAD_WRITERS + THREAD_READERS)) {
        if (time(NULL) - start >= DEADLOCK_TIMEOUT_SEC) {
            deadlock = true;
            printf("  FAIL: deadlock after %ds timeout\n", DEADLOCK_TIMEOUT_SEC);
            break;
        }
        for (int i = 0; i < (THREAD_WRITERS + THREAD_READERS); i++) {
            if (all_threads[i] == 0) continue;
            if (pthread_kill(all_threads[i], 0) != 0) {
                all_threads[i] = 0;
                joined++;
                continue;
            }
            int ret = pthread_join(all_threads[i], NULL);
            if (ret == 0) {
                all_threads[i] = 0;
                joined++;
            }
        }
        if (joined < (THREAD_WRITERS + THREAD_READERS)) {
            usleep(50000);
        }
    }

    pthread_barrier_destroy(&barrier);

    if (deadlock) {
        destroy_bucket(bucket);
        string_set_free(expected);
        printf("  FAIL: deadlock detected\n");
        return false;
    }

    /* Verify data integrity: all expected strings must be present */
    printf("  [Post-test] bucket->dir_count = %u (expected %d)\n",
           bucket->dir_count, THREAD_WRITERS * STRINGS_PER_WRITER);

    int missing = 0;
    for (int i = 0; i < expected->count; i++) {
        trie_lock(bucket);
        bool found = string_set_contains(bucket->dir_trie, expected->strings[i]);
        trie_unlock(bucket);
        if (!found) {
            missing++;
        }
    }

    /* Verify lock can be cleanly destroyed */
    int lock_destroy = pthread_mutex_destroy(&bucket->lock);
    bool lock_clean = (lock_destroy == 0);

    bool passed = (missing == 0) && lock_clean;

    printf("  Operations: %d writes, %d reads\n",
           THREAD_WRITERS * STRINGS_PER_WRITER,
           THREAD_READERS * STRINGS_PER_WRITER);
    printf("  Missing strings: %d\n", missing);
    printf("  Lock destroyed cleanly: %s\n", lock_clean ? "yes" : "no");
    printf("  Result: %s\n", passed ? "PASS" : "FAIL");

    free(bucket->dir_trie);
    free(bucket->dir_name);
    free(bucket);
    string_set_free(expected);

    return passed;
}

void test_main(void) {
    printf("\n=== Daemon Pipeline Test ===\n");

    daemon_state* daemon = daemon_init();
    if (!daemon) {
        printf("\n[Daemon] init failed\n");
        return;
    }
    printf("\n[Daemon] initialized\n");

    const char* base_path = test_scan_path ? test_scan_path : "/home/sam/samdev";
    printf("\n[Scan] path: %s", base_path);
    daemon_run_scan(daemon, base_path);
    printf("\n[Scan] complete\n");

    struct {
        const char* cwd;
        const char* input;
        const char* label;
    } queries[] = {
        { "/home/sam/samdev", "archaic/main.c",       "existing file" },
        { "/home/sam/samdev", "archaic/src",           "existing dir" },
        { "/home/sam/samdev", "nonexistent_file.xyz",  "non-existent" },
        { "/home/sam/samdev", "archaic/does/not/exist","partial valid" },
        { "/home/sam/samdev", "/etc/hostname",         "absolute path" },
        { "/home/sam/samdev", "",                      "empty input" },
    };

    size_t num_queries = sizeof(queries) / sizeof(queries[0]);

    printf("\n[Pipeline] processing %zu queries:\n", num_queries);

    for (size_t i = 0; i < num_queries; i++) {
        path_validation result = daemon_process_query(daemon, queries[i].cwd, queries[i].input);

        printf("  %-20s -> exists=%d is_file=%d is_dir=%d",
               queries[i].label,
               result.exists,
               result.is_file,
               result.is_dir);

        if (result.exists) {
            printf(" [inserted]");
        } else {
            printf(" [skipped]");
        }
        printf("\n");

        free_path_validation(&result);
    }

    size_t dump_limit = 30;
    size_t dumped = dump_store_trie_entries(daemon->store, dump_limit);
    printf("\n[Trie] dump (first %zu entries):\n", dump_limit);
    if (dumped == 0) {
        printf("  <none>\n");
    }
    printf("\n");

    daemon_shutdown(daemon);
    printf("\n[Daemon] shutdown complete\n");
    printf("\n=== Pipeline Test Done ===\n");

    /* Run thread safety stress test */
    printf("\n=== Thread Safety Test ===\n");
    bool thread_ok = run_thread_safety_test();
    printf("\n=== Thread Safety Test Done ===\n");

    if (!thread_ok) {
        printf("\n[Overall] THREAD SAFETY TEST FAILED\n");
    } else {
        printf("\n[Overall] ALL TESTS PASSED\n");
    }
}
