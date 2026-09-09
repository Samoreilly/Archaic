#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/trie.h"

#define N_PATHS 50000
#define RESULT_CAP 50
#define INSERT_PATHS_SMALL 10000

typedef struct {
    const char* name;
    int iters;
    double total_ms;
    double ops_per_sec;
    double ns_per_op;
    double p50_us;
    double p95_us;
    double p99_us;
    double min_us;
    double max_us;
} bench_row;

static bench_row g_rows[64];
static int g_nrow = 0;

static double ns_since(struct timespec* a, struct timespec* b) {
    return (b->tv_sec - a->tv_sec) * 1e9 + (b->tv_nsec - a->tv_nsec);
}

static int cmp_dbl(const void* a, const void* b) {
    double da = *(const double*) a;
    double db = *(const double*) b;
    return (da > db) - (da < db);
}

static void record_batch(const char* name, int iters, double total_ns) {
    bench_row* r = &g_rows[g_nrow++];
    r->name = name;
    r->iters = iters;
    r->total_ms = total_ns / 1e6;
    r->ops_per_sec = (total_ns > 0) ? (iters / (total_ns / 1e9)) : 0;
    r->ns_per_op = (iters > 0) ? (total_ns / iters) : 0;
    r->p50_us = r->ns_per_op / 1e3;
    r->p95_us = r->p50_us;
    r->p99_us = r->p50_us;
    r->min_us = r->p50_us;
    r->max_us = r->p50_us;
}

static void record_samples(const char* name, double* samples_ns, int n) {
    double total = 0;
    for (int i = 0; i < n; i++)
        total += samples_ns[i];
    qsort(samples_ns, (size_t) n, sizeof(double), cmp_dbl);
    bench_row* r = &g_rows[g_nrow++];
    r->name = name;
    r->iters = n;
    r->total_ms = total / 1e6;
    r->ops_per_sec = (total > 0) ? (n / (total / 1e9)) : 0;
    r->ns_per_op = (n > 0) ? (total / n) : 0;
    r->min_us = samples_ns[0] / 1e3;
    r->max_us = samples_ns[n - 1] / 1e3;
    r->p50_us = samples_ns[n * 50 / 100] / 1e3;
    r->p95_us = samples_ns[n * 95 / 100] / 1e3;
    r->p99_us = samples_ns[n * 99 / 100] / 1e3;
}

static char (*g_paths)[256];

static void make_paths(int n) {
    static const char* roots[] = {
        "/home/user/proj/src",
        "/home/user/proj/include",
        "/home/user/proj/tests",
        "/home/user/proj/scripts",
        "/usr/local/share/doc",
        "/opt/app/lib",
    };
    static const char* exts[] = {".c", ".h", ".rs", ".py", ".js", ".md", ".toml", ".sh"};
    for (int i = 0; i < n; i++) {
        const char* root = roots[i % 6];
        const char* ext = exts[i % 8];
        int mod = i / 40;
        snprintf(g_paths[i], 256, "%s/mod_%03d/file_%05d%s", root, mod, i, ext);
    }
}

static Trie* build_trie(int n) {
    Trie* root = create_trie();
    for (int i = 0; i < n; i++)
        insert(root, g_paths[i]);
    return root;
}

static void free_fuzzy(char** paths, int n) {
    for (int i = 0; i < n; i++)
        free(paths[i]);
}

static void bench_insert(int n) {
    Trie* root = create_trie();
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < n; i++)
        insert(root, g_paths[i]);
    clock_gettime(CLOCK_MONOTONIC, &b);
    record_batch(n == INSERT_PATHS_SMALL ? "insert_10k" : "insert_50k", n, ns_since(&a, &b));
    fprintf(stderr, "  nodes after insert_%d: %zu\n", n, trie_node_count(root));
    trie_free_recursive(root);
}

static void bench_prefix(Trie* root, const char* prefix, const char* name, int iters) {
    completions* c = completions_create(RESULT_CAP);
    double* s = calloc((size_t) iters, sizeof(double));
    struct timespec a, b;
    for (int i = 0; i < iters; i++) {
        c->count = 0;
        clock_gettime(CLOCK_MONOTONIC, &a);
        completions_collect(root, prefix, c);
        clock_gettime(CLOCK_MONOTONIC, &b);
        s[i] = ns_since(&a, &b);
        for (size_t k = 0; k < c->count; k++) {
            free(c->paths[k]);
            c->paths[k] = NULL;
        }
        c->count = 0;
    }
    record_samples(name, s, iters);
    completions_free(c);
    free(s);
}

static void bench_scored(Trie* root, const char* prefix, const char* cwd, const char* name,
                         int iters) {
    uint64_t now = 1700000000ULL;
    double* s = calloc((size_t) iters, sizeof(double));
    struct timespec a, b;
    size_t last_count = 0;
    for (int i = 0; i < iters; i++) {
        scored_completions* sc = scored_completions_create(RESULT_CAP);
        clock_gettime(CLOCK_MONOTONIC, &a);
        scored_completions_collect(root, prefix, sc, now, cwd, NULL, 0.50, 0);
        clock_gettime(CLOCK_MONOTONIC, &b);
        s[i] = ns_since(&a, &b);
        last_count = sc->count;
        scored_completions_free(sc);
    }
    fprintf(stderr, "  %s last_count=%zu\n", name, last_count);
    record_samples(name, s, iters);
    free(s);
}

static void bench_fuzzy(Trie* root, const char* query, const char* name, int iters) {
    char* paths[RESULT_CAP];
    bool dirs[RESULT_CAP];
    double* s = calloc((size_t) iters, sizeof(double));
    struct timespec a, b;
    int last_n = 0;
    for (int i = 0; i < iters; i++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        last_n = trie_fuzzy_collect(root, query, paths, dirs, RESULT_CAP);
        clock_gettime(CLOCK_MONOTONIC, &b);
        s[i] = ns_since(&a, &b);
        free_fuzzy(paths, last_n);
    }
    fprintf(stderr, "  %s last_n=%d\n", name, last_n);
    record_samples(name, s, iters);
    free(s);
}

static void bench_search(Trie* root, int iters) {
    double* s = calloc((size_t) iters, sizeof(double));
    struct timespec a, b;
    for (int i = 0; i < iters; i++) {
        char* p = g_paths[i % N_PATHS];
        clock_gettime(CLOCK_MONOTONIC, &a);
        (void) search(root, NULL, p);
        clock_gettime(CLOCK_MONOTONIC, &b);
        s[i] = ns_since(&a, &b);
    }
    record_samples("search_hit", s, iters);
    for (int i = 0; i < iters; i++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        (void) search(root, NULL, "/no/such/path/missing.c");
        clock_gettime(CLOCK_MONOTONIC, &b);
        s[i] = ns_since(&a, &b);
    }
    record_samples("search_miss", s, iters);
    free(s);
}

static void print_table(void) {
    printf("\n");
    printf("%-22s %8s %12s %12s %12s %10s %10s %10s\n", "benchmark", "iters", "total_ms",
           "ops/sec", "ns/op", "p50_us", "p95_us", "p99_us");
    printf("%-22s %8s %12s %12s %12s %10s %10s %10s\n", "----------------------", "--------",
           "------------", "------------", "------------", "----------", "----------",
           "----------");
    for (int i = 0; i < g_nrow; i++) {
        bench_row* r = &g_rows[i];
        printf("%-22s %8d %12.3f %12.0f %12.0f %10.2f %10.2f %10.2f\n", r->name, r->iters,
               r->total_ms, r->ops_per_sec, r->ns_per_op, r->p50_us, r->p95_us, r->p99_us);
    }
    printf("\ncsv\n");
    printf("benchmark,iters,total_ms,ops_per_sec,ns_per_op,p50_us,p95_us,p99_us,min_us,max_us\n");
    for (int i = 0; i < g_nrow; i++) {
        bench_row* r = &g_rows[i];
        printf("%s,%d,%.4f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f\n", r->name, r->iters, r->total_ms,
               r->ops_per_sec, r->ns_per_op, r->p50_us, r->p95_us, r->p99_us, r->min_us, r->max_us);
    }
}

int main(void) {
    g_paths = calloc(N_PATHS, sizeof(*g_paths));
    if (!g_paths)
        return 1;
    make_paths(N_PATHS);

    fprintf(stderr, "archaic search bench  paths=%d\n", N_PATHS);
    fprintf(stderr, "[1/7] insert\n");
    bench_insert(INSERT_PATHS_SMALL);
    bench_insert(N_PATHS);

    fprintf(stderr, "[2/7] build corpus\n");
    Trie* root = build_trie(N_PATHS);

    fprintf(stderr, "[3/7] prefix collect\n");
    bench_prefix(root, "/home/user/proj/src/mod_001/", "prefix_narrow", 400);
    bench_prefix(root, "/home/user/proj/src/", "prefix_medium", 200);
    bench_prefix(root, "/home/", "prefix_wide", 100);

    fprintf(stderr, "[4/7] scored collect (may be slow on baseline)\n");
    bench_scored(root, "/home/user/proj/src/mod_001/", "/home/user/proj", "scored_narrow", 80);
    bench_scored(root, "/home/user/proj/src/", "/home/user/proj", "scored_medium", 20);
    bench_scored(root, "/home/", "/home/user/proj", "scored_wide", 8);

    fprintf(stderr, "[5/7] fuzzy\n");
    bench_fuzzy(root, "file_00010", "fuzzy_exactish", 40);
    bench_fuzzy(root, "srcmod", "fuzzy_acronym", 20);
    bench_fuzzy(root, "fiel", "fuzzy_typo", 20);
    bench_fuzzy(root, "xyzzyplugh", "fuzzy_nomatch", 20);

    fprintf(stderr, "[6/7] exact search\n");
    bench_search(root, 2000);

    fprintf(stderr, "[7/7] done\n");
    print_table();

    trie_free_recursive(root);
    free(g_paths);
    return 0;
}
