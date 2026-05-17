#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/io/fileloader.h"
#include "../src/lru.h"
#include "../src/portable-barrier.h"
#include "../src/threadmanager.h"
#include "../src/trie-storage.h"
#include "../src/trie.h"

#define PERF_ITERATIONS 100000
#define PERF_WARMUP 1000
#define PERF_THREAD_COUNT 8

typedef struct {
    double min_ms;
    double max_ms;
    double mean_ms;
    double p50_ms;
    double p95_ms;
    double p99_ms;
    double stddev_ms;
    double total_ms;
    long ops;
    double ops_per_sec;
} perf_metrics;

static double timespec_to_ms(struct timespec* ts) {
    return ts->tv_sec * 1000.0 + ts->tv_nsec / 1000000.0;
}

static int cmp_double(const void* a, const void* b) {
    double da = *(const double*) a;
    double db = *(const double*) b;
    return (da > db) - (da < db);
}

static perf_metrics compute_metrics(double* samples, long count, double total_ms) {
    perf_metrics m = {0};
    if (count == 0)
        return m;

    qsort(samples, count, sizeof(double), cmp_double);

    m.min_ms = samples[0];
    m.max_ms = samples[count - 1];
    m.ops = count;
    m.total_ms = total_ms;
    m.ops_per_sec = (total_ms > 0) ? (count / (total_ms / 1000.0)) : 0;

    double sum = 0;
    for (long i = 0; i < count; i++)
        sum += samples[i];
    m.mean_ms = sum / count;
    m.p50_ms = samples[(long) (count * 0.50)];
    m.p95_ms = samples[(long) (count * 0.95)];
    m.p99_ms = samples[(long) (count * 0.99)];

    double variance = 0;
    for (long i = 0; i < count; i++) {
        double diff = samples[i] - m.mean_ms;
        variance += diff * diff;
    }
    m.stddev_ms = sqrt(variance / count);

    return m;
}

static void print_metrics(const char* label, perf_metrics* m) {
    printf("\n  %s\n", label);
    printf("    Ops:          %ld\n", m->ops);
    printf("    Total time:   %.3f ms\n", m->total_ms);
    printf("    Throughput:   %.0f ops/sec\n", m->ops_per_sec);
    printf("    Min:          %.4f ms\n", m->min_ms);
    printf("    Mean:         %.4f ms\n", m->mean_ms);
    printf("    P50:          %.4f ms\n", m->p50_ms);
    printf("    P95:          %.4f ms\n", m->p95_ms);
    printf("    P99:          %.4f ms\n", m->p99_ms);
    printf("    Max:          %.4f ms\n", m->max_ms);
    printf("    StdDev:       %.4f ms\n", m->stddev_ms);
}

static char** generate_path_strings(long count, int max_depth, int name_len) {
    (void) name_len;
    char** strings = malloc(count * sizeof(char*));
    const char* dirs[] = {"src",   "lib",     "bin",    "doc",  "test",
                          "build", "include", "config", "data", "scripts"};
    const char* exts[] = {".c", ".h", ".txt", ".md", ".py", ".sh", ".json", ".toml", ".rs", ".go"};
    int nd = sizeof(dirs) / sizeof(dirs[0]);
    int ne = sizeof(exts) / sizeof(exts[0]);

    for (long i = 0; i < count; i++) {
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "/home/user/project");
        int depth = (i % max_depth) + 1;
        for (int d = 0; d < depth; d++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "/%s_%ld", dirs[(i + d) % nd], i);
        }
        if (i % 3 == 0) {
            snprintf(buf + pos, sizeof(buf) - pos, "/file_%ld%s", i, exts[i % ne]);
        } else {
            snprintf(buf + pos, sizeof(buf) - pos, "/dir_%ld", i);
        }
        strings[i] = strdup(buf);
    }
    return strings;
}

static char** generate_random_strings(long count, int len) {
    char** strings = malloc(count * sizeof(char*));
    const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789_./-";
    int cs = sizeof(charset) - 1;

    for (long i = 0; i < count; i++) {
        char* s = malloc(len + 1);
        for (int j = 0; j < len; j++) {
            s[j] = charset[rand() % cs];
        }
        s[len] = '\0';
        strings[i] = s;
    }
    return strings;
}

static void free_strings(char** strings, long count) {
    for (long i = 0; i < count; i++)
        free(strings[i]);
    free(strings);
}

/*
    PERF TEST 1: Trie insertion throughput
*/
static void perf_trie_insert(void) {
    printf("\n[Perf 1] Trie insertion throughput\n");

    Trie* root = create_trie();
    char** strings = generate_path_strings(PERF_ITERATIONS, 4, 32);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (long i = 0; i < PERF_ITERATIONS; i++) {
        insert(root, strings[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double total_ms = timespec_to_ms(&end) - timespec_to_ms(&start);

    perf_metrics m = {0};
    m.ops = PERF_ITERATIONS;
    m.total_ms = total_ms;
    m.ops_per_sec = (total_ms > 0) ? (PERF_ITERATIONS / (total_ms / 1000.0)) : 0;
    m.mean_ms = total_ms / PERF_ITERATIONS;
    m.min_ms = m.mean_ms;
    m.max_ms = m.mean_ms;
    m.p50_ms = m.mean_ms;
    m.p95_ms = m.mean_ms;
    m.p99_ms = m.mean_ms;
    m.stddev_ms = 0;

    print_metrics("Bulk insert (aggregate)", &m);

    size_t node_count = 0;
    size_t trie_mem = 0;
    {
        Trie** stack = malloc(5000000 * sizeof(Trie*));
        int top = 0;
        stack[top++] = root;
        while (top > 0) {
            Trie* n = stack[--top];
            node_count++;
            trie_mem += sizeof(Trie);
            for (uint8_t i = 0; i < n->child_count; i++) {
                if (n->children[i].node && top < 5000000) {
                    stack[top++] = n->children[i].node;
                }
            }
        }
        free(stack);
    }
    printf("    Nodes:        %zu\n", node_count);
    printf("    Trie memory:  %.2f MB\n", trie_mem / (1024.0 * 1024.0));
    printf("    Bytes/node:   %.1f\n", node_count > 0 ? (double) trie_mem / node_count : 0);
    printf("    Bytes/string: %.1f\n", node_count > 0 ? (double) trie_mem / PERF_ITERATIONS : 0);

    trie_free_recursive(root);
    free_strings(strings, PERF_ITERATIONS);
}

/*
    PERF TEST 2: Trie search latency (hit and miss)
*/
static void perf_trie_search(void) {
    printf("\n[Perf 2] Trie search latency\n");

    Trie* root = create_trie();
    char** strings = generate_path_strings(PERF_ITERATIONS, 4, 32);

    for (long i = 0; i < PERF_ITERATIONS; i++) {
        insert(root, strings[i]);
    }

    double* hit_samples = malloc(PERF_ITERATIONS * sizeof(double));
    double* miss_samples = malloc(PERF_ITERATIONS * sizeof(double));

    struct timespec t0, t1;

    for (long i = 0; i < PERF_ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        search(root, NULL, strings[i]);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        hit_samples[i] = timespec_to_ms(&t1) - timespec_to_ms(&t0);
    }

    char* miss_str = "/nonexistent/path/that/does/not/exist/xyz";
    for (long i = 0; i < PERF_ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        search(root, NULL, miss_str);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        miss_samples[i] = timespec_to_ms(&t1) - timespec_to_ms(&t0);
    }

    double hit_total = 0, miss_total = 0;
    for (long i = 0; i < PERF_ITERATIONS; i++) {
        hit_total += hit_samples[i];
        miss_total += miss_samples[i];
    }

    perf_metrics hit_m = compute_metrics(hit_samples, PERF_ITERATIONS, hit_total);
    perf_metrics miss_m = compute_metrics(miss_samples, PERF_ITERATIONS, miss_total);

    print_metrics("Search HIT", &hit_m);
    print_metrics("Search MISS", &miss_m);

    free(hit_samples);
    free(miss_samples);
    free_strings(strings, PERF_ITERATIONS);
    trie_free_recursive(root);
}

/*
    PERF TEST 3: Completion collection latency
*/
static void perf_completions(void) {
    printf("\n[Perf 3] Completion collection latency\n");

    Trie* root = create_trie();

    const char* prefixes[] = {"/home/user/project/src", "/home/user/project/lib",
                              "/home/user/project"};
    int np = sizeof(prefixes) / sizeof(prefixes[0]);

    for (int p = 0; p < np; p++) {
        char buf[512];
        for (int i = 0; i < 10000; i++) {
            snprintf(buf, sizeof(buf), "%s/file_%d.c", prefixes[p], i);
            insert(root, buf);
            snprintf(buf, sizeof(buf), "%s/file_%d.h", prefixes[p], i);
            insert(root, buf);
        }
    }

    for (int p = 0; p < np; p++) {
        double* samples = malloc(PERF_ITERATIONS * sizeof(double));
        struct timespec t0, t1;

        for (long i = 0; i < PERF_ITERATIONS; i++) {
            completions* c = completions_create(50);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            completions_collect(root, prefixes[p], c);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            samples[i] = timespec_to_ms(&t1) - timespec_to_ms(&t0);
            completions_free(c);
        }

        double total = 0;
        for (long i = 0; i < PERF_ITERATIONS; i++)
            total += samples[i];

        completions* probe = completions_create(50);
        completions_collect(root, prefixes[p], probe);
        size_t result_count = probe->count;
        completions_free(probe);

        perf_metrics m = compute_metrics(samples, PERF_ITERATIONS, total);

        char label[256];
        snprintf(label, sizeof(label), "Collect prefix='%s' (results=%zu)", prefixes[p],
                 result_count);
        print_metrics(label, &m);

        free(samples);
    }

    trie_free_recursive(root);
}

/*
    PERF TEST 4: Bucket find/insert latency
*/
static void perf_bucket_ops(void) {
    printf("\n[Perf 4] Bucket store operations\n");

    t_bucket_store* store = calloc(1, sizeof(t_bucket_store));
    pthread_mutex_init(&store->store_lock, NULL);
    struct node* parent = calloc(1, sizeof(struct node));
    parent->is_parent = true;
    store->parent = parent;

    char** paths = generate_path_strings(PERF_ITERATIONS, 3, 32);

    double* find_samples = malloc(PERF_ITERATIONS * sizeof(double));
    double* insert_samples = malloc(PERF_ITERATIONS * sizeof(double));
    struct timespec t0, t1;

    long inserts = 0;
    for (long i = 0; i < PERF_ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        t_bucket* b = find_bucket(store, paths[i], paths[i], 3, false);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        if (b && b->dir_trie) {
            insert(b->dir_trie, paths[i]);
            b->dir_count++;
        }

        double elapsed = timespec_to_ms(&t1) - timespec_to_ms(&t0);
        if (b && b->dir_count == 1) {
            insert_samples[inserts] = elapsed;
            inserts++;
        }
        find_samples[i] = elapsed;
    }

    double find_total = 0, insert_total = 0;
    for (long i = 0; i < PERF_ITERATIONS; i++)
        find_total += find_samples[i];
    for (long i = 0; i < inserts; i++)
        insert_total += insert_samples[i];

    perf_metrics find_m = compute_metrics(find_samples, PERF_ITERATIONS, find_total);
    perf_metrics insert_m = compute_metrics(insert_samples, inserts, insert_total);

    print_metrics("find_bucket (all)", &find_m);
    print_metrics("insert_bucket (new only)", &insert_m);
    printf("    Active buckets: %zu\n", store->right_index);

    for (size_t i = 0; i < store->right_index; i++) {
        if (store->buckets[i])
            destroy_bucket(store->buckets[i]);
    }
    pthread_mutex_destroy(&store->store_lock);
    free(parent);
    free(store);
    free(find_samples);
    free(insert_samples);
    free_strings(paths, PERF_ITERATIONS);
}

/*
    PERF TEST 5: LRU operations
*/
static void perf_lru_ops(void) {
    printf("\n[Perf 5] LRU operations\n");

    t_bucket_store* store = calloc(1, sizeof(t_bucket_store));
    pthread_mutex_init(&store->store_lock, NULL);
    struct node* parent = calloc(1, sizeof(struct node));
    parent->is_parent = true;
    store->parent = parent;

    t_bucket* buckets[BUCKETS];
    for (int i = 0; i < BUCKETS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/bucket_%d", i);
        buckets[i] = create_bucket(name);
        buckets[i]->id = i;
        buckets[i]->array_index = i;
        store->buckets[i] = buckets[i];
        store->right_index++;
        create_or_to_back(store, buckets[i]);
    }

    double* to_front_samples = malloc(PERF_ITERATIONS * sizeof(double));
    double* move_samples = malloc(PERF_ITERATIONS * sizeof(double));
    struct timespec t0, t1;

    for (long i = 0; i < PERF_ITERATIONS; i++) {
        t_bucket* b = buckets[i % BUCKETS];
        clock_gettime(CLOCK_MONOTONIC, &t0);
        move_to_front(store, b);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        move_samples[i] = timespec_to_ms(&t1) - timespec_to_ms(&t0);
    }

    for (long i = 0; i < PERF_ITERATIONS; i++) {
        t_bucket* b = buckets[i % BUCKETS];
        clock_gettime(CLOCK_MONOTONIC, &t0);
        create_or_to_front(store, b);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        to_front_samples[i] = timespec_to_ms(&t1) - timespec_to_ms(&t0);
    }

    double move_total = 0, to_front_total = 0;
    for (long i = 0; i < PERF_ITERATIONS; i++) {
        move_total += move_samples[i];
        to_front_total += to_front_samples[i];
    }

    perf_metrics move_m = compute_metrics(move_samples, PERF_ITERATIONS, move_total);
    perf_metrics to_front_m = compute_metrics(to_front_samples, PERF_ITERATIONS, to_front_total);

    print_metrics("move_to_front", &move_m);
    print_metrics("create_or_to_front", &to_front_m);

    for (int i = 0; i < BUCKETS; i++)
        destroy_bucket(buckets[i]);
    pthread_mutex_destroy(&store->store_lock);
    free(parent);
    free(store);
    free(move_samples);
    free(to_front_samples);
}

/*
    PERF TEST 6: Concurrent insert scaling
*/
typedef struct {
    Trie* root;
    char** strings;
    long count;
    int thread_id;
    atomic_long* total_ops;
    pthread_barrier_t* barrier;
    double* samples;
    pthread_mutex_t* lock;
} perf_thread_ctx;

static void* perf_insert_worker(void* arg) {
    perf_thread_ctx* ctx = (perf_thread_ctx*) arg;
    pthread_barrier_wait(ctx->barrier);

    struct timespec t0, t1;
    for (long i = 0; i < ctx->count; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pthread_mutex_lock(ctx->lock);
        insert(ctx->root, ctx->strings[i]);
        pthread_mutex_unlock(ctx->lock);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ctx->samples[i] = timespec_to_ms(&t1) - timespec_to_ms(&t0);
        atomic_fetch_add(ctx->total_ops, 1);
    }
    return NULL;
}

static void perf_concurrent_insert(void) {
    printf("\n[Perf 6] Concurrent insert scaling\n");

    int thread_counts[] = {1, 2, 4, 8};
    int nt = sizeof(thread_counts) / sizeof(thread_counts[0]);

    for (int ti = 0; ti < nt; ti++) {
        int nthreads = thread_counts[ti];
        long per_thread = PERF_ITERATIONS / nthreads;

        Trie* root = create_trie();
        char** strings = generate_path_strings(PERF_ITERATIONS, 4, 32);

        pthread_mutex_t lock;
        pthread_mutex_init(&lock, NULL);

        pthread_barrier_t barrier;
        pthread_barrier_init(&barrier, NULL, nthreads);
        atomic_long total_ops;
        atomic_init(&total_ops, 0);

        pthread_t* threads = malloc(nthreads * sizeof(pthread_t));
        perf_thread_ctx* ctxs = malloc(nthreads * sizeof(perf_thread_ctx));
        double** all_samples = malloc(nthreads * sizeof(double*));

        for (int t = 0; t < nthreads; t++) {
            all_samples[t] = malloc(per_thread * sizeof(double));
            ctxs[t].root = root;
            ctxs[t].strings = strings + t * per_thread;
            ctxs[t].count = per_thread;
            ctxs[t].thread_id = t;
            ctxs[t].total_ops = &total_ops;
            ctxs[t].barrier = &barrier;
            ctxs[t].samples = all_samples[t];
            ctxs[t].lock = &lock;
            pthread_create(&threads[t], NULL, perf_insert_worker, &ctxs[t]);
        }

        struct timespec wall_start, wall_end;
        clock_gettime(CLOCK_MONOTONIC, &wall_start);

        for (int t = 0; t < nthreads; t++) {
            pthread_join(threads[t], NULL);
        }

        clock_gettime(CLOCK_MONOTONIC, &wall_end);
        double wall_ms = timespec_to_ms(&wall_end) - timespec_to_ms(&wall_start);

        double* merged = malloc(PERF_ITERATIONS * sizeof(double));
        long merged_count = 0;
        for (int t = 0; t < nthreads; t++) {
            for (long i = 0; i < per_thread; i++) {
                merged[merged_count++] = all_samples[t][i];
            }
        }

        perf_metrics m = compute_metrics(merged, merged_count, wall_ms);

        char label[128];
        snprintf(label, sizeof(label), "Concurrent insert (%d threads)", nthreads);
        print_metrics(label, &m);

        for (int t = 0; t < nthreads; t++)
            free(all_samples[t]);
        free(all_samples);
        free(ctxs);
        free(threads);
        free(merged);
        free(strings);
        trie_free_recursive(root);
        pthread_mutex_destroy(&lock);
        pthread_barrier_destroy(&barrier);
    }
}

/*
    PERF TEST 7: Memory overhead analysis
*/
static void perf_memory(void) {
    printf("\n[Perf 7] Memory overhead analysis\n");

    printf("    sizeof(RadixNode):    %zu bytes\n", sizeof(Trie));
    printf("    sizeof(t_bucket):     %zu bytes\n", sizeof(t_bucket));
    printf("    sizeof(t_bucket_store): %zu bytes\n", sizeof(t_bucket_store));
    printf("    sizeof(node):         %zu bytes\n", sizeof(struct node));
    printf("    sizeof(completions):  %zu bytes\n", sizeof(completions));
    printf("    BUCKETS:              %d\n", BUCKETS);
    printf("    RADIX_INLINE:         %d\n", RADIX_INLINE_CHILDREN);

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    long baseline_kb = ru.ru_maxrss;

    Trie* root = create_trie();
    char** strings = generate_path_strings(50000, 4, 32);
    for (long i = 0; i < 50000; i++) {
        insert(root, strings[i]);
    }

    t_bucket_store* store = calloc(1, sizeof(t_bucket_store));
    pthread_mutex_init(&store->store_lock, NULL);
    struct node* parent = calloc(1, sizeof(struct node));
    parent->is_parent = true;
    store->parent = parent;

    for (long i = 0; i < 200; i++) {
        char name[128];
        snprintf(name, sizeof(name), "/path/to/directory_%ld", i);
        t_bucket* b = create_bucket(name);
        b->id = i;
        b->array_index = i;
        store->buckets[i] = b;
        store->right_index++;
        for (int j = 0; j < 100; j++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/file_%d.txt", name, j);
            insert(b->dir_trie, path);
            b->dir_count++;
        }
        create_or_to_back(store, b);
    }

    getrusage(RUSAGE_SELF, &ru);
    long after_kb = ru.ru_maxrss;

    size_t trie_nodes = 0;
    {
        Trie** stack = malloc(5000000 * sizeof(Trie*));
        int top = 0;
        stack[top++] = root;
        while (top > 0) {
            Trie* n = stack[--top];
            trie_nodes++;
            for (uint8_t i = 0; i < n->child_count; i++) {
                if (n->children[i].node && top < 5000000) {
                    stack[top++] = n->children[i].node;
                }
            }
        }
        free(stack);
    }

    size_t bucket_trie_nodes = 0;
    for (size_t i = 0; i < store->right_index; i++) {
        if (!store->buckets[i])
            continue;
        Trie** stack = malloc(500000 * sizeof(Trie*));
        int top = 0;
        stack[top++] = store->buckets[i]->dir_trie;
        while (top > 0) {
            Trie* n = stack[--top];
            bucket_trie_nodes++;
            for (uint8_t j = 0; j < n->child_count; j++) {
                if (n->children[j].node && top < 500000) {
                    stack[top++] = n->children[j].node;
                }
            }
        }
        free(stack);
    }

    printf("\n    RSS baseline:         %ld KB\n", baseline_kb);
    printf("    RSS after load:       %ld KB\n", after_kb);
    printf("    RSS delta:            %ld KB (%.2f MB)\n", after_kb - baseline_kb,
           (after_kb - baseline_kb) / 1024.0);
    printf("    Trie nodes (50k):     %zu\n", trie_nodes);
    printf("    Bucket trie nodes:    %zu (200 buckets x 100 files)\n", bucket_trie_nodes);
    printf("    Bucket overhead:      %zu bytes (200 buckets)\n", 200 * sizeof(t_bucket));
    printf("    Store array:          %zu bytes (%d pointers)\n", BUCKETS * sizeof(t_bucket*),
           BUCKETS);
    printf("    LRU nodes:            %zu bytes (200 nodes)\n", 200 * sizeof(struct node));

    trie_free_recursive(root);
    for (size_t i = 0; i < store->right_index; i++) {
        if (store->buckets[i])
            destroy_bucket(store->buckets[i]);
    }
    pthread_mutex_destroy(&store->store_lock);
    free(parent);
    free(store);
    free_strings(strings, 50000);
}

/*
    PERF TEST 8: String length scaling
*/
static void perf_string_length_scaling(void) {
    printf("\n[Perf 8] String length scaling\n");

    int lengths[] = {16, 32, 64, 128, 256, 512};
    int nl = sizeof(lengths) / sizeof(lengths[0]);
    long per_len = 20000;

    for (int li = 0; li < nl; li++) {
        int len = lengths[li];
        Trie* root = create_trie();
        char** strings = generate_random_strings(per_len, len);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (long i = 0; i < per_len; i++) {
            insert(root, strings[i]);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double insert_ms = timespec_to_ms(&t1) - timespec_to_ms(&t0);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (long i = 0; i < per_len; i++) {
            search(root, NULL, strings[i]);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double search_ms = timespec_to_ms(&t1) - timespec_to_ms(&t0);

        size_t node_count = 0;
        {
            Trie** stack = malloc(5000000 * sizeof(Trie*));
            int top = 0;
            stack[top++] = root;
            while (top > 0) {
                Trie* n = stack[--top];
                node_count++;
                for (uint8_t i = 0; i < n->child_count; i++) {
                    if (n->children[i].node && top < 5000000) {
                        stack[top++] = n->children[i].node;
                    }
                }
            }
            free(stack);
        }

        printf("    len=%-4d insert=%.1fms (%.0f ops/s) search=%.1fms (%.0f ops/s) nodes=%zu "
               "mem=%.2fMB\n",
               len, insert_ms, per_len / (insert_ms / 1000.0), search_ms,
               per_len / (search_ms / 1000.0), node_count,
               node_count * sizeof(Trie) / (1024.0 * 1024.0));

        trie_free_recursive(root);
        free_strings(strings, per_len);
    }
}

/*
    PERF TEST 9: Daemon end-to-end throughput
*/
static void perf_daemon_e2e(const char* scan_path) {
    printf("\n[Perf 9] Daemon end-to-end throughput\n");

    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    daemon_state* daemon = daemon_init();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double init_ms = timespec_to_ms(&t1) - timespec_to_ms(&t0);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    daemon_run_scan(daemon, scan_path);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double scan_ms = timespec_to_ms(&t1) - timespec_to_ms(&t0);

    size_t total_buckets = daemon->store->right_index;
    size_t total_entries = 0;
    for (size_t i = 0; i < total_buckets; i++) {
        if (daemon->store->buckets[i]) {
            total_entries += daemon->store->buckets[i]->dir_count;
        }
    }

    printf("    Init time:          %.3f ms\n", init_ms);
    printf("    Scan time:          %.3f ms\n", scan_ms);
    printf("    Active buckets:     %zu\n", total_buckets);
    printf("    Total entries:      %zu\n", total_entries);
    printf("    Scan throughput:    %.0f entries/sec\n",
           scan_ms > 0 ? (total_entries / (scan_ms / 1000.0)) : 0);

    double* query_samples = malloc(PERF_ITERATIONS * sizeof(double));
    double* completions_samples = malloc(PERF_ITERATIONS * sizeof(double));

    const char* prefixes[] = {scan_path, ""};
    int np = sizeof(prefixes) / sizeof(prefixes[0]);

    for (int p = 0; p < np; p++) {
        double total_q = 0, total_c = 0;
        for (long i = 0; i < PERF_ITERATIONS; i++) {
            struct timespec qt0, qt1;
            clock_gettime(CLOCK_MONOTONIC, &qt0);
            completions* c = daemon_get_completions(daemon, prefixes[p], 50);
            clock_gettime(CLOCK_MONOTONIC, &qt1);
            double elapsed = timespec_to_ms(&qt1) - timespec_to_ms(&qt0);
            completions_samples[i] = elapsed;
            total_c += elapsed;
            if (c)
                completions_free(c);

            clock_gettime(CLOCK_MONOTONIC, &qt0);
            path_validation v = daemon_process_query(daemon, scan_path, "nonexistent_xyz");
            free_path_validation(&v);
            clock_gettime(CLOCK_MONOTONIC, &qt1);
            query_samples[i] = timespec_to_ms(&qt1) - timespec_to_ms(&qt0);
            total_q += query_samples[i];
        }

        perf_metrics qm = compute_metrics(query_samples, PERF_ITERATIONS, total_q);
        perf_metrics cm = compute_metrics(completions_samples, PERF_ITERATIONS, total_c);

        char qlabel[256], clabel[256];
        snprintf(qlabel, sizeof(qlabel), "Query (prefix='%s')", prefixes[p]);
        snprintf(clabel, sizeof(clabel), "Completions (prefix='%s')", prefixes[p]);
        print_metrics(qlabel, &qm);
        print_metrics(clabel, &cm);
    }

    free(query_samples);
    free(completions_samples);
    daemon_shutdown(daemon);
}

void perf_main(const char* scan_path) {
    printf("\n========================================");
    printf("\n  Performance Benchmark Suite");
    printf("\n========================================");
    printf("\n  Iterations per test: %d", PERF_ITERATIONS);
    printf("\n  Radix inline:        %d", RADIX_INLINE_CHILDREN);
    printf("\n  Max buckets:         %d", BUCKETS);
    printf("\n  Node size:           %zu bytes", sizeof(Trie));
    printf("\n========================================\n");

    perf_trie_insert();
    perf_trie_search();
    perf_completions();
    perf_bucket_ops();
    perf_lru_ops();
    perf_concurrent_insert();
    perf_memory();
    perf_string_length_scaling();
    perf_daemon_e2e(scan_path);

    printf("\n========================================\n");
    printf("  Performance benchmarks complete\n");
    printf("========================================\n\n");
}
