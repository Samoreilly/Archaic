#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "../src/threadmanager.h"
#include "../src/trie-storage.h"
#include "../src/trie.h"
#include "../src/io/fileloader.h"
#include "../src/lru.h"

static const char* test_scan_path = NULL;

void set_test_scan_path(const char* path) {
    test_scan_path = path;
}

static int test_char_index(char c) {
    if (c >= 'a' && c <= 'z') {
        return c - 'a';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= '0' && c <= '9') {
        return 26 + (c - '0');
    }
    if (c == '/') {
        return 36;
    }
    if (c == '.') {
        return 37;
    }
    if (c == '_') {
        return 38;
    }
    if (c == '-') {
        return 39;
    }
    return -1;
}

static bool trie_contains_path(Trie* root, const char* str) {
    Trie* curr = root;

    for (size_t i = 0; str[i] != '\0'; i++) {
        int idx = test_char_index(str[i]);
        if (idx < 0 || idx >= TRIE_CHILDREN) {
            continue;
        }
        if (!curr->children[idx]) {
            return false;
        }
        curr = curr->children[idx];
    }

    return true;
}

static bool trie_has_any(Trie* root) {
    for (size_t i = 0; i < TRIE_CHILDREN; i++) {
        if (root->children[i]) {
            return true;
        }
    }
    return false;
}

static bool any_bucket_has_entries(t_bucket_store* store) {
    if (!store) {
        return false;
    }
    for (size_t i = 0; i < store->right_index; i++) {
        t_bucket* bucket = store->buckets[i];
        if (!bucket) {
            continue;
        }
        if (bucket->dir_count > 0 || trie_has_any(bucket->dir_trie)) {
            return true;
        }
    }
    return false;
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

void test_main(void) {
    Trie* root = create_trie();
    state scan = (state){0};
    const char* test_path = "/tmp/alpha-1/file.txt";

    printf("\n[Trie test] inserting: %s", test_path);
    search(root, &scan, (char*)test_path);
    if (scan.running) {
        pthread_join(scan.worker, NULL);
        scan.running = false;
    }

    bool ok = trie_contains_path(root, test_path);
    printf("\n[Trie test] contains full path: %s\n", ok ? "ok" : "fail");

    printf("\n[Validation] existing file: ");
    path_validation v1 = validate_input_path("/home/sam/samdev", "archaic/main.c");
    printf("%s (is_file=%d, is_dir=%d)", v1.exists ? "ok" : "fail", v1.is_file, v1.is_dir);
    free_path_validation(&v1);

    printf("\n[Validation] existing dir: ");
    path_validation v2 = validate_input_path("/home/sam/samdev", "archaic/src");
    printf("%s (is_file=%d, is_dir=%d)", v2.exists ? "ok" : "fail", v2.is_file, v2.is_dir);
    free_path_validation(&v2);

    printf("\n[Validation] non-existent: ");
    path_validation v3 = validate_input_path("/home/sam/samdev", "nonexistent_fake_file.xyz");
    printf("%s (is_file=%d, is_dir=%d)", v3.exists ? "ok" : "fail", v3.is_file, v3.is_dir);
    free_path_validation(&v3);

    printf("\n[Validation] absolute path: ");
    path_validation v4 = validate_input_path("/home/sam/samdev", "/etc/hostname");
    printf("%s (is_file=%d, is_dir=%d)", v4.exists ? "ok" : "fail", v4.is_file, v4.is_dir);
    free_path_validation(&v4);

    t_bucket_store* store = calloc(1, sizeof(t_bucket_store));
    node* parent = calloc(1, sizeof(node));
    parent->is_parent = true;
    store->parent = parent;

    const char* base_path = test_scan_path ? test_scan_path : ".";
    char* norm_dir = normalise_dir(base_path);
    printf("\n[Scan] path: %s", norm_dir);

    file_thread* f_thread = (file_thread*) calloc(1, sizeof(file_thread));
    f_thread->lfu = store;
    f_thread->parent = parent;
    spin_scan_thread(f_thread, norm_dir);

    pthread_join(f_thread->worker, NULL);

    bool scan_ok = any_bucket_has_entries(store);
    printf("\n[Scan] trie entries added: %s\n", scan_ok ? "ok" : "fail");

    size_t dump_limit = 20;
    size_t dumped = dump_store_trie_entries(store, dump_limit);
    printf("\n[Scan] trie dump (first %zu):", dump_limit);
    if (dumped == 0) {
        printf("\n  <none>");
    }
    printf("\n");
}
