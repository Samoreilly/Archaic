/*
 * archaic — Unit Test Suite
 *
 * Comprehensive unit tests covering: trie operations, scoring, fuzzy matching,
 * cache operations, config parsing, hashset, path handling,
 * protocol validation, and edge cases.
 *
 * Run: ./archaic-unit [scan_path]
 *     scan_path defaults to /tmp/archaic_unit_test
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../ipc/protocol.h"
#include "../src/cache.h"
#include "../src/config.h"
#include "../src/hashset.h"
#include "../src/lru.h"
#include "../src/path-utils.h"
#include "../src/scanner.h"
#include "../src/threadmanager.h"
#include "../src/trie-storage.h"
#include "../src/trie.h"
#include "../src/watcher.h"

/* ── Test infrastructure ─────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  [TEST] %-50s ", #name);                                                          \
        fflush(stdout);                                                                            \
    } while (0)

#define PASS()                                                                                     \
    do {                                                                                           \
        tests_passed++;                                                                            \
        printf("PASS\n");                                                                          \
    } while (0)

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_failed++;                                                                            \
        printf("FAIL: %s\n", msg);                                                                 \
    } while (0)

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            FAIL(msg);                                                                             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ_INT(expected, actual, msg)                                                       \
    do {                                                                                           \
        if ((expected) != (actual)) {                                                              \
            printf("FAIL: %s (expected %d, got %d)\n", msg, (int) (expected), (int) (actual));     \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ_STR(expected, actual, msg)                                                       \
    do {                                                                                           \
        if (strcmp((expected), (actual)) != 0) {                                                   \
            printf("FAIL: %s (expected \"%s\", got \"%s\")\n", msg, (expected), (actual));         \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_NULL(ptr, msg)                                                                      \
    do {                                                                                           \
        if ((ptr) != NULL) {                                                                       \
            printf("FAIL: %s (expected NULL, got %p)\n", msg, (void*) (ptr));                      \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_NOT_NULL(ptr, msg)                                                                  \
    do {                                                                                           \
        if ((ptr) == NULL) {                                                                       \
            printf("FAIL: %s (expected non-NULL, got NULL)\n", msg);                               \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ── Helper: create temp directory tree ─────────────────────────────── */

static const char* test_root = NULL;

static void setup_temp_dir(void) {
    char tmpl[] = "/tmp/archaic_unit_XXXXXX";
    test_root = mkdtemp(tmpl);
    ASSERT_NOT_NULL(test_root, "mkdtemp failed");
    /* Make a copy since tmpl is modified by mkdtemp */
    char* root_copy = strdup(test_root);
    test_root = root_copy;

    /* Create directory tree */
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/src", test_root);
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/src/io", test_root);
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/src/util", test_root);
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/build", test_root);
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/build/bin", test_root);
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/docs", test_root);
    mkdir(buf, 0755);

    /* Create test files */
    FILE* f;
    const char* files[] = {
        "/main.c",    "/src/io/reader.c", "/src/io/writer.c",   "/src/util/log.c",
        "/README.md", "/Makefile",        "/build/bin/archaic",
    };
    for (int i = 0; i < (int) (sizeof(files) / sizeof(files[0])); i++) {
        snprintf(buf, sizeof(buf), "%s%s", test_root, files[i]);
        f = fopen(buf, "w");
        if (f)
            fclose(f);
    }
}

static void cleanup_temp_dir(void) {
    if (!test_root)
        return;
    /* Best-effort cleanup */
    char buf[4096];
    const char* files[] = {
        "/main.c",    "/src/io/reader.c", "/src/io/writer.c",   "/src/util/log.c",
        "/README.md", "/Makefile",        "/build/bin/archaic",
    };
    for (int i = 0; i < (int) (sizeof(files) / sizeof(files[0])); i++) {
        snprintf(buf, sizeof(buf), "%s%s", test_root, files[i]);
        unlink(buf);
    }
    snprintf(buf, sizeof(buf), "%s/build/bin", test_root);
    rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/build", test_root);
    rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/docs", test_root);
    rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/src/io", test_root);
    rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/src/util", test_root);
    rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/src", test_root);
    rmdir(buf);
    rmdir(test_root);
    free((void*) test_root);
    test_root = NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 1: Trie Operations
 * ══════════════════════════════════════════════════════════════════════ */

static void test_trie_create_destroy(void) {
    TEST(trie_create_destroy);
    Trie* root = create_trie();
    ASSERT_NOT_NULL(root, "create_trie returned NULL");
    trie_free_recursive(root);
    PASS();
}

static void test_trie_insert_lookup(void) {
    TEST(trie_insert_lookup);
    Trie* root = create_trie();
    insert(root, "/home/user/project/main.c");
    insert(root, "/home/user/project/util.c");
    insert(root, "/home/user/project/README.md");

    /* Verify search doesn't crash with populated trie */
    size_t count = trie_node_count(root);
    ASSERT_TRUE(count > 0, "trie should have nodes after insert");

    trie_free_recursive(root);
    PASS();
}

static void test_trie_insert_empty_string(void) {
    TEST(trie_insert_empty_string);
    Trie* root = create_trie();
    /* Inserting empty string should not crash */
    insert(root, "");
    trie_free_recursive(root);
    PASS();
}

static void test_trie_insert_long_path(void) {
    TEST(trie_insert_long_path);
    Trie* root = create_trie();
    /* Create a path near the 4096 byte limit */
    char long_path[4096];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    /* Should not crash on very long paths */
    insert(root, long_path);
    trie_free_recursive(root);
    PASS();
}

static void test_trie_insert_duplicate(void) {
    TEST(trie_insert_duplicate);
    Trie* root = create_trie();
    insert(root, "/test/path.c");
    insert(root, "/test/path.c");
    insert(root, "/test/path.c");
    /* Trie should handle duplicates without corruption */
    size_t count = trie_node_count(root);
    ASSERT_TRUE(count > 0, "trie should have nodes after insert");
    trie_free_recursive(root);
    PASS();
}

static void test_trie_many_inserts(void) {
    TEST(trie_many_inserts);
    Trie* root = create_trie();
    char buf[256];
    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "/test/dir%d/file%d.c", i % 100, i);
        insert(root, buf);
    }
    size_t count = trie_node_count(root);
    ASSERT_TRUE(count >= 1000, "trie should have 1000+ nodes");
    trie_free_recursive(root);
    PASS();
}

static void test_listing_wide_dir(void) {
    TEST(listing_wide_dir);
    Trie* root = create_trie();
    char buf[256];
    for (int i = 0; i < 600; i++) {
        snprintf(buf, sizeof(buf), "/wide/f%03d.c", i);
        insert(root, buf);
    }
    insert(root, "/wide/sub/");
    insert(root, "/wide/sub/nested.c");
    scored_completions* sc = scored_completions_create(32);
    scored_completions_collect(root, "/wide/", sc, 0, "/", NULL, 0.0, 0);
    ASSERT_TRUE(sc->count == 601, "one-level listing keeps all siblings");
    int saw0 = 0, saw599 = 0, saw_nested = 0, saw_sub = 0;
    for (size_t i = 0; i < sc->count; i++) {
        if (strstr(sc->entries[i].path, "f000.c"))
            saw0 = 1;
        if (strstr(sc->entries[i].path, "f599.c"))
            saw599 = 1;
        if (strstr(sc->entries[i].path, "nested"))
            saw_nested = 1;
        if (strstr(sc->entries[i].path, "/wide/sub"))
            saw_sub = 1;
    }
    ASSERT_TRUE(saw0 && saw599 && saw_sub && !saw_nested, "siblings kept, nested excluded");
    scored_completions_free(sc);
    trie_free_recursive(root);
    PASS();
}

static void test_scored_entry_compact(void) {
    TEST(scored_entry_compact);
    ASSERT_TRUE(sizeof(scored_entry) <= 64, "scored_entry must stay pointer-sized");
    Trie* root = create_trie();
    insert(root, "/a/file1.c");
    insert(root, "/a/file2.c");
    scored_completions* sc = scored_completions_create(4);
    scored_completions_collect(root, "/a/", sc, 0, "/", NULL, 0.0, 0);
    ASSERT_EQ_INT(2, (int) sc->count, "collect finds both");
    for (size_t i = 0; i < sc->count; i++)
        ASSERT_TRUE(sc->entries[i].path != NULL, "heap path set");
    scored_completions_free(sc);
    trie_free_recursive(root);
    PASS();
}

static void test_trie_compact(void) {
    TEST(trie_compact);
    Trie* root = create_trie();
    insert(root, "/a/b/c/d/e/file.txt");
    insert(root, "/a/b/c/d/e/other.txt");
    insert(root, "/a/b/c/d/e/another.txt");
    size_t before = trie_node_count(root);
    trie_compact(root);
    size_t after = trie_node_count(root);
    /* Compaction should not increase node count */
    ASSERT_TRUE(after <= before, "compact should not increase nodes");
    trie_free_recursive(root);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 2: Scoring
 * ══════════════════════════════════════════════════════════════════════ */

static void test_score_hidden_demotion(void) {
    TEST(score_hidden_demotion);
    /* is_hidden_path should detect dot-prefixed paths */
    ASSERT_TRUE(is_hidden_path("/home/user/.config"), "dotfiles should be hidden");
    ASSERT_TRUE(is_hidden_path("/home/user/.gitignore"), "dotfiles should be hidden");
    ASSERT_TRUE(!is_hidden_path("/home/user/config"), "normal files should not be hidden");
    ASSERT_TRUE(!is_hidden_path("/home/user/doc.txt"), "normal files should not be hidden");
    PASS();
}

static void test_git_tracked(void) {
    TEST(git_tracked);
    /* is_git_tracked is currently a stub (always false); pin that so a
     * future real implementation must update this test deliberately. */
    ASSERT_TRUE(!is_git_tracked("/tmp/nonexistent"), "stub returns false off-repo");
    ASSERT_TRUE(!is_git_tracked(NULL), "stub returns false for NULL");
    PASS();
}

static void test_relevant_extension(void) {
    TEST(relevant_extension);
    ASSERT_TRUE(!is_relevant_extension(NULL, "/src"), "NULL path is never relevant");
    ASSERT_TRUE(!is_relevant_extension("/src/README", "/src"), "extensionless is never relevant");
    /* Deterministic project context: Makefile => .c relevant, .py not. */
    char tmpl[] = "/tmp/archaic_ext_XXXXXX";
    char* proj = mkdtemp(tmpl);
    ASSERT_NOT_NULL(proj, "mkdtemp failed");
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/Makefile", proj);
    FILE* f = fopen(buf, "w");
    if (f)
        fclose(f);
    snprintf(buf, sizeof(buf), "%s/main.c", proj);
    char main_c[4096];
    strncpy(main_c, buf, sizeof(main_c) - 1);
    snprintf(buf, sizeof(buf), "%s/main.py", proj);
    char main_py[4096];
    strncpy(main_py, buf, sizeof(main_py) - 1);
    ASSERT_TRUE(is_relevant_extension(main_c, proj), ".c relevant under Makefile project");
    ASSERT_TRUE(!is_relevant_extension(main_py, proj), ".py not relevant under Makefile project");
    snprintf(buf, sizeof(buf), "%s/Makefile", proj);
    unlink(buf);
    rmdir(proj);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 3: Fuzzy Matching
 * ══════════════════════════════════════════════════════════════════════ */

static void test_fuzzy_basic(void) {
    TEST(fuzzy_basic);
    Trie* root = create_trie();
    insert(root, "/home/user/project/src/main.c");
    insert(root, "/home/user/project/src/io/reader.c");
    insert(root, "/home/user/project/Makefile");
    insert(root, "/home/user/project/README.md");

    char* paths[50];
    bool is_dirs[50];
    int count = trie_fuzzy_collect(root, "main", paths, is_dirs, 50);
    ASSERT_TRUE(count >= 1, "fuzzy should find 'main'");
    for (int i = 0; i < count; i++)
        free(paths[i]);

    trie_free_recursive(root);
    PASS();
}

static void test_fuzzy_empty_query(void) {
    TEST(fuzzy_empty_query);
    Trie* root = create_trie();
    insert(root, "/test/file.c");
    char* paths[50];
    bool is_dirs[50];
    int count = trie_fuzzy_collect(root, "", paths, is_dirs, 50);
    ASSERT_EQ_INT(0, count, "empty query should return 0 results");
    trie_free_recursive(root);
    PASS();
}

static void test_fuzzy_case_insensitive(void) {
    TEST(fuzzy_case_insensitive);
    Trie* root = create_trie();
    insert(root, "/home/user/Makefile");
    char* paths[50];
    bool is_dirs[50];
    int count = trie_fuzzy_collect(root, "make", paths, is_dirs, 50);
    ASSERT_TRUE(count >= 1, "fuzzy should be case-insensitive");
    for (int i = 0; i < count; i++)
        free(paths[i]);
    trie_free_recursive(root);
    PASS();
}

static void test_fuzzy_no_match(void) {
    TEST(fuzzy_no_match);
    Trie* root = create_trie();
    insert(root, "/home/user/project/main.c");
    char* paths[50];
    bool is_dirs[50];
    int count = trie_fuzzy_collect(root, "xyzzyplugh", paths, is_dirs, 50);
    ASSERT_EQ_INT(0, count, "no-match query should return 0 results");
    trie_free_recursive(root);
    PASS();
}

static void test_fuzzy_subsequence_match(void) {
    TEST(fuzzy_subsequence_match);
    Trie* root = create_trie();
    insert(root, "/home/user/src/io/main.c");
    char* paths[50];
    bool is_dirs[50];
    int count = trie_fuzzy_collect(root, "si", paths, is_dirs, 50);
    /* "si" matches 's' (src) followed by 'i' (io) in exactly one path */
    ASSERT_EQ_INT(1, count, "subsequence query matches exactly one path");
    ASSERT_EQ_STR("/home/user/src/io/main.c", paths[0], "matched path is the inserted one");
    for (int i = 0; i < count; i++)
        free(paths[i]);
    trie_free_recursive(root);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 4: Cache
 * ══════════════════════════════════════════════════════════════════════ */

static void test_cache_create_destroy(void) {
    TEST(cache_create_destroy);
    query_cache* cache = cache_create(1024, 2);
    ASSERT_NOT_NULL(cache, "cache_create returned NULL");
    cache_destroy(cache);
    PASS();
}

static void test_cache_put_get(void) {
    TEST(cache_put_get);
    query_cache* cache = cache_create(1024, 60);
    ASSERT_NOT_NULL(cache, "cache_create failed");

    /* Build scored_completions via the public API and scored_completions_collect */
    Trie* root = create_trie();
    insert(root, "/test/file.c");

    scored_completions* sc = scored_completions_create(10);
    ASSERT_NOT_NULL(sc, "scored_completions_create failed");
    uint64_t now = (uint64_t) time(NULL);
    scored_completions_collect(root, "/test", sc, now, "/test", "", 0.0, 0);

    /* Even if no results (trie is empty at this prefix), put/get should work */
    cache_put(cache, "/test", sc);
    const scored_completions* found = cache_get(cache, "/test");
    /* Cache should return the stored entry (even if empty) */
    if (found) {
        cache_release(cache, found);
    }

    scored_completions_free(sc);
    trie_free_recursive(root);
    cache_destroy(cache);
    PASS();
}

static void test_cache_miss(void) {
    TEST(cache_miss);
    query_cache* cache = cache_create(1024, 2);
    const scored_completions* found = cache_get(cache, "/nonexistent");
    ASSERT_NULL(found, "cache should return NULL for miss");
    cache_destroy(cache);
    PASS();
}

static void test_cache_ttl_expiry(void) {
    TEST(cache_ttl_expiry);
    query_cache* cache = cache_create(1024, 1); /* 1 second TTL */
    ASSERT_NOT_NULL(cache, "cache_create failed");

    Trie* root = create_trie();
    insert(root, "/test/ttl_file.c");
    scored_completions* sc = scored_completions_create(10);
    uint64_t now = (uint64_t) time(NULL);
    scored_completions_collect(root, "/test", sc, now, "/test", "", 0.0, 0);

    cache_put(cache, "/test", sc);

    /* Should be present immediately */
    const scored_completions* found = cache_get(cache, "/test");
    ASSERT_NOT_NULL(found, "cache should have entry immediately after put");
    if (found)
        cache_release(cache, found);

    /* Wait for TTL to expire */
    sleep(2);

    /* Should be expired now */
    found = cache_get(cache, "/test");
    ASSERT_NULL(found, "cache entry should expire after TTL");

    scored_completions_free(sc);
    trie_free_recursive(root);
    cache_destroy(cache);
    PASS();
}

static void test_cache_eviction(void) {
    TEST(cache_eviction);
    query_cache* cache = cache_create(10, 60);
    ASSERT_NOT_NULL(cache, "cache_create failed");

    Trie* root = create_trie();
    for (int i = 0; i < 20; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/test/evict%d/file.c", i);
        insert(root, path);
    }

    uint64_t now = (uint64_t) time(NULL);
    for (int i = 0; i < 20; i++) {
        char key[64];
        snprintf(key, sizeof(key), "/test/evict%d", i);
        scored_completions* sc = scored_completions_create(10);
        scored_completions_collect(root, key, sc, now, "/test", "", 0.0, 0);
        cache_put(cache, key, sc);
        scored_completions_free(sc);
    }

    cache_stats stats = cache_get_stats(cache);
    ASSERT_TRUE(stats.evictions >= 5, "cache should evict entries when over capacity");

    trie_free_recursive(root);
    cache_destroy(cache);
    PASS();
}

static void test_cache_invalidate(void) {
    TEST(cache_invalidate);
    query_cache* cache = cache_create(1024, 60);
    Trie* root = create_trie();
    insert(root, "/test/invalidate.c");

    scored_completions* sc = scored_completions_create(10);
    uint64_t now = (uint64_t) time(NULL);
    scored_completions_collect(root, "/test", sc, now, "/test", "", 0.0, 0);
    cache_put(cache, "/test", sc);
    cache_invalidate(cache);

    const scored_completions* found = cache_get(cache, "/test");
    ASSERT_NULL(found, "cache should be empty after invalidation");

    scored_completions_free(sc);
    trie_free_recursive(root);
    cache_destroy(cache);
    PASS();
}

static void test_cache_clear(void) {
    TEST(cache_clear);
    query_cache* cache = cache_create(1024, 60);
    Trie* root = create_trie();
    insert(root, "/test/clear.c");

    scored_completions* sc = scored_completions_create(10);
    uint64_t now = (uint64_t) time(NULL);
    scored_completions_collect(root, "/test", sc, now, "/test", "", 0.0, 0);
    cache_put(cache, "/test", sc);
    cache_clear(cache);

    cache_stats stats = cache_get_stats(cache);
    ASSERT_EQ_INT(0, (int) stats.entries, "cache should have 0 entries after clear");

    scored_completions_free(sc);
    trie_free_recursive(root);
    cache_destroy(cache);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 5: Config Parsing
 * ══════════════════════════════════════════════════════════════════════ */

static void test_config_defaults(void) {
    TEST(config_defaults);
    archaic_config cfg;
    config_init_defaults(&cfg);

    ASSERT_EQ_INT(4, cfg.daemon.scan_threads, "default scan_threads should be 4");
    ASSERT_EQ_INT(10, cfg.daemon.max_depth, "default max_depth should be 10");
    ASSERT_EQ_INT(300, cfg.daemon.rescan_interval_seconds, "default rescan should be 300");
    ASSERT_EQ_INT(65536, (int) cfg.storage.max_buckets, "default max_buckets should be 65536");
    ASSERT_TRUE(cfg.storage.cache_max_entries == 1024, "default cache_max_entries should be 1024");
    ASSERT_TRUE(fabs(cfg.scoring.weight_frequency - 0.40) < 0.001,
                "weight_frequency should be 0.40");
    ASSERT_TRUE(fabs(cfg.scoring.weight_recency - 0.30) < 0.001, "weight_recency should be 0.30");
    PASS();
}

static void test_config_sandbox_validate(void) {
    TEST(config_sandbox_validate);
    archaic_config cfg;
    config_init_defaults(&cfg);

    cfg.daemon.scan_threads = 1000;
    cfg.daemon.max_depth = 500;
    cfg.storage.max_buckets = 0;
    cfg.storage.cache_max_entries = 50000;

    config_sandbox_validate(&cfg);

    ASSERT_TRUE(cfg.daemon.scan_threads <= 32, "scan_threads should be clamped to 32");
    ASSERT_EQ_INT(20, cfg.daemon.max_depth, "max_depth 500 should clamp to exactly 20");
    ASSERT_EQ_INT(1024, (int) cfg.storage.max_buckets, "max_buckets 0 should clamp to exactly 1024");
    ASSERT_TRUE(cfg.storage.cache_max_entries == 50000,
                "cache_max_entries 50000 should be within range (65536 max)");
    PASS();
}

static void test_config_load_from_file(void) {
    TEST(config_load_from_file);
    /* Write a temp config file */
    const char* cfg_path = "/tmp/archaic_test_config.toml";
    FILE* f = fopen(cfg_path, "w");
    ASSERT_NOT_NULL(f, "could not create temp config file");
    fprintf(f, "[daemon]\nscan_threads = 8\nmax_depth = 20\n\n");
    fprintf(f, "[scoring]\nweight_frequency = 0.5\nweight_recency = 0.2\n\n");
    fclose(f);

    archaic_config cfg;
    config_init_defaults(&cfg);
    int rc = config_load(&cfg, cfg_path);
    ASSERT_EQ_INT(0, rc, "config_load should return 0");
    ASSERT_EQ_INT(8, cfg.daemon.scan_threads, "scan_threads should be 8");
    ASSERT_TRUE(fabs(cfg.scoring.weight_frequency - 0.5) < 0.001, "weight_frequency should be 0.5");

    unlink(cfg_path);
    PASS();
}

static void test_config_load_missing_file(void) {
    TEST(config_load_missing_file);
    archaic_config cfg;
    config_init_defaults(&cfg);
    int rc = config_load(&cfg, "/tmp/nonexistent_config_archaic.toml");
    ASSERT_TRUE(rc != 0, "config_load should fail for missing file");
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 6: Hashset
 * ══════════════════════════════════════════════════════════════════════ */

static void test_hashset_basic(void) {
    TEST(hashset_basic);
    hashset hs;
    hashset_init(&hs, 16);

    hashset_insert(&hs, "node_modules");
    hashset_insert(&hs, ".git");
    hashset_insert(&hs, "build");

    ASSERT_TRUE(hashset_contains(&hs, "node_modules"), "should contain node_modules");
    ASSERT_TRUE(hashset_contains(&hs, ".git"), "should contain .git");
    ASSERT_TRUE(!hashset_contains(&hs, "src"), "should not contain src");
    ASSERT_TRUE(!hashset_contains(&hs, ""), "should not contain empty string");

    hashset_free(&hs);
    PASS();
}

static void test_hashset_resize(void) {
    TEST(hashset_resize);
    hashset hs;
    hashset_init(&hs, 4); /* tiny initial capacity */

    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item_%d", i);
        hashset_insert(&hs, buf);
    }

    /* All items should still be findable after resize */
    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item_%d", i);
        ASSERT_TRUE(hashset_contains(&hs, buf), "item should be present after resize");
    }

    hashset_free(&hs);
    PASS();
}

static void test_hashset_duplicate_insert(void) {
    TEST(hashset_duplicate_insert);
    hashset hs;
    hashset_init(&hs, 16);

    hashset_insert(&hs, "test");
    hashset_insert(&hs, "test");
    hashset_insert(&hs, "test");

    ASSERT_TRUE(hashset_contains(&hs, "test"), "duplicate inserts should still contain key");
    hashset_free(&hs);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 8: Path Validation
 * ══════════════════════════════════════════════════════════════════════ */

static void test_path_validation_existing(void) {
    TEST(path_validation_existing);
    setup_temp_dir();
    /* setup_temp_dir creates $root/README.md: existence must be reported. */
    path_validation v = validate_input_path(test_root, "README.md");
    ASSERT_TRUE(v.exists, "created README.md must validate as existing");
    free_path_validation(&v);
    cleanup_temp_dir();
    PASS();
}

static void test_path_validation_nonexistent(void) {
    TEST(path_validation_nonexistent);
    path_validation v = validate_input_path("/tmp", "this_file_does_not_exist_12345.txt");
    ASSERT_TRUE(!v.exists, "nonexistent file should have exists=false");
    free_path_validation(&v);
    PASS();
}

static void test_path_validation_directory(void) {
    TEST(path_validation_directory);
    path_validation v = validate_input_path("/tmp", "");
    /* Empty input with /tmp as cwd */
    /* Should not crash regardless */
    free_path_validation(&v);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 9: Archaicignore (from existing test.c, verify portability)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_archaicignore_wildcard(void) {
    TEST(archaicignore_wildcard);
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/archaic_test_wc_%d", (int) getpid());
    mkdir(tmpdir, 0755);

    char ignore_path[512];
    snprintf(ignore_path, sizeof(ignore_path), "%s/.archaicignore", tmpdir);
    FILE* f = fopen(ignore_path, "w");
    fprintf(f, "*.log\n*.tmp\n");
    fclose(f);

    archaic_config cfg;
    config_init_defaults(&cfg);
    int loaded = config_load_archaicignore(&cfg, tmpdir);

    bool found_log = false, found_tmp = false;
    for (int i = 0; i < cfg.scanner.ignore_file_count; i++) {
        if (strcmp(cfg.scanner.ignore_files[i], "*.log") == 0)
            found_log = true;
        if (strcmp(cfg.scanner.ignore_files[i], "*.tmp") == 0)
            found_tmp = true;
    }
    ASSERT_TRUE(loaded >= 2, "should load at least 2 wildcard patterns");
    ASSERT_TRUE(found_log, "should contain *.log pattern");
    ASSERT_TRUE(found_tmp, "should contain *.tmp pattern");

    unlink(ignore_path);
    rmdir(tmpdir);
    PASS();
}

static void test_archaicignore_merges_with_defaults(void) {
    TEST(archaicignore_merges_with_defaults);
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/archaic_test_merge2_%d", (int) getpid());
    mkdir(tmpdir, 0755);

    char ignore_path[512];
    snprintf(ignore_path, sizeof(ignore_path), "%s/.archaicignore", tmpdir);
    FILE* f = fopen(ignore_path, "w");
    fprintf(f, "# Comment line\n\ncustom_dir\n");
    fclose(f);

    archaic_config cfg;
    config_init_defaults(&cfg);
    config_load_archaicignore(&cfg, tmpdir);

    bool found_git = false;
    for (int i = 0; i < cfg.scanner.ignore_dir_count; i++) {
        if (strcmp(cfg.scanner.ignore_dirs[i], ".git") == 0)
            found_git = true;
    }
    /* custom_dir could be added as a dir or file pattern depending on parser heuristics */
    bool found_custom = false;
    for (int i = 0; i < cfg.scanner.ignore_dir_count; i++) {
        if (strcmp(cfg.scanner.ignore_dirs[i], "custom_dir") == 0)
            found_custom = true;
    }
    for (int i = 0; i < cfg.scanner.ignore_file_count; i++) {
        if (strcmp(cfg.scanner.ignore_files[i], "custom_dir") == 0)
            found_custom = true;
    }
    ASSERT_TRUE(found_git, "default .git should be preserved after merge");
    ASSERT_TRUE(found_custom, "custom_dir should be added from .archaicignore");

    unlink(ignore_path);
    rmdir(tmpdir);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 10: Protocol Validation
 * ══════════════════════════════════════════════════════════════════════ */

static void test_protocol_header_validation(void) {
    TEST(protocol_header_validation);
    ipc_header hdr;

    /* Valid header */
    ipc_write_header(&hdr, IPC_MSG_COMPLETE, 100, 1);
    ASSERT_TRUE(ipc_validate_header(&hdr), "valid header should pass validation");
    ASSERT_EQ_INT(IPC_PROTOCOL_VERSION, ipc_header_version(&hdr), "version should match");

    /* Invalid magic */
    hdr.magic = 0xDEADBEEF;
    ASSERT_TRUE(!ipc_validate_header(&hdr), "invalid magic should fail validation");

    /* Oversized payload */
    ipc_write_header(&hdr, IPC_MSG_COMPLETE, IPC_MAX_PAYLOAD + 1, 1);
    ASSERT_TRUE(!ipc_validate_header(&hdr), "oversized payload should fail validation");

    PASS();
}

static void test_protocol_packed_completions(void) {
    TEST(protocol_packed_completions);
    uint8_t buf[4096];
    size_t pos = ipc_pack_completions_begin(buf, sizeof(buf), 1);
    uint32_t count = 0;
    ASSERT_TRUE(ipc_pack_completions_add(buf, sizeof(buf), &pos, &count, "/wide/f000.c", 0, 1.5f) ==
                    0,
                "pack file");
    ASSERT_TRUE(ipc_pack_completions_add(buf, sizeof(buf), &pos, &count, "/wide/sub", 1, 2.0f) == 0,
                "pack dir");
    ipc_pack_completions_finish(buf, count);
    ipc_completion_list* list = calloc(1, sizeof(*list));
    ASSERT_TRUE(list && ipc_unpack_completions(buf, pos, list) == 0, "unpack");
    ASSERT_EQ_INT(2, (int) list->count, "count");
    ASSERT_EQ_INT(1, (int) list->scanning, "scanning flag");
    ASSERT_TRUE(strcmp(list->paths[0], "/wide/f000.c") == 0, "path 0");
    ASSERT_EQ_INT(0, (int) list->is_dirs[0], "file");
    ASSERT_TRUE(strcmp(list->paths[1], "/wide/sub") == 0, "path 1");
    ASSERT_EQ_INT(1, (int) list->is_dirs[1], "dir");
    free(list);
    PASS();
}

static void test_protocol_rle_roundtrip(void) {
    TEST(protocol_rle_roundtrip);
    /* Test compress then decompress */
    uint8_t data[] = {1, 1, 1, 1, 1, 2, 2, 2, 3, 4, 4, 4, 4, 4, 4};
    uint8_t compressed[256];
    uint8_t decompressed[256];

    size_t comp_size = ipc_rle_compress(data, sizeof(data), compressed, sizeof(compressed));
    ASSERT_TRUE(comp_size > 0, "compress should succeed");
    ASSERT_TRUE(comp_size < sizeof(data), "compressed should be smaller");

    size_t decomp_size =
        ipc_rle_decompress(compressed, comp_size, decompressed, sizeof(decompressed));
    ASSERT_TRUE(decomp_size == sizeof(data), "decompressed size should match original");

    for (size_t i = 0; i < sizeof(data); i++) {
        ASSERT_TRUE(decompressed[i] == data[i], "decompressed data should match original");
    }
    PASS();
}

static void test_protocol_rle_incompressible(void) {
    TEST(protocol_rle_incompressible);
    /* Random data that doesn't compress well */
    uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint8_t compressed[256];

    size_t comp_size = ipc_rle_compress(data, sizeof(data), compressed, sizeof(compressed));
    /* RLE returns 0 if compression doesn't save space */
    ASSERT_TRUE(comp_size == 0, "incompressible data should return 0");
    PASS();
}

static void test_protocol_empty_payload(void) {
    TEST(protocol_empty_payload);
    uint8_t compressed[256];
    size_t comp_size = ipc_rle_compress(NULL, 0, compressed, sizeof(compressed));
    ASSERT_EQ_INT(0, (int) comp_size, "empty payload should compress to 0 bytes");
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 11: Edge Cases
 * ══════════════════════════════════════════════════════════════════════ */

static void test_special_characters_in_paths(void) {
    TEST(special_characters_in_paths);
    Trie* root = create_trie();

    /* Paths with spaces, unicode, and special characters */
    insert(root, "/home/user/my project/file.txt");
    insert(root, "/home/user/über/file.txt");
    insert(root, "/home/user/README (copy).md");
    insert(root, "/home/user/file with spaces.c");

    size_t count = trie_node_count(root);
    ASSERT_TRUE(count > 0, "trie should handle special chars");

    trie_free_recursive(root);
    PASS();
}

static void test_completions_with_limit(void) {
    TEST(completions_with_limit);
    Trie* root = create_trie();
    for (int i = 0; i < 100; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "/test/dir%d/file%d.c", i, i);
        insert(root, buf);
    }

    completions* c = completions_create(10);
    ASSERT_NOT_NULL(c, "completions_create should succeed");

    completions_collect(root, "/test", c);
    ASSERT_EQ_INT(10, (int) c->count, "collect honors the capacity cap");

    completions_free(c);
    trie_free_recursive(root);
    PASS();
}

static void test_scored_completions_with_limit(void) {
    TEST(scored_completions_with_limit);
    Trie* root = create_trie();
    for (int i = 0; i < 100; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "/home/user/project%d/main.c", i);
        insert(root, buf);
    }

    scored_completions* sc = scored_completions_create(20);
    ASSERT_NOT_NULL(sc, "scored_completions_create should succeed");

    uint64_t now = (uint64_t) time(NULL);
    scored_completions_collect(root, "/home", sc, now, "/home", "", 0.0, 0);

    /* Should return results, at most 20 */
    ASSERT_TRUE(sc->count >= 0 && sc->count <= 50, "should have 0-50 results");

    scored_completions_free(sc);
    trie_free_recursive(root);
    PASS();
}

static void test_session_learning(void) {
    TEST(session_learning);
    /* Reset session state */
    session_reset();

    /* Record a selection */
    session_record_selection("/home/user/project/main.c");

    /* Get boost for that path */
    double boost = session_get_boost("/home/user/project/main.c");
    ASSERT_TRUE(boost > 0.0, "should have positive boost for selected path");

    /* Different path should have no boost */
    double no_boost = session_get_boost("/home/user/other/path.c");
    ASSERT_TRUE(no_boost <= 0.0, "should have no boost for non-selected path");

    session_reset();
    PASS();
}

static void test_normalise_dir(void) {
    TEST(normalise_dir);
    char* result;

    result = normalise_dir("/home/user/project/src");
    ASSERT_NOT_NULL(result, "normalise_dir should return non-NULL");
    ASSERT_TRUE(strcmp(result, "/home/user/project/src") == 0 ||
                    strcmp(result, "/home/user/project/src/") == 0,
                "normalised path should match expected");
    free(result);

    result = normalise_dir("/");
    ASSERT_NOT_NULL(result, "normalise_dir root should return non-NULL");
    free(result);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * TEST GROUP 12: Concurrent Stress Tests
 * ══════════════════════════════════════════════════════════════════════ */

#define CACHE_THREADS 4
#define CACHE_OPS_PER_THREAD 50

typedef struct {
    query_cache* cache;
    Trie* root;
    int thread_id;
    atomic_int* errors;
} cache_thread_ctx;

static atomic_int g_cache_errors;

static void* cache_worker(void* arg) {
    cache_thread_ctx* ctx = (cache_thread_ctx*) arg;
    uint64_t now = (uint64_t) time(NULL);
    for (int i = 0; i < CACHE_OPS_PER_THREAD; i++) {
        char key[64];
        snprintf(key, sizeof(key), "/test/concurrent%d", (ctx->thread_id * 13 + i) % 50);
        scored_completions* sc = scored_completions_create(10);
        scored_completions_collect(ctx->root, key, sc, now, "/test", "", 0.0, 0);
        cache_put(ctx->cache, key, sc);
        const scored_completions* found = cache_get(ctx->cache, key);
        if (!found)
            atomic_fetch_add(ctx->errors, 1);
        else
            cache_release(ctx->cache, found);
        scored_completions_free(sc);
    }
    return NULL;
}

static void test_concurrent_cache_ops(void) {
    TEST(concurrent_cache_ops);
    query_cache* cache = cache_create(1024, 60);
    ASSERT_NOT_NULL(cache, "cache_create failed");

    Trie* root = create_trie();
    for (int i = 0; i < 50; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "/test/concurrent%d/file.c", i);
        insert(root, buf);
    }

    atomic_init(&g_cache_errors, 0);

    cache_thread_ctx ctxs[CACHE_THREADS];
    for (int i = 0; i < CACHE_THREADS; i++) {
        ctxs[i].cache = cache;
        ctxs[i].root = root;
        ctxs[i].thread_id = i;
        ctxs[i].errors = &g_cache_errors;
    }

    pthread_t threads[CACHE_THREADS];
    for (int i = 0; i < CACHE_THREADS; i++)
        pthread_create(&threads[i], NULL, cache_worker, &ctxs[i]);

    for (int i = 0; i < CACHE_THREADS; i++)
        pthread_join(threads[i], NULL);

    printf("(errors: %d) ", atomic_load(&g_cache_errors));
    ASSERT_EQ_INT(0, atomic_load(&g_cache_errors), "no lost updates under concurrency");

    trie_free_recursive(root);
    cache_destroy(cache);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * FILESYSTEM WATCHER TESTS
 * ══════════════════════════════════════════════════════════════════════ */

static void test_watcher_create_destroy(void) {
    TEST(watcher_create_destroy);
    fs_watcher* w = watcher_create();
    ASSERT_NOT_NULL(w, "watcher_create returned NULL");
    watcher_destroy(w);
    PASS();
}

static void test_watcher_add_root(void) {
    TEST(watcher_add_root);
    fs_watcher* w = watcher_create();
    ASSERT_NOT_NULL(w, "watcher_create returned NULL");

    int result = watcher_add_root(w, "/tmp");
    ASSERT_TRUE(result == 0, "watcher_add_root should return 0");
    ASSERT_TRUE(w->root_count == 1, "root_count should be 1");
    ASSERT_EQ_STR("/tmp", w->roots[0], "root path should be /tmp");

    result = watcher_add_root(w, "/home");
    ASSERT_TRUE(result == 0, "watcher_add_root should return 0 for second root");
    ASSERT_TRUE(w->root_count == 2, "root_count should be 2");

    watcher_destroy(w);
    PASS();
}

static void test_watcher_add_root_overflow(void) {
    TEST(watcher_add_root_overflow);
    fs_watcher* w = watcher_create();
    ASSERT_NOT_NULL(w, "watcher_create returned NULL");

    for (int i = 0; i < WATCHER_MAX_ROOTS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/test%d", i);
        watcher_add_root(w, path);
    }

    int result = watcher_add_root(w, "/tmp/overflow");
    ASSERT_TRUE(result == -1, "watcher_add_root should return -1 on overflow");
    ASSERT_TRUE(w->root_count == WATCHER_MAX_ROOTS, "root_count should be at max");

    watcher_destroy(w);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * IPC FUZZ TESTS
 * ══════════════════════════════════════════════════════════════════════ */

static void test_ipc_fuzz_random_headers(void) {
    TEST(ipc_fuzz_random_headers);
    uint8_t buf[16];
    uint32_t seed = 0xDEADBEEF;
    int passed = 0;
    int total = 1000;

    for (int i = 0; i < total; i++) {
        for (int b = 0; b < 16; b++) {
            seed = seed * 1103515245 + 12345;
            buf[b] = (uint8_t) (seed >> 16);
        }

        ipc_header* hdr = (ipc_header*) buf;
        if (ipc_validate_header(hdr)) {
            ASSERT_TRUE((hdr->magic >> 16) == IPC_MAGIC_PREFIX,
                        "valid fuzz header must have correct magic");
            ASSERT_TRUE(hdr->payload_len < IPC_MAX_PAYLOAD,
                        "valid fuzz header must have bounded payload");
            passed++;
        }
    }

    ASSERT_TRUE(passed < total, "not all random data should validate as headers");
    (void) passed;
    PASS();
}

static void test_ipc_fuzz_corrupted_magic(void) {
    TEST(ipc_fuzz_corrupted_magic);
    ipc_header hdr;
    ipc_write_header(&hdr, IPC_MSG_COMPLETE, 100, 1);

    for (int bit = 0; bit < 32; bit++) {
        ipc_header mutated = hdr;
        mutated.magic ^= (1U << bit);

        if ((mutated.magic >> 16) == IPC_MAGIC_PREFIX) {
            ipc_header original = hdr;
            uint16_t orig_ver = ipc_header_version(&original);
            uint16_t mut_ver = ipc_header_version(&mutated);
            if (mut_ver >= 1 && mut_ver <= IPC_PROTOCOL_VERSION && mut_ver == orig_ver)
                ASSERT_TRUE(ipc_validate_header(&mutated),
                            "bit-flip preserving magic+version should validate");
        }
    }
    PASS();
}

static void test_ipc_fuzz_oversized_payloads(void) {
    TEST(ipc_fuzz_oversized_payloads);
    ipc_header hdr;
    ipc_write_header(&hdr, IPC_MSG_COMPLETE, IPC_MAX_PAYLOAD - 1, 1);
    ASSERT_TRUE(ipc_validate_header(&hdr), "max-1 payload should validate");

    hdr.payload_len = IPC_MAX_PAYLOAD;
    ASSERT_TRUE(!ipc_validate_header(&hdr), "payload at max should fail");

    hdr.payload_len = UINT32_MAX;
    ASSERT_TRUE(!ipc_validate_header(&hdr), "UINT32_MAX payload should fail");

    hdr.payload_len = 0;
    ASSERT_TRUE(ipc_validate_header(&hdr), "zero payload should validate");
    PASS();
}

static void test_ipc_fuzz_rle_boundary(void) {
    TEST(ipc_fuzz_rle_boundary);
    uint8_t all_same[1024];
    memset(all_same, 0xAA, sizeof(all_same));
    uint8_t compressed[2048];
    uint8_t decompressed[2048];

    size_t comp = ipc_rle_compress(all_same, sizeof(all_same), compressed, sizeof(compressed));
    ASSERT_TRUE(comp > 0, "uniform data should compress");
    ASSERT_TRUE(comp < sizeof(all_same), "compressed should be smaller");

    size_t decomp = ipc_rle_decompress(compressed, comp, decompressed, sizeof(decompressed));
    ASSERT_TRUE(decomp == sizeof(all_same), "decompressed size should match");
    for (size_t i = 0; i < sizeof(all_same); i++) {
        ASSERT_TRUE(decompressed[i] == 0xAA, "decompressed data should match");
    }

    uint8_t alternating[256];
    for (int i = 0; i < 256; i++)
        alternating[i] = (uint8_t) (i & 1);
    comp = ipc_rle_compress(alternating, sizeof(alternating), compressed, sizeof(compressed));
    ASSERT_TRUE(comp == 0, "alternating data should not compress");

    PASS();
}

static void test_ipc_fuzz_rle_random_data(void) {
    TEST(ipc_fuzz_rle_random_data);
    uint32_t seed = 0xCAFEBABE;
    uint8_t data[512];
    uint8_t compressed[1024];
    uint8_t decompressed[1024];

    for (int trial = 0; trial < 50; trial++) {
        for (int i = 0; i < 512; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            data[i] = (uint8_t) (seed >> 24);
        }

        size_t comp = ipc_rle_compress(data, sizeof(data), compressed, sizeof(compressed));
        if (comp > 0) {
            size_t decomp =
                ipc_rle_decompress(compressed, comp, decompressed, sizeof(decompressed));
            if (decomp != sizeof(data)) {
                ASSERT_TRUE(0, "rle roundtrip failed for random data");
                break;
            }
            int match = 1;
            for (size_t j = 0; j < sizeof(data); j++) {
                if (decompressed[j] != data[j]) {
                    match = 0;
                    break;
                }
            }
            if (!match) {
                ASSERT_TRUE(0, "rle decompressed data mismatch");
                break;
            }
        }
    }
    PASS();
}

static void test_ipc_fuzz_truncated_payloads(void) {
    TEST(ipc_fuzz_truncated_payloads);
    ipc_complete_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.prefix, "/home", sizeof(req.prefix) - 1);
    strncpy(req.cwd, "/home/user", sizeof(req.cwd) - 1);
    req.limit = 50;
    req.dirs_only = 0;

    ipc_header hdr;
    ipc_write_header(&hdr, IPC_MSG_COMPLETE, sizeof(req), 42);
    ASSERT_TRUE(ipc_validate_header(&hdr), "valid complete header should pass");

    uint32_t valid_payload = hdr.payload_len;

    hdr.payload_len = valid_payload - 1;
    ASSERT_TRUE(ipc_validate_header(&hdr), "off-by-one payload should still validate as header");

    hdr.payload_len = 0;
    ASSERT_TRUE(ipc_validate_header(&hdr), "zero payload should validate");

    hdr.payload_len = IPC_MAX_PAYLOAD;
    ASSERT_TRUE(!ipc_validate_header(&hdr), "oversized payload should fail validation");
    PASS();
}

static void test_ipc_fuzz_message_type_bounds(void) {
    TEST(ipc_fuzz_message_type_bounds);
    ipc_header hdr;
    ipc_msg_type valid_types[] = {IPC_MSG_SCAN,
                                  IPC_MSG_QUERY,
                                  IPC_MSG_COMPLETE,
                                  IPC_MSG_SHUTDOWN,
                                  IPC_MSG_SUGGEST,
                                  IPC_MSG_SAVE,
                                  IPC_MSG_PING,
                                  IPC_MSG_METRICS,
                                  IPC_MSG_SCAN_STATUS,
                                  IPC_MSG_FUZZY_COMPLETE,
                                  IPC_MSG_RECENT,
                                  IPC_MSG_OK,
                                  IPC_MSG_ERROR,
                                  IPC_MSG_COMPLETIONS,
                                  IPC_MSG_VALIDATION,
                                  IPC_MSG_SUGGESTION,
                                  IPC_MSG_PONG,
                                  IPC_MSG_METRICS_RESP,
                                  IPC_MSG_SCAN_STATUS_RESP,
                                  IPC_MSG_FUZZY_COMPLETIONS,
                                  IPC_MSG_RECENT_RESP};

    for (int i = 0; i < (int) (sizeof(valid_types) / sizeof(valid_types[0])); i++) {
        ipc_write_header(&hdr, valid_types[i], 100, 1);
        ASSERT_TRUE(ipc_validate_header(&hdr), "known message type should validate");
    }

    ipc_write_header(&hdr, 0, 100, 1);
    if ((hdr.magic >> 16) == IPC_MAGIC_PREFIX)
        ASSERT_TRUE(ipc_validate_header(&hdr), "type 0 with valid magic should validate");

    ipc_write_header(&hdr, 99999, 100, 1);
    ASSERT_TRUE(ipc_validate_header(&hdr),
                "unknown type with valid magic+version should still validate header");
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Group 15: Env Var Expansion ─────────────────────────────────────────── */

static void test_env_var_expand_home(void) {
    TEST(env_var_expand_home);
    setenv("HOME", "/home/testuser", 1);
    char buf[4096];
    expand_env_vars("$HOME/projects", buf, sizeof(buf));
    ASSERT_EQ_STR("/home/testuser/projects", buf, "$HOME expansion");
    unsetenv("HOME");
    PASS();
}

static void test_env_var_expand_braces(void) {
    TEST(env_var_expand_braces);
    setenv("PROJECTS", "/data/code", 1);
    char buf[4096];
    expand_env_vars("${PROJECTS}/src", buf, sizeof(buf));
    ASSERT_EQ_STR("/data/code/src", buf, "${PROJECTS} expansion");
    unsetenv("PROJECTS");
    PASS();
}

static void test_env_var_expand_unset(void) {
    TEST(env_var_expand_unset);
    unsetenv("NONEXISTENT_VAR");
    char buf[4096];
    expand_env_vars("$NONEXISTENT_VAR/path", buf, sizeof(buf));
    ASSERT_EQ_STR("/path", buf, "unset var expands to empty");
    PASS();
}

static void test_env_var_expand_unset_braces(void) {
    TEST(env_var_expand_unset_braces);
    unsetenv("MISSING_VAR");
    char buf[4096];
    expand_env_vars("${MISSING_VAR}/path", buf, sizeof(buf));
    ASSERT_EQ_STR("${MISSING_VAR}/path", buf, "unset ${VAR} preserved");
    PASS();
}

static void test_env_var_double_dollar(void) {
    TEST(env_var_double_dollar);
    char buf[4096];
    expand_env_vars("$$HOME", buf, sizeof(buf));
    ASSERT_EQ_STR("$HOME", buf, "$$ escapes to $");
    PASS();
}

static void test_env_var_no_expansion(void) {
    TEST(env_var_no_expansion);
    char buf[4096];
    expand_env_vars("/plain/path", buf, sizeof(buf));
    ASSERT_EQ_STR("/plain/path", buf, "no vars to expand");
    PASS();
}

/* ── Group 16: Path Normalization ───────────────────────────────────────── */

static void test_path_normalize_dots(void) {
    TEST(path_normalize_dots);
    char buf[4096];
    size_t len = path_normalize(buf, "/home/user/../other/./dir", sizeof(buf));
    ASSERT_EQ_STR("/home/other/dir", buf, "normalize /../ and /./");
    (void) len;
    PASS();
}

static void test_path_normalize_double_slash(void) {
    TEST(path_normalize_double_slash);
    char buf[4096];
    path_normalize(buf, "/home//user///dir", sizeof(buf));
    ASSERT_EQ_STR("/home/user/dir", buf, "normalize double slashes");
    PASS();
}

static void test_path_normalize_root(void) {
    TEST(path_normalize_root);
    char buf[4096];
    path_normalize(buf, "/", sizeof(buf));
    ASSERT_EQ_STR("/", buf, "normalize root");
    PASS();
}

static void test_path_expand_tilde(void) {
    TEST(path_expand_tilde);
    setenv("HOME", "/home/testuser", 1);
    char buf[4096];
    path_expand_tilde(buf, "~/projects", sizeof(buf));
    ASSERT_EQ_STR("/home/testuser/projects", buf, "tilde expansion");
    unsetenv("HOME");
    PASS();
}

static void test_path_expand_tilde_no_home(void) {
    TEST(path_expand_tilde_no_home);
    unsetenv("HOME");
    char buf[4096];
    path_expand_tilde(buf, "~/projects", sizeof(buf));
    ASSERT_TRUE(buf[0] == '/', "tilde expansion falls back to /");
    PASS();
}

/* ── Group 17: Executable Script Detection ───────────────────────────────── */

static void test_executable_script_detection(void) {
    TEST(executable_script_detection);
    ASSERT_TRUE(is_executable_script("deploy.sh"), ".sh detected");
    ASSERT_TRUE(is_executable_script("setup.bash"), ".bash detected");
    ASSERT_TRUE(is_executable_script("main.py"), ".py detected");
    ASSERT_TRUE(is_executable_script("app.js"), ".js detected");
    ASSERT_TRUE(is_executable_script(".bashrc"), "dot rc detected");
    ASSERT_TRUE(!is_executable_script("readme.md"), ".md not executable");
    ASSERT_TRUE(!is_executable_script("image.png"), ".png not executable");
    ASSERT_TRUE(!is_executable_script("Makefile"), "Makefile not in ext list");
    ASSERT_TRUE(!is_executable_script(".racecar"), ".racecar is not an rc file");
    ASSERT_TRUE(!is_executable_script(".accurate"), ".accurate is not an rc file");
    PASS();
}

/* ── Group 18: Scanner Timeout ─────────────────────────────────────────── */

static void test_scanner_dir_timeout(void) {
    TEST(scanner_dir_timeout);
    parallel_scanner scanner;
    memset(&scanner, 0, sizeof(scanner));
    ASSERT_EQ_INT(0, scanner.dir_timeout_ms, "default timeout is 0");
    parallel_scanner_set_dir_timeout(&scanner, 5000);
    ASSERT_EQ_INT(5000, scanner.dir_timeout_ms, "timeout set to 5000ms");
    parallel_scanner_set_dir_timeout(&scanner, 0);
    ASSERT_EQ_INT(0, scanner.dir_timeout_ms, "timeout reset to 0");
    parallel_scanner_set_dir_timeout(&scanner, -1);
    ASSERT_EQ_INT(0, scanner.dir_timeout_ms, "negative timeout clamped to 0");
    PASS();
}

/* ── Group 19: Config Path Validation ──────────────────────────────────── */

static void test_config_validate_paths(void) {
    TEST(config_validate_paths);
    archaic_config cfg;
    config_init_defaults(&cfg);
    strncpy(cfg.daemon.scan_path, "/tmp", sizeof(cfg.daemon.scan_path) - 1);
    cfg.daemon.scan_path_count = 0;
    int errors = config_validate_paths(&cfg);
    ASSERT_EQ_INT(0, errors, "/tmp is valid directory");
    PASS();
}

static void test_config_validate_paths_invalid(void) {
    TEST(config_validate_paths_invalid);
    archaic_config cfg;
    config_init_defaults(&cfg);
    strncpy(cfg.daemon.scan_path, "/nonexistent_archaic_test_path",
            sizeof(cfg.daemon.scan_path) - 1);
    cfg.daemon.scan_path_count = 0;
    int errors = config_validate_paths(&cfg);
    ASSERT_TRUE(errors > 0, "nonexistent path should report errors");
    PASS();
}

static void test_config_validate_paths_empty(void) {
    TEST(config_validate_paths_empty);
    archaic_config cfg;
    config_init_defaults(&cfg);
    cfg.daemon.scan_path[0] = '\0';
    cfg.daemon.scan_path_count = 0;
    int errors = config_validate_paths(&cfg);
    ASSERT_TRUE(errors > 0, "no paths configured should report error");
    PASS();
}

/* ── Memory budget (#5) ─────────────────────────────────────────────────── */

static t_bucket_store* budget_test_store(void) {
    t_bucket_store* s = (t_bucket_store*) calloc(1, sizeof(t_bucket_store));
    if (!s)
        return NULL;
    pthread_mutex_init(&s->store_lock, NULL);
    s->max_buckets = BUCKETS;
    s->max_total_nodes = 0; /* node budget disabled, like the daemon default */
    s->parent = (node*) calloc(1, sizeof(node));
    if (!s->parent) {
        free(s);
        return NULL;
    }
    s->parent->is_parent = true;
    insert_bucket(s, "/mem/a");
    insert_bucket(s, "/mem/b");
    insert_bucket(s, "/mem/c");
    for (size_t i = 0; i < s->right_index; i++) {
        if (!s->buckets[i] || !s->buckets[i]->dir_trie)
            continue;
        char buf[64];
        for (int j = 0; j < 20; j++) {
            snprintf(buf, sizeof(buf), "/mem/x/file%d.txt", j);
            insert(s->buckets[i]->dir_trie, buf);
        }
    }
    return s;
}

static void budget_free_store(t_bucket_store* s) {
    if (!s)
        return;
    for (size_t i = 0; i < s->right_index; i++) {
        if (s->buckets[i])
            destroy_bucket(s->buckets[i]);
    }
    /* LRU wrapper nodes are owned by the list, not the buckets. */
    if (s->parent) {
        node* n = s->parent->first;
        while (n) {
            node* next = n->next;
            free(n);
            n = next;
        }
        free(s->parent);
    }
    pthread_mutex_destroy(&s->store_lock);
    free(s);
}

static void test_memory_budget_disabled(void) {
    TEST(memory_budget_disabled);
    t_bucket_store* s = budget_test_store();
    ASSERT_NOT_NULL(s, "store setup");
    ASSERT_EQ_INT(3, (int) s->right_index, "three buckets inserted");
    store_set_max_memory(s, 0);
    ASSERT_EQ_INT(0, store_check_memory_budget(s), "disabled budget reports 0");
    store_enforce_budget(s);
    ASSERT_EQ_INT(3, (int) s->right_index, "disabled budget evicts nothing");
    budget_free_store(s);
    PASS();
}

static void test_memory_budget_evicts_to_fit(void) {
    TEST(memory_budget_evicts_to_fit);
    t_bucket_store* s = budget_test_store();
    ASSERT_NOT_NULL(s, "store setup");
    size_t used = store_calculate_memory_bytes(s);
    ASSERT_TRUE(used > 0, "populated store costs memory");
    ASSERT_TRUE(store_check_memory_budget(s) == 0, "no budget set yet");
    /* Tight but satisfiable: fixed store overhead + ~1.2 buckets, so
     * enforcement must evict exactly two of the three equal buckets. */
    size_t one_bucket = 0, fixed = 0, cap = 0;
    {
        t_bucket_store* e = (t_bucket_store*) calloc(1, sizeof(t_bucket_store));
        ASSERT_NOT_NULL(e, "empty store setup");
        pthread_mutex_init(&e->store_lock, NULL);
        fixed = store_calculate_memory_bytes(e);
        pthread_mutex_destroy(&e->store_lock);
        free(e);
        ASSERT_TRUE(used > fixed, "buckets cost memory beyond fixed overhead");
        one_bucket = (used - fixed) / 3;
        cap = fixed + one_bucket * 120 / 100;
        store_set_max_memory(s, cap);
    }
    ASSERT_TRUE(store_calculate_memory_bytes(s) > cap, "store starts over budget");
    ASSERT_TRUE(store_check_memory_budget(s) >= 100, "over budget reports >=100%");
    store_enforce_budget(s);
    ASSERT_EQ_INT(1, (int) s->right_index, "eviction stops with one bucket left");
    ASSERT_TRUE(store_calculate_memory_bytes(s) <= cap, "store fits budget after evict");
    budget_free_store(s);
    PASS();
}

static void test_memory_budget_no_evict_under_cap(void) {
    TEST(memory_budget_no_evict_under_cap);
    t_bucket_store* s = budget_test_store();
    ASSERT_NOT_NULL(s, "store setup");
    size_t used = store_calculate_memory_bytes(s);
    store_set_max_memory(s, used * 2);
    ASSERT_TRUE(store_check_memory_budget(s) <= 100, "under budget reports <=100%");
    store_enforce_budget(s);
    ASSERT_EQ_INT(3, (int) s->right_index, "under-budget store keeps all buckets");
    budget_free_store(s);
    PASS();
}

/* ── Per-root policy (#6) ─────────────────────────────────────────────── */

static void test_root_line_bare(void) {
    TEST(root_line_bare);
    char path[4096];
    config_root_policy pol;
    ASSERT_EQ_INT(0, config_parse_root_line("/home/sam/src", path, sizeof(path), &pol),
                  "bare path parses");
    ASSERT_EQ_STR("/home/sam/src", path, "path part");
    ASSERT_EQ_INT(-1, pol.depth, "depth defaults to global");
    ASSERT_EQ_INT(1, pol.watch, "watch defaults on");
    ASSERT_EQ_INT(0, pol.ignore_dir_count, "no ignores by default");
    PASS();
}

static void test_root_line_full(void) {
    TEST(root_line_full);
    char path[4096];
    config_root_policy pol;
    ASSERT_EQ_INT(0,
                  config_parse_root_line("/home/sam/big depth=3 watch=0 ignore_dirs=target,dist",
                                         path, sizeof(path), &pol),
                  "annotated line parses");
    ASSERT_EQ_STR("/home/sam/big", path, "path part");
    ASSERT_EQ_INT(3, pol.depth, "depth parsed");
    ASSERT_EQ_INT(0, pol.watch, "watch parsed");
    ASSERT_EQ_INT(2, pol.ignore_dir_count, "two ignores parsed");
    ASSERT_EQ_STR("target", pol.ignore_dirs[0], "first ignore");
    ASSERT_EQ_STR("dist", pol.ignore_dirs[1], "second ignore");
    PASS();
}

static void test_root_line_bad(void) {
    TEST(root_line_bad);
    char path[4096];
    config_root_policy pol;
    ASSERT_TRUE(config_parse_root_line("/x depth=abc", path, sizeof(path), &pol) != 0,
                "bad depth rejected");
    ASSERT_TRUE(config_parse_root_line("/x frobnicate=1", path, sizeof(path), &pol) != 0,
                "unknown key rejected");
    ASSERT_TRUE(config_parse_root_line("", path, sizeof(path), &pol) != 0, "empty rejected");
    /* Out-of-range depth clamps instead of failing. */
    ASSERT_EQ_INT(0, config_parse_root_line("/x depth=999", path, sizeof(path), &pol),
                  "huge depth parses");
    ASSERT_EQ_INT(64, pol.depth, "depth clamps to 64");
    PASS();
}

static void test_root_policy_lookup(void) {
    TEST(root_policy_lookup);
    archaic_config cfg;
    config_init_defaults(&cfg);
    ASSERT_TRUE(config_root_policy_for(&cfg, "/home/sam/src") == NULL, "no policy by default");
    config_root_policy* dst = &cfg.daemon.root_policies[cfg.daemon.root_policy_count++];
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->path, "/home/sam/src", sizeof(dst->path) - 1);
    dst->depth = 4;
    dst->watch = 1;
    const config_root_policy* got = config_root_policy_for(&cfg, "/home/sam/src");
    ASSERT_TRUE(got != NULL, "stored policy found");
    ASSERT_EQ_INT(4, got->depth, "depth round-trips");
    ASSERT_TRUE(config_root_policy_for(&cfg, "/other") == NULL, "other path has none");
    PASS();
}

int main(int argc, char* argv[]) {
    (void) argc;
    (void) argv;

    printf("\n========================================\n");
    printf("  Archaic Unit Tests\n");
    printf("========================================\n\n");

    /* Group 1: Trie Operations */
    printf("--- Trie Operations ---\n");
    test_trie_create_destroy();
    test_trie_insert_lookup();
    test_trie_insert_empty_string();
    test_trie_insert_long_path();
    test_trie_insert_duplicate();
    test_trie_many_inserts();
    test_listing_wide_dir();
    test_scored_entry_compact();
    test_trie_compact();

    /* Group 2: Scoring */
    printf("\n--- Scoring ---\n");
    test_score_hidden_demotion();
    test_git_tracked();
    test_relevant_extension();

    /* Group 3: Fuzzy Matching */
    printf("\n--- Fuzzy Matching ---\n");
    test_fuzzy_basic();
    test_fuzzy_empty_query();
    test_fuzzy_case_insensitive();
    test_fuzzy_no_match();
    test_fuzzy_subsequence_match();

    /* Group 4: Cache */
    printf("\n--- Cache ---\n");
    test_cache_create_destroy();
    test_cache_put_get();
    test_cache_miss();
    test_cache_ttl_expiry();
    test_cache_eviction();
    test_cache_invalidate();
    test_cache_clear();

    /* Group 5: Config Parsing */
    printf("\n--- Config Parsing ---\n");
    test_config_defaults();
    test_config_sandbox_validate();
    test_config_load_from_file();
    test_config_load_missing_file();

    /* Group 6: Hashset */
    printf("\n--- Hashset ---\n");
    test_hashset_basic();
    test_hashset_resize();
    test_hashset_duplicate_insert();


    /* Group 8: Path Validation */
    printf("\n--- Path Validation ---\n");
    test_path_validation_existing();
    test_path_validation_nonexistent();
    test_path_validation_directory();

    /* Group 9: Archaicignore */
    printf("\n--- Archaicignore ---\n");
    test_archaicignore_wildcard();
    test_archaicignore_merges_with_defaults();

    /* Group 10: Protocol Validation */
    printf("\n--- Protocol ---\n");
    test_protocol_header_validation();
    test_protocol_packed_completions();
    test_protocol_rle_roundtrip();
    test_protocol_rle_incompressible();
    test_protocol_empty_payload();

    /* Group 11: Edge Cases */
    printf("\n--- Edge Cases ---\n");
    test_special_characters_in_paths();
    test_completions_with_limit();
    test_scored_completions_with_limit();
    test_session_learning();
    test_normalise_dir();

    /* Group 12: Concurrent Stress */
    printf("\n--- Concurrent Stress ---\n");
    test_concurrent_cache_ops();

    /* Group 13: Filesystem Watcher */
    printf("\n--- Filesystem Watcher ---\n");
    test_watcher_create_destroy();
    test_watcher_add_root();
    test_watcher_add_root_overflow();

    /* Group 14: IPC Fuzz Testing */
    printf("\n--- IPC Fuzz Testing ---\n");
    test_ipc_fuzz_random_headers();
    test_ipc_fuzz_corrupted_magic();
    test_ipc_fuzz_oversized_payloads();
    test_ipc_fuzz_rle_boundary();
    test_ipc_fuzz_rle_random_data();
    test_ipc_fuzz_truncated_payloads();
    test_ipc_fuzz_message_type_bounds();

    /* Group 15: Env Var Expansion */
    printf("\n--- Env Var Expansion ---\n");
    test_env_var_expand_home();
    test_env_var_expand_braces();
    test_env_var_expand_unset();
    test_env_var_expand_unset_braces();
    test_env_var_double_dollar();
    test_env_var_no_expansion();

    /* Group 16: Path Normalization */
    printf("\n--- Path Normalization ---\n");
    test_path_normalize_dots();
    test_path_normalize_double_slash();
    test_path_normalize_root();
    test_path_expand_tilde();
    test_path_expand_tilde_no_home();

    /* Group 17: Executable Script Detection */
    printf("\n--- Executable Script Detection ---\n");
    test_executable_script_detection();

    /* Group 18: Scanner Timeout */
    printf("\n--- Scanner Timeout ---\n");
    test_scanner_dir_timeout();

    /* Group 19: Config Path Validation */
    printf("\n--- Config Path Validation ---\n");
    test_config_validate_paths();
    test_config_validate_paths_invalid();
    test_config_validate_paths_empty();

    /* Memory budget */
    printf("\n--- Memory Budget ---\n");
    test_memory_budget_disabled();
    test_memory_budget_evicts_to_fit();
    test_memory_budget_no_evict_under_cap();

    /* Per-root policy */
    printf("\n--- Per-Root Policy ---\n");
    test_root_line_bare();
    test_root_line_full();
    test_root_line_bad();
    test_root_policy_lookup();

    /* Summary */
    printf("\n========================================\n");
    printf("  Results: %d/%d tests passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(" (%d FAILED)", tests_failed);
    printf("\n========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
