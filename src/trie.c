#include "trie.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "threadmanager.h"

/* ────────────────────────────────────────────────────────────────────────────
 * Feature 21: Git-aware scoring
 * ──────────────────────────────────────────────────────────────────────────── */

/* Cached git root to avoid repeated filesystem walks */
static char g_git_root[4096] = {0};
static pthread_mutex_t g_git_root_lock = PTHREAD_MUTEX_INITIALIZER;

static bool dir_has_git(const char* dir) {
    char git_path[4096];
    int n = snprintf(git_path, sizeof(git_path), "%s/.git", dir);
    if (n < 0 || (size_t) n >= sizeof(git_path))
        return false;
    struct stat st;
    return stat(git_path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Walk up from `path` to find a .git directory. Returns true if found and
   sets `root_out` to the directory containing .git. */
static bool find_git_root(const char* path, char* root_out, size_t root_cap) {
    char buf[4096];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip trailing filename to get directory */
    char* slash = strrchr(buf, '/');
    if (!slash)
        return false;
    *slash = '\0';

    while (buf[0] != '\0') {
        if (dir_has_git(buf)) {
            if (root_out && root_cap > 0) {
                strncpy(root_out, buf, root_cap - 1);
                root_out[root_cap - 1] = '\0';
            }
            return true;
        }
        slash = strrchr(buf, '/');
        if (!slash)
            break;
        *slash = '\0';
    }
    return false;
}

bool is_git_tracked(const char* path) {
    if (!path || path[0] == '\0')
        return false;

    /* Check cached git root first */
    pthread_mutex_lock(&g_git_root_lock);
    if (g_git_root[0] != '\0') {
        size_t root_len = strlen(g_git_root);
        if (strncmp(path, g_git_root, root_len) == 0 &&
            (path[root_len] == '/' || path[root_len] == '\0')) {
            pthread_mutex_unlock(&g_git_root_lock);
            /* Verify with git ls-files */
            char cmd[4224];
            snprintf(cmd, sizeof(cmd), "git -C \"%s\" ls-files --error-unmatch -- '%s' 2>/dev/null",
                     g_git_root, path);
            FILE* fp = popen(cmd, "r");
            if (!fp)
                return true; /* Assume tracked if git command fails */
            char line[4096];
            bool tracked = (fgets(line, sizeof(line), fp) != NULL);
            pclose(fp);
            return tracked;
        }
    }
    pthread_mutex_unlock(&g_git_root_lock);

    /* Find git root from scratch */
    char root[4096];
    if (!find_git_root(path, root, sizeof(root)))
        return false;

    /* Cache the git root */
    pthread_mutex_lock(&g_git_root_lock);
    strncpy(g_git_root, root, sizeof(g_git_root) - 1);
    g_git_root[sizeof(g_git_root) - 1] = '\0';
    pthread_mutex_unlock(&g_git_root_lock);

    /* Check if file is tracked */
    char cmd[4224];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" ls-files --error-unmatch -- '%s' 2>/dev/null", root,
             path);
    FILE* fp = popen(cmd, "r");
    if (!fp)
        return true;
    char line[4096];
    bool tracked = (fgets(line, sizeof(line), fp) != NULL);
    pclose(fp);
    return tracked;
}

/* Reset the cached git root (call when scan path changes) */
void git_root_reset(void) {
    pthread_mutex_lock(&g_git_root_lock);
    g_git_root[0] = '\0';
    pthread_mutex_unlock(&g_git_root_lock);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Feature 22: File extension relevance
 * ──────────────────────────────────────────────────────────────────────────── */

/* Project type → preferred extensions mapping */
typedef struct {
    const char* config_file;
    const char* extensions[8];
} ext_mapping;

static const ext_mapping EXT_MAPPINGS[] = {
    {"package.json", {".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs", NULL}},
    {"Cargo.toml", {".rs", ".rlib", NULL}},
    {"CMakeLists.txt", {".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", NULL}},
    {"Makefile", {".c", ".h", ".cpp", ".hpp", ".cc", ".mk", NULL}},
    {"setup.py", {".py", ".pyx", NULL}},
    {"pyproject.toml", {".py", ".pyx", NULL}},
    {"go.mod", {".go", NULL}},
    {"Gemfile", {".rb", ".rake", NULL}},
    {"pom.xml", {".java", NULL}},
    {"build.gradle", {".java", ".kt", ".kts", NULL}},
};
static const int EXT_MAPPINGS_COUNT = (int) (sizeof(EXT_MAPPINGS) / sizeof(EXT_MAPPINGS[0]));

static bool file_exists_at(const char* dir, const char* filename) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, filename);
    if (n < 0 || (size_t) n >= sizeof(path))
        return false;
    struct stat st;
    return stat(path, &st) == 0;
}

/* Get the file extension from a path (returns pointer into path or empty) */
static const char* get_extension(const char* path) {
    const char* basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    const char* dot = strrchr(basename, '.');
    if (!dot || dot == basename)
        return "";
    return dot;
}

bool is_relevant_extension(const char* path, const char* scan_root) {
    if (!path || path[0] == '\0')
        return false;

    const char* ext = get_extension(path);
    if (ext[0] == '\0')
        return false;

    /* Walk up from path to scan_root looking for config files */
    char buf[4096];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Get directory of the file */
    char* slash = strrchr(buf, '/');
    if (!slash)
        return false;
    *slash = '\0';

    size_t root_len = scan_root ? strlen(scan_root) : 0;

    while (buf[0] != '\0') {
        /* Check each config file mapping */
        for (int i = 0; i < EXT_MAPPINGS_COUNT; i++) {
            if (file_exists_at(buf, EXT_MAPPINGS[i].config_file)) {
                /* Found a config file — check if extension matches */
                for (int j = 0; EXT_MAPPINGS[i].extensions[j] != NULL; j++) {
                    if (strcmp(ext, EXT_MAPPINGS[i].extensions[j]) == 0)
                        return true;
                }
                /* Config found but extension doesn't match — not relevant */
                return false;
            }
        }

        /* Stop if we've reached the scan root */
        if (root_len > 0 && strncmp(buf, scan_root, root_len) == 0 &&
            (buf[root_len] == '/' || buf[root_len] == '\0'))
            break;

        slash = strrchr(buf, '/');
        if (!slash)
            break;
        *slash = '\0';
    }

    return false;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Feature 25: Hidden file demotion
 * ──────────────────────────────────────────────────────────────────────────── */

bool is_hidden_path(const char* path) {
    if (!path || path[0] == '\0')
        return false;

    const char* p = path;
    while (*p) {
        if (*p == '/') {
            p++;
            if (*p == '.')
                return true;
        } else {
            p++;
        }
    }
    return false;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Feature 26: Executable script detection — boost scripts with shebangs
 * and common executable extensions
 * ──────────────────────────────────────────────────────────────────────────── */

bool is_executable_script(const char* path) {
    const char* basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    static const char* const exec_exts[] = {".sh", ".bash", ".zsh", ".fish", ".py",
                                            ".rb", ".pl",   ".js",  ".ts",   ".rs",
                                            ".go", ".java", ".lua", ".vim",  NULL};
    for (int i = 0; exec_exts[i]; i++) {
        size_t elen = strlen(exec_exts[i]);
        size_t nlen = strlen(basename);
        if (nlen >= elen && strcmp(basename + nlen - elen, exec_exts[i]) == 0)
            return true;
    }

    if (basename[0] == '.' && strstr(basename, "rc") != NULL)
        return true;

    return false;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Feature 24: Session-based learning
 * ──────────────────────────────────────────────────────────────────────────── */

#define SESSION_MAX_ENTRIES 256

typedef struct {
    char path[4096];
    uint64_t select_count;
} session_entry;

static session_entry g_session_entries[SESSION_MAX_ENTRIES];
static int g_session_count = 0;
static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;

static int session_find_entry(const char* path) {
    for (int i = 0; i < g_session_count; i++) {
        if (strcmp(g_session_entries[i].path, path) == 0)
            return i;
    }
    return -1;
}

void session_record_selection(const char* path) {
    if (!path || path[0] == '\0')
        return;

    pthread_mutex_lock(&g_session_lock);

    int idx = session_find_entry(path);
    if (idx >= 0) {
        g_session_entries[idx].select_count++;
    } else if (g_session_count < SESSION_MAX_ENTRIES) {
        strncpy(g_session_entries[g_session_count].path, path,
                sizeof(g_session_entries[g_session_count].path) - 1);
        g_session_entries[g_session_count]
            .path[sizeof(g_session_entries[g_session_count].path) - 1] = '\0';
        g_session_entries[g_session_count].select_count = 1;
        g_session_count++;
    }

    pthread_mutex_unlock(&g_session_lock);
}

double session_get_boost(const char* path) {
    if (!path || path[0] == '\0')
        return 0.0;

    pthread_mutex_lock(&g_session_lock);

    double boost = 0.0;
    size_t path_len = strlen(path);

    for (int i = 0; i < g_session_count; i++) {
        const char* selected = g_session_entries[i].path;
        size_t sel_len = strlen(selected);

        /* If the current path starts with a selected path (sub-path match),
           apply boost proportional to selection count */
        if (path_len > sel_len && strncmp(path, selected, sel_len) == 0 && path[sel_len] == '/') {
            boost += 0.05 * (double) g_session_entries[i].select_count;
        }
        /* Exact match gets a larger boost */
        if (strcmp(path, selected) == 0) {
            boost += 0.10 * (double) g_session_entries[i].select_count;
        }
    }

    /* Cap the boost at 0.5 to prevent runaway scores */
    if (boost > 0.5)
        boost = 0.5;

    pthread_mutex_unlock(&g_session_lock);
    return boost;
}

void session_reset(void) {
    pthread_mutex_lock(&g_session_lock);
    g_session_count = 0;
    memset(g_session_entries, 0, sizeof(g_session_entries));
    pthread_mutex_unlock(&g_session_lock);
}

static inline RadixChild* find_child(RadixNode* node, char c) {
    int lo = 0, hi = node->child_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        char mc = node->children[mid].edge_char;
        if (mc == c)
            return &node->children[mid];
        if (mc < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static inline void add_child(RadixNode* node, char c, RadixNode* child) {
    if (node->child_count >= node->child_capacity) {
        size_t new_cap = node->child_capacity * 2;
        RadixChild* new_children = malloc(new_cap * sizeof(RadixChild));
        if (!new_children)
            return;
        memcpy(new_children, node->children, node->child_count * sizeof(RadixChild));
        if (node->children != node->inline_storage) {
            free(node->children);
        }
        node->children = new_children;
        node->child_capacity = (uint8_t) new_cap;
    }

    /* Find insertion position to maintain sorted order by edge_char */
    int lo = 0, hi = node->child_count - 1;
    int pos = node->child_count;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (node->children[mid].edge_char < c) {
            lo = mid + 1;
        } else {
            pos = mid;
            hi = mid - 1;
        }
    }

    /* Shift elements right to make room */
    if (pos < node->child_count) {
        memmove(&node->children[pos + 1], &node->children[pos],
                (node->child_count - pos) * sizeof(RadixChild));
    }

    node->children[pos].edge_char = c;
    node->children[pos].node = child;
    node->child_count++;
}

__attribute__((unused)) static void remove_child_at(RadixNode* node, uint8_t idx) {
    if (idx >= node->child_count)
        return;
    for (uint8_t i = idx; i < node->child_count - 1; i++) {
        node->children[i] = node->children[i + 1];
    }
    node->child_count--;
}

Trie* create_trie(void) {
    RadixNode* node = calloc(1, sizeof(RadixNode));
    if (!node)
        return NULL;
    node->children = node->inline_storage;
    node->child_capacity = RADIX_INLINE_CHILDREN;
    node->child_count = 0;
    node->freq = 1;
    node->is_leaf = false;
    node->key = NULL;
    node->key_len = 0;
    return node;
}

void trie_free_recursive(Trie* node) {
    if (!node)
        return;
    for (uint8_t i = 0; i < node->child_count; i++) {
        trie_free_recursive(node->children[i].node);
    }
    if (node->children != node->inline_storage) {
        free(node->children);
    }
    free(node->key);
    free(node);
}

void insert(Trie* root, const char* str) {
    if (!root || !str || str[0] == '\0')
        return;

    uint64_t now = (uint64_t) time(NULL);

    RadixNode* curr = root;
    size_t i = 0;
    size_t len = strlen(str);

    while (i < len) {
        char c = str[i];
        RadixChild* child = find_child(curr, c);

        if (!child) {
            RadixNode* new_node = calloc(1, sizeof(RadixNode));
            if (!new_node)
                return;
            new_node->key = strdup(str + i);
            new_node->key_len = len - i;
            new_node->children = new_node->inline_storage;
            new_node->child_capacity = RADIX_INLINE_CHILDREN;
            new_node->is_leaf = true;
            new_node->freq = 1;
            new_node->last_access = now;
            new_node->is_dir = (str[len - 1] == '/');
            add_child(curr, c, new_node);
            return;
        }

        RadixNode* child_node = child->node;
        size_t match = 0;
        size_t edge_len = child_node->key_len;

        while (match < edge_len && i + match < len && child_node->key[match] == str[i + match]) {
            match++;
        }

        if (match == edge_len) {
            curr = child_node;
            curr->freq++;
            curr->last_access = now;
            i += match;

            if (i == len) {
                curr->is_leaf = true;
                return;
            }
        } else if (match == 0) {
            RadixNode* new_node = calloc(1, sizeof(RadixNode));
            if (!new_node)
                return;
            new_node->key = strdup(str + i);
            new_node->key_len = len - i;
            new_node->children = new_node->inline_storage;
            new_node->child_capacity = RADIX_INLINE_CHILDREN;
            new_node->is_leaf = true;
            new_node->freq = 1;
            new_node->last_access = now;
            new_node->is_dir = (str[len - 1] == '/');
            add_child(curr, str[i], new_node);
            return;
        } else {
            RadixNode* split = calloc(1, sizeof(RadixNode));
            if (!split)
                return;
            split->key = strndup(child_node->key, match);
            split->key_len = match;
            split->children = split->inline_storage;
            split->child_capacity = RADIX_INLINE_CHILDREN;
            split->is_leaf = false;
            split->freq = child_node->freq;
            split->last_access = child_node->last_access;
            split->is_dir = false;

            memmove(child_node->key, child_node->key + match, child_node->key_len - match + 1);
            child_node->key_len -= match;

            uint8_t child_idx = 0;
            for (; child_idx < curr->child_count; child_idx++) {
                if (&curr->children[child_idx] == child)
                    break;
            }
            curr->children[child_idx].edge_char = split->key[0];
            curr->children[child_idx].node = split;

            add_child(split, child_node->key[0], child_node);

            if (i + match < len) {
                RadixNode* new_node = calloc(1, sizeof(RadixNode));
                if (!new_node)
                    return;
                new_node->key = strdup(str + i + match);
                new_node->key_len = len - i - match;
                new_node->children = new_node->inline_storage;
                new_node->child_capacity = RADIX_INLINE_CHILDREN;
                new_node->is_leaf = true;
                new_node->freq = 1;
                new_node->last_access = now;
                new_node->is_dir = (str[len - 1] == '/');
                add_child(split, str[i + match], new_node);
            }
            return;
        }
    }
}

Trie* search(Trie* root, state* scan, char* str) {
    (void) scan;
    if (!root || !str || str[0] == '\0')
        return NULL;

    uint64_t now = (uint64_t) time(NULL);

    RadixNode* curr = root;
    size_t i = 0;
    size_t len = strlen(str);

    while (i < len) {
        RadixChild* child = find_child(curr, str[i]);
        if (!child)
            return NULL;

        RadixNode* child_node = child->node;
        size_t edge_len = child_node->key_len;

        if (i + edge_len > len)
            return NULL;

        if (memcmp(child_node->key, str + i, edge_len) != 0) {
            return NULL;
        }

        curr = child_node;
        curr->freq++;
        curr->last_access = now;
        i += edge_len;
    }

    return curr;
}

/*
   Completion collection
*/

completions* completions_create(size_t capacity) {
    completions* c = calloc(1, sizeof(completions));
    if (!c)
        return NULL;
    c->paths = calloc(capacity, sizeof(char*));
    if (!c->paths) {
        free(c);
        return NULL;
    }
    c->is_dirs = calloc(capacity, sizeof(bool));
    if (!c->is_dirs) {
        free(c->paths);
        free(c);
        return NULL;
    }
    c->capacity = capacity;
    c->count = 0;
    return c;
}

void completions_free(completions* c) {
    if (!c)
        return;
    for (size_t i = 0; i < c->count; i++) {
        free(c->paths[i]);
    }
    free(c->paths);
    free(c->is_dirs);
    free(c);
}

static RadixNode* find_prefix_node(Trie* root, const char* prefix, size_t* out_matched_in_node) {
    if (!root || !prefix || prefix[0] == '\0') {
        if (out_matched_in_node)
            *out_matched_in_node = 0;
        return root;
    }

    RadixNode* curr = root;
    size_t i = 0;
    size_t len = strlen(prefix);

    while (i < len) {
        RadixChild* child = find_child(curr, prefix[i]);
        if (!child) {
            if (out_matched_in_node)
                *out_matched_in_node = 0;
            return NULL;
        }

        RadixNode* child_node = child->node;
        size_t edge_len = child_node->key_len;

        if (i + edge_len > len) {
            size_t match_in_key = len - i;
            if (memcmp(child_node->key, prefix + i, match_in_key) == 0) {
                if (out_matched_in_node)
                    *out_matched_in_node = match_in_key;
                return child_node;
            }
            if (out_matched_in_node)
                *out_matched_in_node = 0;
            return NULL;
        }

        if (memcmp(child_node->key, prefix + i, edge_len) != 0) {
            if (out_matched_in_node)
                *out_matched_in_node = 0;
            return NULL;
        }

        curr = child_node;
        i += edge_len;
    }

    if (out_matched_in_node)
        *out_matched_in_node = 0;
    return curr;
}

static void collect_dfs(RadixNode* node, char* buffer, size_t depth, const char* prefix,
                        completions* out) {
    if (!node || out->count >= out->capacity)
        return;

    if (node->is_leaf && depth > 0) {
        buffer[depth] = '\0';
        size_t plen = strlen(prefix);
        size_t slen = strlen(buffer);
        char* full = malloc(plen + slen + 1);
        if (full && out->count < out->capacity) {
            memcpy(full, prefix, plen);
            memcpy(full + plen, buffer, slen + 1);
            out->paths[out->count] = full;
            out->is_dirs[out->count] = node->is_dir;
            out->count++;
        }
    }

    for (uint8_t i = 0; i < node->child_count; i++) {
        RadixChild* child = &node->children[i];
        RadixNode* child_node = child->node;
        size_t klen = child_node->key_len;

        if (depth + klen >= 2048)
            continue;

        memcpy(buffer + depth, child_node->key, klen);
        collect_dfs(child_node, buffer, depth + klen, prefix, out);
        if (out->count >= out->capacity)
            return;
    }
}

void completions_collect(Trie* root, const char* prefix, completions* out) {
    if (!root || !out)
        return;

    size_t matched_in_node = 0;
    RadixNode* node = find_prefix_node(root, prefix, &matched_in_node);
    if (!node)
        return;

    char buffer[2048];
    size_t depth = 0;

    /* Skip if prefix ends with / - user wants contents, not the dir itself */
    size_t prefix_len = strlen(prefix);
    int prefix_is_dir = (prefix_len > 0 && prefix[prefix_len - 1] == '/') ? 1 : 0;

    if (node->is_leaf && matched_in_node == 0 && !prefix_is_dir) {
        size_t plen = strlen(prefix);
        char* full = malloc(plen + 1);
        if (full && out->count < out->capacity) {
            memcpy(full, prefix, plen + 1);
            out->paths[out->count] = full;
            out->is_dirs[out->count] = node->is_dir;
            out->count++;
        }
    }

    if (node != root && node->key && node->key_len > 0 && matched_in_node > 0) {
        size_t remaining_key = node->key_len - matched_in_node;
        if (remaining_key > 0 && remaining_key < 2048) {
            memcpy(buffer, node->key + matched_in_node, remaining_key);
            depth = remaining_key;

            if (node->is_leaf && !prefix_is_dir) {
                buffer[depth] = '\0';
                size_t plen = strlen(prefix);
                char* full = malloc(plen + depth + 1);
                if (full && out->count < out->capacity) {
                    memcpy(full, prefix, plen);
                    memcpy(full + plen, buffer, depth + 1);
                    out->paths[out->count] = full;
                    out->is_dirs[out->count] = node->is_dir;
                    out->count++;
                }
            }
        }
    }

    collect_dfs(node, buffer, depth, prefix, out);
}

/*
   Scored completion collection
*/

scored_completions* scored_completions_create(size_t capacity) {
    scored_completions* sc = calloc(1, sizeof(scored_completions));
    if (!sc)
        return NULL;
    sc->entries = calloc(capacity, sizeof(scored_entry));
    if (!sc->entries) {
        free(sc);
        return NULL;
    }
    sc->capacity = capacity;
    sc->count = 0;
    return sc;
}

void scored_completions_free(scored_completions* sc) {
    if (!sc)
        return;
    free(sc->entries);
    free(sc);
}

static inline int path_depth(const char* path) {
    int d = 0;
    for (const char* p = path; *p; p++) {
        if (*p == '/')
            d++;
    }
    return d;
}

static inline double clampd(double val, double lo, double hi) {
    if (val < lo)
        return lo;
    if (val > hi)
        return hi;
    return val;
}

static double compute_score(const char* path, uint64_t freq, uint64_t last_access, bool is_dir,
                            uint64_t now, int max_depth, const char* cwd) {
    double score = 0.0;

    /* Frequency: normalize to 0-1 range using log scale */
    double freq_norm = (freq > 0) ? (1.0 - 1.0 / (1.0 + (double) freq)) : 0.0;
    freq_norm = clampd(freq_norm, 0.0, 1.0);
    score += SCORE_WEIGHT_FREQ * freq_norm;

    /* Recency: true exponential decay with 1-hour half-life */
    double recency_norm = 0.0;
    if (last_access > 0) {
        if (now > last_access) {
            double age = (double) (now - last_access);
            recency_norm = pow(0.5, age / 3600.0);
        } else {
            recency_norm = 1.0;
        }
    }
    recency_norm = clampd(recency_norm, 0.0, 1.0);
    score += SCORE_WEIGHT_RECENCY * recency_norm;

    /* Depth: shallower paths rank higher */
    int depth = path_depth(path);
    double depth_norm = (max_depth > 0) ? (1.0 - (double) depth / (double) max_depth) : 0.5;
    depth_norm = clampd(depth_norm, 0.0, 1.0);
    score += SCORE_WEIGHT_DEPTH * depth_norm;

    /* Feature 23: Directory depth penalty — 5% reduction per level beyond 5 */
    if (depth > 5) {
        int excess = depth - 5;
        double penalty = 1.0 - (0.05 * (double) excess);
        if (penalty < 0.1)
            penalty = 0.1;
        score *= penalty;
    }

    /* Type: files rank above directories */
    score += SCORE_WEIGHT_TYPE * (is_dir ? 0.0 : 1.0);

    /* CWD proximity: paths closer to current working directory rank higher */
    if (cwd && cwd[0] != '\0') {
        size_t cwd_len = strlen(cwd);
        size_t path_len = strlen(path);
        if (path_len >= cwd_len && strncmp(path, cwd, cwd_len) == 0) {
            int extra_dirs = 0;
            for (size_t i = cwd_len; i < path_len; i++) {
                if (path[i] == '/')
                    extra_dirs++;
            }
            double cwd_norm = 1.0 / (1.0 + extra_dirs);
            score += SCORE_WEIGHT_CWD * clampd(cwd_norm, 0.0, 1.0);
        }
    }

    /* Feature 21: Git-aware scoring — boost files in git-tracked repos */
    if (is_git_tracked(path)) {
        score *= 1.15;
    }

    /* Feature 22: File extension relevance — boost by 20% for project-relevant extensions */
    if (is_relevant_extension(path, cwd)) {
        score *= 1.20;
    }

    /* Feature 26: Executable script boost */
    if (!is_dir && is_executable_script(path)) {
        score *= 1.10;
    }

    /* Feature 24: Session-based learning boost */
    double sess_boost = session_get_boost(path);
    if (sess_boost > 0.0) {
        score += sess_boost;
    }

    /* Recent file bonus: slight boost for paths accessed in the last 24 hours */
    if (last_access > 0 && now > last_access) {
        double age_hours = (double) (now - last_access) / 3600.0;
        if (age_hours < 24.0) {
            double recent_bonus = (24.0 - age_hours) / 24.0 * 0.05;
            score += recent_bonus;
        }
    }

    return score;
}

static int cmp_path(const void* a, const void* b) {
    return strcmp(((const scored_entry*) a)->path, ((const scored_entry*) b)->path);
}

static void scored_insert(scored_completions* sc, const char* path, double score, uint64_t freq,
                          uint64_t last_access, bool is_dir) {
    /* Binary search for duplicate */
    scored_entry key;
    memset(&key, 0, sizeof(key));
    strncpy(key.path, path, sizeof(key.path) - 1);
    key.path[sizeof(key.path) - 1] = '\0';
    scored_entry* found = bsearch(&key, sc->entries, sc->count, sizeof(scored_entry), cmp_path);
    if (found)
        return;

    if (sc->count < sc->capacity) {
        scored_entry* e = &sc->entries[sc->count];
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        e->score = score;
        e->freq = freq;
        e->last_access = last_access;
        e->is_dir = is_dir;
        sc->count++;
        size_t idx = sc->count - 1;
        while (idx > 0 && cmp_path(&sc->entries[idx - 1], &sc->entries[idx]) > 0) {
            scored_entry tmp = sc->entries[idx - 1];
            sc->entries[idx - 1] = sc->entries[idx];
            sc->entries[idx] = tmp;
            idx--;
        }
    } else if (score > sc->entries[0].score) {
        scored_entry* e = &sc->entries[0];
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        e->score = score;
        e->freq = freq;
        e->last_access = last_access;
        e->is_dir = is_dir;
        qsort(sc->entries, sc->count, sizeof(scored_entry), cmp_path);
    }
}

typedef struct {
    char buffer[2048];
    size_t depth;
    const char* prefix;
    size_t prefix_len;
    scored_completions* out;
    uint64_t now;
    int max_depth;
    const char* cwd;
} scored_dfs_ctx;

static void scored_collect_dfs(RadixNode* node, scored_dfs_ctx* ctx) {
    if (!node || ctx->out->count >= ctx->out->capacity * 2)
        return;

    if (node->is_leaf && ctx->depth > 0) {
        ctx->buffer[ctx->depth] = '\0';
        size_t plen = ctx->prefix_len;
        if (plen + ctx->depth < 4096) {
            char full[4096];
            memcpy(full, ctx->prefix, plen);
            memcpy(full + plen, ctx->buffer, ctx->depth + 1);

            double score = compute_score(full, node->freq, node->last_access, node->is_dir,
                                         ctx->now, ctx->max_depth, ctx->cwd);
            scored_insert(ctx->out, full, score, node->freq, node->last_access, node->is_dir);
        }
    }

    for (uint8_t i = 0; i < node->child_count; i++) {
        RadixChild* child = &node->children[i];
        RadixNode* child_node = child->node;
        size_t klen = child_node->key_len;

        if (ctx->depth + klen >= 2048)
            continue;

        memcpy(ctx->buffer + ctx->depth, child_node->key, klen);
        size_t prev_depth = ctx->depth;
        ctx->depth += klen;
        scored_collect_dfs(child_node, ctx);
        ctx->depth = prev_depth;
    }
}

static int cmp_score_desc(const void* a, const void* b) {
    const scored_entry* ea = (const scored_entry*) a;
    const scored_entry* eb = (const scored_entry*) b;
    if (eb->score > ea->score)
        return 1;
    if (eb->score < ea->score)
        return -1;
    return strcmp(ea->path, eb->path);
}

void scored_completions_collect(Trie* root, const char* prefix, scored_completions* out,
                                uint64_t now, const char* cwd) {
    if (!root || !out)
        return;

    size_t matched_in_node = 0;
    RadixNode* node = find_prefix_node(root, prefix, &matched_in_node);
    if (!node)
        return;

    scored_dfs_ctx ctx;
    ctx.depth = 0;
    ctx.prefix = prefix;
    ctx.prefix_len = strlen(prefix);
    ctx.out = out;
    ctx.now = now;
    ctx.max_depth = 0;
    ctx.cwd = cwd;

    ctx.max_depth = path_depth(prefix) + 10;

    size_t prefix_len = strlen(prefix);
    int prefix_is_dir = (prefix_len > 0 && prefix[prefix_len - 1] == '/') ? 1 : 0;
    if (node->is_leaf && matched_in_node == 0 && !prefix_is_dir) {
        double score = compute_score(prefix, node->freq, node->last_access, node->is_dir, now,
                                     ctx.max_depth, cwd);
        scored_insert(out, prefix, score, node->freq, node->last_access, node->is_dir);
    }

    if (node != root && node->key && node->key_len > 0 && matched_in_node > 0) {
        size_t remaining_key = node->key_len - matched_in_node;
        if (remaining_key > 0 && remaining_key < 2048) {
            memcpy(ctx.buffer, node->key + matched_in_node, remaining_key);
            ctx.depth = remaining_key;

            if (node->is_leaf && !prefix_is_dir) {
                ctx.buffer[ctx.depth] = '\0';
                size_t plen = strlen(prefix);
                if (plen + ctx.depth < 4096) {
                    char full[4096];
                    memcpy(full, prefix, plen);
                    memcpy(full + plen, ctx.buffer, ctx.depth + 1);
                    double score = compute_score(full, node->freq, node->last_access, node->is_dir,
                                                 now, ctx.max_depth, cwd);
                    scored_insert(out, full, score, node->freq, node->last_access, node->is_dir);
                }
            }
        }
    }

    scored_collect_dfs(node, &ctx);

    qsort(out->entries, out->count, sizeof(scored_entry), cmp_score_desc);

    /* Feature 25: Hidden file demotion — push dotfiles/dotdirs to end unless
       user explicitly typed a dot in the last path component */
    int user_typed_dot = 0;
    if (prefix_len > 0) {
        const char* last_slash = strrchr(prefix, '/');
        const char* last_component = last_slash ? last_slash + 1 : prefix;
        if (last_component[0] == '.')
            user_typed_dot = 1;
    }
    if (!user_typed_dot && out->count > 1) {
        size_t write = 0;
        for (size_t read = 0; read < out->count; read++) {
            if (!is_hidden_path(out->entries[read].path)) {
                if (read != write) {
                    scored_entry tmp = out->entries[read];
                    out->entries[read] = out->entries[write];
                    out->entries[write] = tmp;
                }
                write++;
            }
        }
    }
}

/*
   Fuzzy matching: subsequence match against all leaf paths
*/

typedef struct {
    char path[4096];
    int match_quality;
    uint64_t freq;
    uint64_t last_access;
    bool is_dir;
} fuzzy_entry;

/* ── Levenshtein distance with early termination ─────────────────────── */

static int levenshtein_distance(const char* s, const char* t, int max_dist) {
    int s_len = (int) strlen(s);
    int t_len = (int) strlen(t);

    if (abs(s_len - t_len) > max_dist)
        return max_dist + 1;

    int* prev = malloc((size_t) (t_len + 1) * sizeof(int));
    int* curr = malloc((size_t) (t_len + 1) * sizeof(int));
    if (!prev || !curr) {
        free(prev);
        free(curr);
        return max_dist + 1;
    }

    for (int j = 0; j <= t_len; j++)
        prev[j] = j;

    for (int i = 1; i <= s_len; i++) {
        curr[0] = i;
        int row_min = i;
        for (int j = 1; j <= t_len; j++) {
            int cost = (s[i - 1] == t[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            curr[j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
            if (curr[j] < row_min)
                row_min = curr[j];
        }
        if (row_min > max_dist) {
            free(prev);
            free(curr);
            return max_dist + 1;
        }
        int* tmp = prev;
        prev = curr;
        curr = tmp;
    }

    int result = prev[t_len];
    free(prev);
    free(curr);
    return result;
}

/* ── Path segment tokenization scoring ───────────────────────────────── */

static int segment_token_score(const char* path, const char* query) {
    int path_len = (int) strlen(path);
    int query_len = (int) strlen(query);
    if (query_len == 0 || path_len == 0)
        return -1;

    /* Extract segments and build acronym from first chars */
    char acronym[256];
    int acronym_len = 0;

    /* First char of path is always in acronym */
    if (acronym_len < (int) sizeof(acronym) - 1)
        acronym[acronym_len++] = (char) tolower((unsigned char) path[0]);

    for (int i = 1; i < path_len; i++) {
        if (path[i] == '/' || path[i] == '\\' || path[i] == '_' || path[i] == '-' ||
            path[i] == '.') {
            /* Next non-separator char is a segment start */
            int j = i + 1;
            while (j < path_len && (path[j] == '/' || path[j] == '\\' || path[j] == '_' ||
                                    path[j] == '-' || path[j] == '.'))
                j++;
            if (j < path_len && acronym_len < (int) sizeof(acronym) - 1)
                acronym[acronym_len++] = (char) tolower((unsigned char) path[j]);
        }
    }
    acronym[acronym_len] = '\0';

    /* Try exact acronym match */
    if (acronym_len == query_len) {
        int match = 1;
        for (int i = 0; i < query_len; i++) {
            if (tolower((unsigned char) acronym[i]) != tolower((unsigned char) query[i])) {
                match = 0;
                break;
            }
        }
        if (match)
            return 100 + query_len * 10;
    }

    /* Try prefix-of-segment matching: query chars match start of each segment */
    if (query_len <= acronym_len) {
        int score = 0;
        int qi = 0;
        for (int si = 0; si < acronym_len && qi < query_len; si++) {
            if (tolower((unsigned char) acronym[si]) == tolower((unsigned char) query[qi])) {
                score += 10 + (si == 0 ? 5 : 0);
                qi++;
            }
        }
        if (qi == query_len)
            return score;
    }

    /* Try matching query as prefix-of-consecutive-segments */
    int best_segment_score = -1;
    for (int start = 0; start < path_len; start++) {
        /* Must start at path beginning or after separator */
        if (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\' &&
            path[start - 1] != '_' && path[start - 1] != '-' && path[start - 1] != '.')
            continue;

        int match_count = 0;
        int qi = 0;
        for (int pi = start; pi < path_len && qi < query_len; pi++) {
            if (tolower((unsigned char) path[pi]) == tolower((unsigned char) query[qi])) {
                match_count++;
                qi++;
                /* Bonus for matching right after a separator */
                if (pi > 0 && (path[pi - 1] == '/' || path[pi - 1] == '\\' || path[pi - 1] == '_' ||
                               path[pi - 1] == '-' || path[pi - 1] == '.'))
                    match_count += 2;
            }
        }
        if (qi == query_len && match_count > best_segment_score)
            best_segment_score = match_count * 5;
    }

    return best_segment_score;
}

/* ── Path-boundary bonus for fuzzy matching ──────────────────────────── */

#define FUZZY_BONUS_PATH_BOUNDARY 6
#define FUZZY_BONUS_WORD_BOUNDARY 4
#define FUZZY_BONUS_CAMEL_CASE 3
#define FUZZY_BONUS_CONSECUTIVE 3
#define FUZZY_SCORE_MATCH 10
#define FUZZY_SCORE_GAP_START -3
#define FUZZY_SCORE_GAP_EXTENSION -1

static int path_char_class(char c) {
    if (c == '/' || c == '\\')
        return 0; /* path separator */
    if (c == '-' || c == '_' || c == '.' || c == ' ')
        return 1; /* word separator */
    if (c >= 'A' && c <= 'Z')
        return 2; /* uppercase */
    if (c >= 'a' && c <= 'z')
        return 3; /* lowercase */
    if (c >= '0' && c <= '9')
        return 4; /* digit */
    return 5;     /* other */
}

static int path_bonus_at(const char* path, int pos) {
    if (pos == 0)
        return FUZZY_BONUS_PATH_BOUNDARY;
    int prev_class = path_char_class(path[pos - 1]);
    int curr_class = path_char_class(path[pos]);
    if (prev_class == 0)
        return FUZZY_BONUS_PATH_BOUNDARY;
    if (prev_class == 1)
        return FUZZY_BONUS_WORD_BOUNDARY;
    if (prev_class == 3 && curr_class == 2)
        return FUZZY_BONUS_CAMEL_CASE;
    return 0;
}

/* ── Enhanced fuzzy scoring combining strategies ──────────────────────── */

static int enhanced_fuzzy_score(const char* path, const char* query) {
    int path_len = (int) strlen(path);
    int query_len = (int) strlen(query);
    if (query_len == 0 || path_len < query_len)
        return -1;

    /* 1. Exact prefix match (highest priority) */
    if (strncasecmp(path, query, (size_t) query_len) == 0)
        return 1000 + query_len;

    /* 2. Segment tokenization match */
    int seg_score = segment_token_score(path, query);
    if (seg_score > 0)
        return seg_score + 100; /* High but below exact */

    /* 3. Subsequence match with path-boundary bonuses (improved fzy-style) */
    const char* basename = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/')
            basename = p + 1;
    }
    int bn_len = (int) strlen(basename);

    /* First pass: compute subsequence with boundary bonuses */
    int score = 0;
    int qi = 0;
    int prev_match_pos = -1;
    int consecutive = 0;

    for (int bi = 0; bi < bn_len && qi < query_len; bi++) {
        if (tolower((unsigned char) basename[bi]) == tolower((unsigned char) query[qi])) {
            int bonus = path_bonus_at(basename, bi);
            if (prev_match_pos >= 0) {
                int gap = bi - prev_match_pos - 1;
                if (gap > 0)
                    score += FUZZY_SCORE_GAP_START + gap * FUZZY_SCORE_GAP_EXTENSION;
            }
            score += FUZZY_SCORE_MATCH + bonus;
            if (consecutive >= 2)
                score += FUZZY_SCORE_GAP_START;
            consecutive++;
            prev_match_pos = bi;
            qi++;
        } else {
            consecutive = 0;
        }
    }

    if (qi == query_len)
        return score;

    /* 4. Levenshtein distance on basename only (typo tolerance) */
    int basename_dist = levenshtein_distance(query, basename, 3);
    if (basename_dist > 0 && basename_dist <= 3)
        return (4 - basename_dist) * 5 + 10;

    /* 5. Levenshtein distance on full path */
    int path_dist = levenshtein_distance(query, path, 3);
    if (path_dist > 0 && path_dist <= 3)
        return (4 - path_dist) * 3 + 5;

    return -1;
}

static int fuzzy_score(const char* path, const char* query) {
    return enhanced_fuzzy_score(path, query);
}

static void fuzzy_collect_dfs(RadixNode* node, char* buffer, size_t depth, const char* query,
                              fuzzy_entry* entries, size_t* count, size_t capacity,
                              int* min_score) {
    if (!node || *count >= capacity * 2)
        return;

    if (node->is_leaf && depth > 0) {
        buffer[depth] = '\0';
        int score = fuzzy_score(buffer, query);
        if (score >= 0 && score >= *min_score) {
            if (*count < capacity) {
                fuzzy_entry* e = &entries[*count];
                strncpy(e->path, buffer, sizeof(e->path) - 1);
                e->path[sizeof(e->path) - 1] = '\0';
                e->match_quality = score;
                e->freq = node->freq;
                e->last_access = node->last_access;
                e->is_dir = node->is_dir;
                (*count)++;
            }
            if (*count >= capacity) {
                *min_score = entries[0].match_quality;
            }
        }
    }

    for (uint8_t i = 0; i < node->child_count; i++) {
        RadixChild* child = &node->children[i];
        RadixNode* child_node = child->node;
        size_t klen = child_node->key_len;
        if (depth + klen >= 2048)
            continue;
        memcpy(buffer + depth, child_node->key, klen);
        fuzzy_collect_dfs(child_node, buffer, depth + klen, query, entries, count, capacity,
                          min_score);
    }
}

static int cmp_fuzzy(const void* a, const void* b) {
    const fuzzy_entry* ea = (const fuzzy_entry*) a;
    const fuzzy_entry* eb = (const fuzzy_entry*) b;
    if (eb->match_quality != ea->match_quality)
        return eb->match_quality - ea->match_quality;
    return strcmp(ea->path, eb->path);
}

int trie_fuzzy_collect(Trie* root, const char* query, char** paths, bool* is_dirs, int capacity) {
    if (!root || !query || query[0] == '\0' || !paths || capacity <= 0)
        return 0;

    fuzzy_entry* entries = calloc((size_t) capacity, sizeof(fuzzy_entry));
    if (!entries)
        return 0;

    size_t count = 0;
    int min_score = 0;
    char buffer[2048];

    fuzzy_collect_dfs(root, buffer, 0, query, entries, &count, (size_t) capacity, &min_score);

    qsort(entries, count, sizeof(fuzzy_entry), cmp_fuzzy);

    int n = (int) (count < (size_t) capacity ? count : (size_t) capacity);
    for (int i = 0; i < n; i++) {
        paths[i] = strdup(entries[i].path);
        if (!paths[i]) {
            for (int j = 0; j < i; j++)
                free(paths[j]);
            free(entries);
            return i;
        }
        if (is_dirs)
            is_dirs[i] = entries[i].is_dir;
    }

    free(entries);
    return n;
}

size_t trie_node_count(Trie* root) {
    if (!root)
        return 0;
    size_t count = 1;
    for (uint8_t i = 0; i < root->child_count; i++) {
        count += trie_node_count(root->children[i].node);
    }
    return count;
}

static void compact_node(RadixNode* node) {
    if (!node)
        return;

    /* Recurse into children first */
    for (uint8_t i = 0; i < node->child_count; i++) {
        compact_node(node->children[i].node);
    }

    /* Merge single-child non-leaf nodes */
    if (node->child_count == 1 && !node->is_leaf && node->key != NULL) {
        RadixChild* child = &node->children[0];
        RadixNode* child_node = child->node;

        size_t new_key_len = node->key_len + child_node->key_len;
        char* new_key = malloc(new_key_len + 1);
        if (!new_key)
            return;

        memcpy(new_key, node->key, node->key_len);
        memcpy(new_key + node->key_len, child_node->key, child_node->key_len);
        new_key[new_key_len] = '\0';

        free(node->key);
        node->key = new_key;
        node->key_len = new_key_len;

        /* Adopt child's children */
        if (child_node->child_count > 0) {
            if (node->child_capacity < child_node->child_count) {
                size_t new_cap = child_node->child_count;
                if (new_cap < RADIX_INLINE_CHILDREN)
                    new_cap = RADIX_INLINE_CHILDREN;
                RadixChild* new_children = malloc(new_cap * sizeof(RadixChild));
                if (!new_children)
                    return;
                if (node->children != node->inline_storage)
                    free(node->children);
                node->children = new_children;
                node->child_capacity = (uint8_t) new_cap;
            }
            memcpy(node->children, child_node->children,
                   child_node->child_count * sizeof(RadixChild));
            node->child_count = child_node->child_count;
        } else {
            node->child_count = 0;
        }

        /* Preserve leaf flag and metadata from child if it was a leaf */
        if (child_node->is_leaf) {
            node->is_leaf = true;
            node->is_dir = child_node->is_dir;
        }

        /* Free the merged child */
        if (child_node->children != child_node->inline_storage)
            free(child_node->children);
        free(child_node->key);
        free(child_node);
    }
}

void trie_compact(Trie* root) {
    if (!root)
        return;
    /* Compact children of root, but never compact root itself */
    for (uint8_t i = 0; i < root->child_count; i++) {
        compact_node(root->children[i].node);
    }
}
