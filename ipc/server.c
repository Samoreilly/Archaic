#include "server.h"
#include "../src/io/fileloader.h"
#include "../src/metrics.h"
#include "../src/path-utils.h"
#include "../src/threadpool.h"
#include "../src/trie.h"
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEDUP_CAPACITY 256
#define DEDUP_EMPTY 0ULL

typedef struct {
    uint64_t hashes[DEDUP_CAPACITY];
    char* entries[DEDUP_CAPACITY];
    int count;
} dedup_set;

static uint64_t dedup_hash(const char* str) {
    /* FNV-1a 64-bit: far lower collision rate than 32-bit, plus we
     * strcmp on hash match so collisions can never drop results. */
    uint64_t h = 14695981039346656037ULL;
    for (const char* p = str; *p; p++) {
        h ^= (uint64_t) (unsigned char) *p;
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

static void dedup_init(dedup_set* ds) {
    memset(ds->hashes, 0, sizeof(ds->hashes));
    memset(ds->entries, 0, sizeof(ds->entries));
    ds->count = 0;
}

static void dedup_free(dedup_set* ds) {
    if (!ds)
        return;
    for (int i = 0; i < DEDUP_CAPACITY; i++) {
        free(ds->entries[i]);
        ds->entries[i] = NULL;
        ds->hashes[i] = DEDUP_EMPTY;
    }
    ds->count = 0;
}

static int dedup_contains(dedup_set* ds, const char* path) {
    uint64_t h = dedup_hash(path);
    uint32_t idx = (uint32_t) (h % DEDUP_CAPACITY);
    for (uint32_t i = 0; i < DEDUP_CAPACITY; i++) {
        uint32_t pos = (idx + i) % DEDUP_CAPACITY;
        if (ds->hashes[pos] == DEDUP_EMPTY)
            return 0;
        if (ds->hashes[pos] == h) {
            /* Hash match: confirm with strcmp so a collision never
             * causes a false dedup drop. NULL entry = hash-only
             * fallback from a failed strdup (still dedup on hash). */
            if (!ds->entries[pos] || strcmp(ds->entries[pos], path) == 0)
                return 1;
            /* Hash collision with different string: keep probing. */
        }
    }
    return 0;
}

static void dedup_insert(dedup_set* ds, const char* path) {
    if (ds->count >= DEDUP_CAPACITY)
        return;
    uint64_t h = dedup_hash(path);
    uint32_t idx = (uint32_t) (h % DEDUP_CAPACITY);
    for (uint32_t i = 0; i < DEDUP_CAPACITY; i++) {
        uint32_t pos = (idx + i) % DEDUP_CAPACITY;
        if (ds->hashes[pos] == DEDUP_EMPTY) {
            ds->hashes[pos] = h;
            ds->entries[pos] = strdup(path);
            /* If strdup fails, keep hash-only entry: degrades to
             * hash dedup rather than failing the whole request. */
            ds->count++;
            return;
        }
        if (ds->hashes[pos] == h && ds->entries[pos] && strcmp(ds->entries[pos], path) == 0)
            return; /* already present */
    }
}

__attribute__((unused)) static int cmp_recent_desc(const void* a, const void* b) {
    const scored_entry* ea = (const scored_entry*) a;
    const scored_entry* eb = (const scored_entry*) b;
    if (eb->last_access > ea->last_access)
        return 1;
    if (eb->last_access < ea->last_access)
        return -1;
    return strcmp(ea->path, eb->path);
}

struct ipc_server {
    daemon_state* daemon;
    int listen_fd;
    pthread_t worker;
    atomic_bool running;
    char sock_path[4096];
    threadpool* pool;
    struct timespec start_time;
    atomic_int active_connections;
};

typedef struct {
    ipc_server* srv;
    int fd;
} client_ctx;

static int read_exact(int fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char*) buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        total += (size_t) n;
    }
    return 0;
}

static int write_exact(int fd, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, (const char*) buf + total, len - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        total += (size_t) n;
    }
    return 0;
}

static int validate_string_field(const char* str, size_t max_len) {
    if (!str)
        return -1;
    for (size_t i = 0; i < max_len; i++) {
        if (str[i] == '\0')
            return 0;
    }
    return -1;
}

/* ── Reusable per-thread pack buffer (avoids malloc(256K) per Tab) ──
 * malloc(256KB) per COMPLETE/FUZZY goes through mmap + page faults.
 * Threadpool workers are long-lived, so keep one buffer per thread and
 * reuse it. Fallback to malloc if the first alloc fails. */
static uint8_t* packed_buffer_reuse(bool* owned) {
    static __thread uint8_t* buf = NULL;
    if (buf) {
        if (owned)
            *owned = false;
        return buf;
    }
    buf = malloc(IPC_MAX_PAYLOAD);
    if (buf) {
        if (owned)
            *owned = false;
        return buf;
    }
    uint8_t* heap = malloc(IPC_MAX_PAYLOAD);
    if (owned)
        *owned = true;
    return heap;
}

/* ── Filesystem fallback: index must never veto the filesystem ──────
 * If the trie has no results (stale index, outside scan roots, ignored
 * dir, new file before rescan), list the parent directory directly.
 * Bounded three ways: single opendir + readdir, no recursion, stops once
 * `want` is reached, caps directory scan to avoid huge-dir stalls, and a
 * wall-clock budget so hung mounts (NFS/Lustre) can't wedge a threadpool
 * worker past the client's 400ms Tab timeout. Uses d_type to avoid
 * stat() per entry except symlinks/unknown. */
#define FS_FALLBACK_MAX_SCAN 5000
#define FS_FALLBACK_BUDGET_MS 150
static void fs_fallback_fill(const char* expanded_prefix, int explicit_slash, int dirs_only,
                             int typed_dot, uint32_t want, uint32_t* out_idx,
                             uint32_t* packed_count, uint8_t* packed, size_t* pack_pos,
                             dedup_set* seen) {
    if (!expanded_prefix || expanded_prefix[0] == '\0')
        return;
    if (!out_idx || !packed_count || !packed || !pack_pos || !seen)
        return;
    if (*out_idx >= want)
        return;

    /* Split into parent dir + basename prefix. expanded_prefix is absolute
     * and normalized (no trailing slash); explicit_slash recovers whether
     * the user typed one, in which case we list the dir's children. */
    char dir[4096];
    const char* base = "";
    if (explicit_slash) {
        if (strlen(expanded_prefix) >= sizeof(dir))
            return;
        strcpy(dir, expanded_prefix[0] ? expanded_prefix : "/");
        base = "";
    } else {
        size_t elen = strlen(expanded_prefix);
        /* Strip trailing slashes for split (keep root "/"). */
        size_t trim = elen;
        while (trim > 1 && expanded_prefix[trim - 1] == '/')
            trim--;
        const char* last_slash = NULL;
        for (size_t i = 0; i < trim; i++) {
            if (expanded_prefix[i] == '/')
                last_slash = expanded_prefix + i;
        }
        if (!last_slash) {
            return; /* should not happen for absolute paths */
        }
        if (elen > 0 && expanded_prefix[elen - 1] == '/') {
            /* "…/dir/" → list dir itself */
            size_t dlen = trim;
            if (dlen == 0)
                dlen = 1;
            if (dlen >= sizeof(dir))
                return;
            memcpy(dir, expanded_prefix, dlen);
            dir[dlen] = '\0';
            base = "";
        } else {
            size_t dlen = (size_t) (last_slash - expanded_prefix);
            if (dlen == 0)
                dlen = 1; /* parent is root */
            if (dlen >= sizeof(dir))
                return;
            memcpy(dir, expanded_prefix, dlen);
            dir[dlen] = '\0';
            base = last_slash + 1;
        }
    }
    size_t baselen = strlen(base);

    DIR* dp = opendir(dir);
    if (!dp)
        return; /* ENOENT/EACCES: nothing to add, stay silent */

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    size_t scanned = 0;
    struct dirent* de;
    while ((de = readdir(dp)) != NULL) {
        if (++scanned > FS_FALLBACK_MAX_SCAN)
            break;
        if (*out_idx >= want)
            break;
        /* Time budget: bail out so a hung mount can't wedge this worker
         * past the shell's 400ms Tab timeout. Index results (packed
         * first) are already returned; fallback is best-effort. */
        if ((scanned & 63) == 0) {
            struct timespec t_now;
            clock_gettime(CLOCK_MONOTONIC, &t_now);
            long elapsed_ms = (t_now.tv_sec - t_start.tv_sec) * 1000L +
                              (t_now.tv_nsec - t_start.tv_nsec) / 1000000L;
            if (elapsed_ms > FS_FALLBACK_BUDGET_MS)
                break;
        }
        const char* name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (baselen > 0 && strncmp(name, base, baselen) != 0)
            continue;
        if (!typed_dot && name[0] == '.')
            continue;

        bool is_dir = false;
        if (de->d_type == DT_DIR) {
            is_dir = true;
        } else if (de->d_type == DT_REG) {
            is_dir = false;
        } else if (de->d_type == DT_LNK || de->d_type == DT_UNKNOWN) {
            char full[4096];
            int n = snprintf(full, sizeof(full), "%s/%s",
                             strcmp(dir, "/") == 0 ? "" : dir, name);
            if (n <= 0 || (size_t) n >= sizeof(full))
                continue;
            struct stat st;
            if (stat(full, &st) != 0)
                continue; /* dangling symlink etc: skip */
            is_dir = S_ISDIR(st.st_mode);
        } else {
            continue; /* sockets/fifos/devices: not completable */
        }
        if (dirs_only && !is_dir)
            continue;

        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, name);
        if (n <= 0 || (size_t) n >= sizeof(full))
            continue;
        if (dedup_contains(seen, full))
            continue;
        dedup_insert(seen, full);
        if (ipc_pack_completions_add(packed, IPC_MAX_PAYLOAD, pack_pos, packed_count, full,
                                     is_dir ? 1 : 0, 0.0f) != 0)
            break;
        (*out_idx)++;
    }
    closedir(dp);
}

static uint8_t complete_empty_hint(daemon_state* d, const char* abs) {
    if (!d || !abs || abs[0] == '\0')
        return IPC_HINT_EMPTY;
    int under = 0;
    for (int i = 0; i < d->last_scan_path_count; i++) {
        size_t rl = strlen(d->last_scan_paths[i]);
        if (rl == 0)
            continue;
        if (strncmp(abs, d->last_scan_paths[i], rl) == 0 &&
            (abs[rl] == '\0' || abs[rl] == '/')) {
            under = 1;
            break;
        }
    }
    if (!under)
        return IPC_HINT_OUTSIDE;
    char tmp[4096];
    strncpy(tmp, abs, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
        for (int i = 0; i < d->scanner.ignore_dir_count; i++) {
            if (fnmatch(d->scanner.ignore_dirs[i], tok, 0) == 0)
                return IPC_HINT_IGNORED;
        }
    }
    if (atomic_load(&d->scanning))
        return IPC_HINT_SCANNING;
    return IPC_HINT_EMPTY;
}

/* Discard hdr.payload_len bytes so the next read starts at a header.
 * Prevents framing desync / request smuggling after a bad-length error. */
static void drain_payload(int fd, uint32_t len) {
    char buf[1024];
    while (len > 0) {
        size_t chunk = len > sizeof(buf) ? sizeof(buf) : len;
        if (read_exact(fd, buf, chunk) < 0)
            break;
        len -= (uint32_t) chunk;
    }
}

static void send_error(int fd, uint32_t req_id, int32_t code, const char* msg) {
    ipc_header hdr;
    ipc_error_resp resp;
    resp.error_code = code;
    memset(resp.message, 0, sizeof(resp.message));
    strncpy(resp.message, msg, sizeof(resp.message) - 1);

    ipc_write_header(&hdr, IPC_MSG_ERROR, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void send_ok(int fd, uint32_t req_id) {
    ipc_header hdr;
    ipc_ok_resp resp;
    resp.status = 0;

    ipc_write_header(&hdr, IPC_MSG_OK, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_scan(ipc_server* srv, int fd, uint32_t req_id, const ipc_scan_req* req) {
    if (validate_string_field(req->path, sizeof(req->path)) != 0) {
        send_error(fd, req_id, -6, "invalid path: not null-terminated");
        return;
    }
    /* Backward compat: old clients sent scan "__clear_cache__" to flush
     * the query cache. Honor it as a cache clear so we never index a
     * magic path or pollute last_scan_paths. New clients use
     * IPC_MSG_CLEAR_CACHE. */
    if (strcmp(req->path, "__clear_cache__") == 0) {
        cache_clear(srv->daemon->cache);
        send_ok(fd, req_id);
        return;
    }
    /* Empty path = rescan all known roots (used by `reindex` with no arg).
     * Never default to "/" server-side either. */
    if (req->path[0] == '\0') {
        if (srv->daemon->last_scan_path_count <= 0) {
            send_error(fd, req_id, -9, "no scan roots configured");
            return;
        }
        const char* roots[CONFIG_MAX_ROOTS];
        for (int i = 0; i < srv->daemon->last_scan_path_count; i++)
            roots[i] = srv->daemon->last_scan_paths[i];
        daemon_run_scan_multi(srv->daemon, roots, srv->daemon->last_scan_path_count);
        send_ok(fd, req_id);
        return;
    }
    /* Normalize first so "/a/../b" and "~/x" resolve safely. Only exact
     * ".." components are traversal; substrings like "a..b" are legit
     * filenames. path_normalize already collapses ".." without escaping
     * "/", so no rejection is needed for normalized absolute paths. */
    {
        char expanded[4096];
        char normalized[4096];
        path_expand_abbrev(expanded, req->path, sizeof(expanded));
        path_normalize(normalized, expanded, sizeof(normalized));
        if (normalized[0] == '\0') {
            send_error(fd, req_id, -6, "invalid path");
            return;
        }
        daemon_run_scan(srv->daemon, normalized);
    }
    send_ok(fd, req_id);
}

static void handle_save(ipc_server* srv, int fd, uint32_t req_id, const ipc_save_req* req) {
    if (validate_string_field(req->save_path, sizeof(req->save_path)) != 0) {
        send_error(fd, req_id, -6, "invalid save path: not null-terminated");
        return;
    }
    daemon_save_state(srv->daemon, req->save_path);
    send_ok(fd, req_id);
}

static void handle_query(ipc_server* srv, int fd, uint32_t req_id, const ipc_query_req* req) {
    if (validate_string_field(req->cwd, sizeof(req->cwd)) != 0) {
        send_error(fd, req_id, -6, "invalid cwd: not null-terminated");
        return;
    }
    if (validate_string_field(req->input, sizeof(req->input)) != 0) {
        send_error(fd, req_id, -6, "invalid input: not null-terminated");
        return;
    }

    char expanded_input[4096];
    path_expand_abbrev(expanded_input, req->input, sizeof(expanded_input));

    /* ".." components (e.g. "cd ../foo") are legitimate shell navigation.
     * Downstream join_path()/normalise_dir() resolves them safely without
     * escaping "/", and substrings like "a..b" are valid filenames, so do
     * not reject here. */

    path_validation v = daemon_process_query(srv->daemon, req->cwd, expanded_input);

    ipc_header hdr;
    ipc_validation_resp resp;
    resp.exists = v.exists;
    resp.is_dir = v.is_dir;
    resp.is_file = v.is_file;
    memset(resp.full_path, 0, sizeof(resp.full_path));
    if (v.full_path) {
        strncpy(resp.full_path, v.full_path, sizeof(resp.full_path) - 1);
    }

    ipc_write_header(&hdr, IPC_MSG_VALIDATION, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));

    free_path_validation(&v);
}

static void handle_complete(ipc_server* srv, int fd, uint32_t req_id, const ipc_complete_req* req) {
    if (validate_string_field(req->prefix, sizeof(req->prefix)) != 0) {
        send_error(fd, req_id, -6, "invalid prefix: not null-terminated");
        return;
    }
    if (validate_string_field(req->cwd, sizeof(req->cwd)) != 0) {
        send_error(fd, req_id, -6, "invalid cwd: not null-terminated");
        return;
    }

    char expanded_tmp[4096];
    char expanded_prefix[4096];
    path_expand_abbrev(expanded_tmp, req->prefix, sizeof(expanded_tmp));
    if (expanded_tmp[0] != '/' && req->cwd[0] == '/') {
        char joined[4096];
        int n = snprintf(joined, sizeof(joined), "%s/%s", req->cwd, expanded_tmp);
        if (n > 0 && (size_t) n < sizeof(joined))
            memcpy(expanded_tmp, joined, (size_t) n + 1);
    }
    size_t tmp_len = strlen(expanded_tmp);
    int explicit_slash = (tmp_len > 0 && expanded_tmp[tmp_len - 1] == '/');
    const char* keep_dot = strrchr(expanded_tmp, '/');
    int keep_trailing_dot = (keep_dot && strcmp(keep_dot, "/.") == 0);
    path_normalize(expanded_prefix, expanded_tmp, sizeof(expanded_prefix));
    if (keep_trailing_dot) {
        size_t elen = strlen(expanded_prefix);
        if (elen + 2 < sizeof(expanded_prefix)) {
            if (elen > 0 && expanded_prefix[elen - 1] != '/')
                expanded_prefix[elen++] = '/';
            expanded_prefix[elen++] = '.';
            expanded_prefix[elen] = '\0';
        }
    }

    uint64_t now = (uint64_t) time(NULL);
    uint32_t want = req->limit > 0 ? req->limit : 50;
    if (want > IPC_COMPLETE_MAX)
        want = IPC_COMPLETE_MAX;

    scored_result sr_dirs = daemon_get_scored_completions(srv->daemon, expanded_prefix, want, now,
                                                          req->cwd, 1);
    scored_result sr_files = {NULL, false};
    if (!req->dirs_only)
        sr_files = daemon_get_scored_completions(srv->daemon, expanded_prefix, want, now, req->cwd,
                                                 0);

    bool packed_owned = false;
    uint8_t* packed = packed_buffer_reuse(&packed_owned);
    if (!packed) {
        send_error(fd, req_id, -7, "out of memory");
        if (sr_dirs.data)
            daemon_release_scored(srv->daemon, sr_dirs);
        if (sr_files.data)
            daemon_release_scored(srv->daemon, sr_files);
        return;
    }
    uint8_t scanning = atomic_load(&srv->daemon->scanning) ? 1 : 0;
    size_t pack_pos = ipc_pack_completions_begin(packed, IPC_MAX_PAYLOAD, scanning);
    uint32_t packed_count = 0;

    const scored_completions* sources[2];
    int nsrc = 0;
    if (sr_dirs.data)
        sources[nsrc++] = sr_dirs.data;
    if (sr_files.data)
        sources[nsrc++] = sr_files.data;

    uint32_t out_idx = 0;
    size_t prefix_len = strlen(expanded_prefix);
    if (prefix_len > 1 && expanded_prefix[prefix_len - 1] == '/')
        prefix_len--;
    const char* last_comp = strrchr(expanded_prefix, '/');
    last_comp = last_comp ? last_comp + 1 : expanded_prefix;
    int typed_dot = (last_comp[0] == '.');
    dedup_set seen;
    dedup_init(&seen);

    for (int s = 0; s < nsrc && out_idx < want; s++) {
        const scored_completions* sc = sources[s];
        uint32_t n = (uint32_t) sc->count;
        for (uint32_t i = 0; i < n && out_idx < want; i++) {
            if (req->dirs_only && !sc->entries[i].is_dir)
                continue;
            if (!req->dirs_only && s == 1 && sc->entries[i].is_dir)
                continue;

            const char* p = sc->entries[i].path;
            if (!typed_dot && is_hidden_path(p))
                continue;
            size_t path_len = strlen(p);
            size_t effective_path_len = path_len;
            if (effective_path_len > 1 && p[effective_path_len - 1] == '/')
                effective_path_len--;

            if (prefix_len > 0) {
                if (effective_path_len <= prefix_len)
                    continue;
                if (strncmp(p, expanded_prefix, prefix_len) != 0)
                    continue;
                if (p[prefix_len] == '/') {
                    if (memchr(p + prefix_len + 1, '/', effective_path_len - prefix_len - 1) != NULL)
                        continue;
                } else if (explicit_slash) {
                    continue;
                } else if (memchr(p + prefix_len, '/', effective_path_len - prefix_len) != NULL) {
                    continue;
                }
            }

            char clean[4096];
            if (path_len > 0 && p[path_len - 1] == '/') {
                memcpy(clean, p, path_len - 1);
                clean[path_len - 1] = '\0';
            } else {
                strncpy(clean, p, sizeof(clean) - 1);
                clean[sizeof(clean) - 1] = '\0';
            }

            if (dedup_contains(&seen, clean))
                continue;
            dedup_insert(&seen, clean);

            if (ipc_pack_completions_add(packed, IPC_MAX_PAYLOAD, &pack_pos, &packed_count, clean,
                                         sc->entries[i].is_dir ? 1 : 0,
                                         (float) sc->entries[i].score) != 0)
                break;
            out_idx++;
        }
    }
    /* Index missed (or was thin): fall back to the live filesystem so a
     * stale index, an outside-roots path, or an ignored dir still completes.
     * Index results keep priority (packed first); fallback appends. */
    if (out_idx < want) {
        fs_fallback_fill(expanded_prefix, explicit_slash, req->dirs_only ? 1 : 0, typed_dot, want,
                         &out_idx, &packed_count, packed, &pack_pos, &seen);
    }
    dedup_free(&seen);
    ipc_pack_completions_finish(packed, packed_count);
    if (packed_count == 0) {
        uint8_t hint = complete_empty_hint(srv->daemon, expanded_prefix);
        if (scanning)
            hint = IPC_HINT_SCANNING;
        ipc_pack_completions_set_hint(packed, hint);
    }
    if (sr_dirs.data)
        daemon_release_scored(srv->daemon, sr_dirs);
    if (sr_files.data)
        daemon_release_scored(srv->daemon, sr_files);

    daemon_log_query(srv->daemon, req->prefix, req->cwd, packed_count);

    ipc_header hdr;
    ipc_write_header(&hdr, IPC_MSG_COMPLETIONS, (uint32_t) pack_pos, req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, packed, pack_pos);
    if (packed_owned)
        free(packed);
}

static void handle_suggest(ipc_server* srv, int fd, uint32_t req_id, const ipc_suggest_req* req) {
    if (validate_string_field(req->prefix, sizeof(req->prefix)) != 0) {
        send_error(fd, req_id, -6, "invalid prefix: not null-terminated");
        return;
    }
    if (validate_string_field(req->cwd, sizeof(req->cwd)) != 0) {
        send_error(fd, req_id, -6, "invalid cwd: not null-terminated");
        return;
    }

    char expanded_tmp[4096];
    char expanded_prefix[4096];
    path_expand_abbrev(expanded_tmp, req->prefix, sizeof(expanded_tmp));
    path_normalize(expanded_prefix, expanded_tmp, sizeof(expanded_prefix));

    uint64_t now = (uint64_t) time(NULL);
    scored_result sr =
        daemon_get_scored_completions(srv->daemon, expanded_prefix, 1, now, req->cwd, 0);
    const scored_completions* sc = sr.data;

    ipc_header hdr;
    ipc_suggestion_resp resp;
    memset(&resp, 0, sizeof(resp));

    if (sc && sc->count > 0) {
        const char* p = sc->entries[0].path;
        size_t plen = strlen(p);
        if (plen > 0 && p[plen - 1] == '/') {
            memcpy(resp.path, p, plen - 1);
            resp.path[plen - 1] = '\0';
        } else {
            strncpy(resp.path, p, sizeof(resp.path) - 1);
            resp.path[sizeof(resp.path) - 1] = '\0';
        }
        resp.score = sc->entries[0].score;
        resp.freq = sc->entries[0].freq;
        resp.is_dir = sc->entries[0].is_dir ? 1 : 0;
        session_record_selection(resp.path);
        daemon_release_scored(srv->daemon, sr);
    }

    ipc_write_header(&hdr, IPC_MSG_SUGGESTION, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_recent(ipc_server* srv, int fd, uint32_t req_id, const ipc_recent_req* req) {
    ipc_header hdr;
    ipc_recent_resp resp;
    resp.count = 0;
    memset(resp.paths, 0, sizeof(resp.paths));
    memset(resp.is_dirs, 0, sizeof(resp.is_dirs));

    uint32_t limit = req->limit > 0 && req->limit <= 50 ? req->limit : 50;
    char* paths[50] = {0};
    bool alloc_ok = true;
    for (uint32_t i = 0; i < limit; i++) {
        paths[i] = malloc(4096);
        if (!paths[i]) {
            alloc_ok = false;
            break;
        }
    }
    if (!alloc_ok) {
        for (uint32_t i = 0; i < limit; i++)
            free(paths[i]);
        send_error(fd, req_id, -7, "out of memory");
        return;
    }
    bool is_dirs[50] = {0};

    int count = daemon_get_recent_files(srv->daemon, paths, is_dirs, (int) limit);

    dedup_set seen;
    dedup_init(&seen);
    uint32_t out_idx = 0;
    for (int i = 0; i < count && out_idx < 50; i++) {
        if (dedup_contains(&seen, paths[i]))
            continue;
        dedup_insert(&seen, paths[i]);
        strncpy(resp.paths[out_idx], paths[i], sizeof(resp.paths[out_idx]) - 1);
        resp.paths[out_idx][sizeof(resp.paths[out_idx]) - 1] = '\0';
        resp.is_dirs[out_idx] = is_dirs[i] ? 1 : 0;
        out_idx++;
    }
    resp.count = out_idx;
    dedup_free(&seen);

    for (uint32_t i = 0; i < limit; i++) {
        free(paths[i]);
    }

    ipc_write_header(&hdr, IPC_MSG_RECENT_RESP, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_ping(ipc_server* srv, int fd, uint32_t req_id) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec_diff = (int64_t) now.tv_sec - (int64_t) srv->start_time.tv_sec;
    int64_t nsec_diff = (int64_t) now.tv_nsec - (int64_t) srv->start_time.tv_nsec;
    if (nsec_diff < 0) {
        sec_diff--;
        nsec_diff += 1000000000LL;
    }
    uint64_t uptime_ms = (uint64_t) sec_diff * 1000ULL + (uint64_t) nsec_diff / 1000000ULL;

    ipc_header hdr;
    ipc_pong_resp resp;
    resp.uptime_ms = uptime_ms;

    ipc_write_header(&hdr, IPC_MSG_PONG, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_metrics(ipc_server* srv, int fd, uint32_t req_id) {
    metrics_snapshot snap = metrics_snapshot_get(&srv->daemon->metrics);

    ipc_header hdr;
    ipc_metrics_resp resp;
    resp.queries_total = snap.queries_total;
    resp.completions_total = snap.completions_total;
    resp.scans_total = snap.scans_total;
    resp.errors_total = snap.errors_total;
    resp.cache_hits = snap.cache_hits;
    resp.cache_misses = snap.cache_misses;
    resp.query_latency_avg_ms = snap.query_latency_avg_ms;

    store_lock(srv->daemon->store);
    resp.total_dirs_indexed = srv->daemon->store->right_index;
    uint64_t total_nodes = atomic_load(&srv->daemon->store->total_nodes);
    store_unlock(srv->daemon->store);
    resp.total_paths_indexed = total_nodes + resp.total_dirs_indexed;

    ipc_write_header(&hdr, IPC_MSG_METRICS_RESP, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_scan_status(ipc_server* srv, int fd, uint32_t req_id) {
    scan_status s = daemon_scan_status(srv->daemon);

    ipc_header hdr;
    ipc_scan_status_resp resp;
    resp.scanning = s.scanning ? 1 : 0;
    resp.buckets_so_far = s.buckets_so_far;

    ipc_write_header(&hdr, IPC_MSG_SCAN_STATUS_RESP, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_fuzzy_complete(ipc_server* srv, int fd, uint32_t req_id,
                                  const ipc_complete_req* req) {
    if (validate_string_field(req->prefix, sizeof(req->prefix)) != 0) {
        send_error(fd, req_id, -6, "invalid prefix: not null-terminated");
        return;
    }
    /* Only exact ".." components are traversal; "a..b" is a valid
     * filename fragment for fuzzy matching. */
    if (path_has_dotdot_component(req->prefix)) {
        send_error(fd, req_id, -8, "path traversal not allowed");
        return;
    }

    uint32_t want = req->limit > 0 ? req->limit : 50;
    if (want > IPC_COMPLETE_MAX)
        want = IPC_COMPLETE_MAX;
    completions* fc = daemon_get_fuzzy_completions(srv->daemon, req->prefix, want);

    bool packed_owned = false;
    uint8_t* packed = packed_buffer_reuse(&packed_owned);
    if (!packed) {
        if (fc)
            completions_free(fc);
        send_error(fd, req_id, -7, "out of memory");
        return;
    }
    uint8_t scanning = atomic_load(&srv->daemon->scanning) ? 1 : 0;
    size_t pack_pos = ipc_pack_completions_begin(packed, IPC_MAX_PAYLOAD, scanning);
    uint32_t packed_count = 0;

    if (fc) {
        uint32_t n = (uint32_t) fc->count;
        dedup_set seen;
        dedup_init(&seen);
        for (uint32_t i = 0; i < n && packed_count < want; i++) {
            const char* p = fc->paths[i];
            if (!p)
                continue;
            size_t path_len = strlen(p);

            char clean[4096];
            if (path_len > 0 && p[path_len - 1] == '/') {
                memcpy(clean, p, path_len - 1);
                clean[path_len - 1] = '\0';
            } else {
                strncpy(clean, p, sizeof(clean) - 1);
                clean[sizeof(clean) - 1] = '\0';
            }

            if (dedup_contains(&seen, clean))
                continue;
            dedup_insert(&seen, clean);

            uint8_t is_dir = (fc->is_dirs && fc->is_dirs[i]) ? 1 : 0;
            if (ipc_pack_completions_add(packed, IPC_MAX_PAYLOAD, &pack_pos, &packed_count, clean,
                                         is_dir, 0.0f) != 0)
                break;
        }
        dedup_free(&seen);
        completions_free(fc);
    }

    ipc_pack_completions_finish(packed, packed_count);
    ipc_header hdr;
    ipc_write_header(&hdr, IPC_MSG_FUZZY_COMPLETIONS, (uint32_t) pack_pos, req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, packed, pack_pos);
    if (packed_owned)
        free(packed);
}

static void handle_select(ipc_server* srv, int fd, uint32_t req_id, const ipc_select_req* req) {
    if (validate_string_field(req->path, sizeof(req->path)) != 0) {
        send_error(fd, req_id, -6, "invalid path: not null-terminated");
        return;
    }
    daemon_record_selection(srv->daemon, req->path);
    send_ok(fd, req_id);
}

static void handle_client(ipc_server* srv, int fd) {
    ipc_header hdr;

    while (atomic_load(&srv->running)) {
        if (read_exact(fd, &hdr, sizeof(hdr)) < 0) {
            break;
        }

        if (!ipc_validate_header(&hdr)) {
            if ((hdr.magic >> 16) == IPC_MAGIC_PREFIX) {
                char msg[64];
                snprintf(msg, sizeof(msg), "unsupported protocol version %u (server supports 1-%u)",
                         ipc_header_version(&hdr), IPC_PROTOCOL_VERSION);
                send_error(fd, 0, -5, msg);
            } else {
                send_error(fd, 0, -1, "invalid message header");
            }
            break;
        }

        if (hdr.payload_len > IPC_MAX_PAYLOAD) {
            send_error(fd, hdr.request_id, -2, "payload too large");
            break;
        }

        switch (hdr.msg_type) {
        case IPC_MSG_SCAN: {
            ipc_scan_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid scan payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            handle_scan(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_SAVE: {
            ipc_save_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid save payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            handle_save(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_QUERY: {
            ipc_query_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid query payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            handle_query(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_COMPLETE: {
            ipc_complete_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid complete payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            handle_complete(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_SUGGEST: {
            ipc_suggest_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid suggest payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            handle_suggest(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_SHUTDOWN: {
            if (hdr.payload_len != 0)
                drain_payload(fd, hdr.payload_len);
            send_ok(fd, hdr.request_id);
            atomic_store(&srv->running, false);
            close(fd);
            return;
        }
        case IPC_MSG_PING: {
            if (hdr.payload_len != 0) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid ping payload");
                break;
            }
            handle_ping(srv, fd, hdr.request_id);
            break;
        }
        case IPC_MSG_METRICS: {
            if (hdr.payload_len != 0) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid metrics payload");
                break;
            }
            handle_metrics(srv, fd, hdr.request_id);
            break;
        }
        case IPC_MSG_SCAN_STATUS: {
            if (hdr.payload_len != 0) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid scan-status payload");
                break;
            }
            handle_scan_status(srv, fd, hdr.request_id);
            break;
        }
        case IPC_MSG_FUZZY_COMPLETE: {
            ipc_complete_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid fuzzy complete payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            handle_fuzzy_complete(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_SELECT: {
            ipc_select_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid select payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            handle_select(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_RECENT: {
            ipc_recent_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid recent payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            handle_recent(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_BOOKMARKS: {
            ipc_bookmarks_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid bookmarks payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            ipc_header resp_hdr;
            ipc_bookmarks_resp resp;
            resp.count = 0;
            memset(resp.paths, 0, sizeof(resp.paths));

            uint32_t n = srv->daemon->bookmark_count < 50 ? srv->daemon->bookmark_count : 50;
            uint32_t limit = req.limit > 0 && req.limit < 50 ? req.limit : 50;
            for (uint32_t i = 0; i < n && resp.count < limit; i++) {
                if (srv->daemon->bookmarks[i][0] != '\0') {
                    strncpy(resp.paths[resp.count], srv->daemon->bookmarks[i],
                            sizeof(resp.paths[resp.count]) - 1);
                    resp.count++;
                }
            }

            ipc_write_header(&resp_hdr, IPC_MSG_BOOKMARKS_RESP, sizeof(resp), hdr.request_id);
            write_exact(fd, &resp_hdr, sizeof(resp_hdr));
            write_exact(fd, &resp, sizeof(resp));
            break;
        }
        case IPC_MSG_HEALTH: {
            ipc_health_resp resp;
            memset(&resp, 0, sizeof(resp));
            resp.daemon_running = 1;
            resp.scanning = atomic_load(&srv->daemon->scanning) ? 1 : 0;
            resp.watcher_active = (srv->daemon->watcher != NULL) ? 1 : 0;
            store_lock(srv->daemon->store);
            resp.buckets_indexed = srv->daemon->store ? srv->daemon->store->right_index : 0;
            store_unlock(srv->daemon->store);
            resp.queries_total = atomic_load(&srv->daemon->metrics.queries_total);
            resp.cache_hits = atomic_load(&srv->daemon->metrics.cache_hits);
            resp.cache_misses = atomic_load(&srv->daemon->metrics.cache_misses);
            resp.rescan_interval = (uint32_t) srv->daemon->rescan_interval_seconds;
            resp.bookmark_count = (uint32_t) srv->daemon->bookmark_count;
            resp.recent_count = (uint32_t) srv->daemon->recent.count;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            resp.uptime_seconds = (uint64_t) (now.tv_sec - srv->daemon->start_time.tv_sec);
            resp.estimated_memory_bytes = atomic_load(&srv->daemon->store->estimated_memory_bytes);
#ifdef __linux__
            {
                FILE* mf = fopen("/proc/self/statm", "r");
                if (mf) {
                    unsigned long rss_pages = 0;
                    if (fscanf(mf, "%*u %lu", &rss_pages) == 1) {
                        long pg = sysconf(_SC_PAGESIZE);
                        if (pg > 0)
                            resp.estimated_memory_bytes = (uint64_t) rss_pages * (uint64_t) pg;
                    }
                    fclose(mf);
                }
            }
#endif
            resp.daemon_pid = (int32_t) getpid();
            resp.active_connections = atomic_load(&srv->active_connections);
            strncpy(resp.socket_path, srv->sock_path, sizeof(resp.socket_path) - 1);

            resp.scanning_progress_pct = 0;
            if (atomic_load(&srv->daemon->scanning)) {
                int dirs = atomic_load(&srv->daemon->scanner.dirs_scanned);
                resp.scanning_progress_pct = dirs > 0 ? (dirs < 100 ? dirs : 100) : 0;
            }

            resp.scan_root_count = srv->daemon->last_scan_path_count;
            for (int i = 0; i < resp.scan_root_count && i < 10; i++) {
                strncpy(resp.scan_roots[i], srv->daemon->last_scan_paths[i],
                        sizeof(resp.scan_roots[i]) - 1);
            }
            resp.protocol_version = IPC_PROTOCOL_VERSION;

            cache_stats cs = cache_get_stats(srv->daemon->cache);
            resp.cache_entries = (int32_t) cs.entries;

            resp.files_scanned = (uint64_t) atomic_load(&srv->daemon->scanner.files_scanned);
            resp.dirs_scanned = (uint64_t) atomic_load(&srv->daemon->scanner.dirs_scanned);

            ipc_header resp_hdr;
            ipc_write_header(&resp_hdr, IPC_MSG_HEALTH_RESP, sizeof(resp), hdr.request_id);
            write_exact(fd, &resp_hdr, sizeof(resp_hdr));
            write_exact(fd, &resp, sizeof(resp));
            break;
        }
        case IPC_MSG_RELOAD_CONFIG: {
            if (hdr.payload_len != 0)
                drain_payload(fd, hdr.payload_len);
            atomic_store(&srv->daemon->config_reload_requested, true);
            ipc_header resp_hdr;
            ipc_ok_resp resp;
            resp.status = 0;
            ipc_write_header(&resp_hdr, IPC_MSG_OK, sizeof(resp), hdr.request_id);
            write_exact(fd, &resp_hdr, sizeof(resp_hdr));
            write_exact(fd, &resp, sizeof(resp));
            break;
        }
        case IPC_MSG_RESET_STATS: {
            if (hdr.payload_len != 0)
                drain_payload(fd, hdr.payload_len);
            memset(&srv->daemon->metrics, 0, sizeof(srv->daemon->metrics));
            cache_clear(srv->daemon->cache);
            ipc_header resp_hdr;
            ipc_ok_resp resp;
            resp.status = 0;
            ipc_write_header(&resp_hdr, IPC_MSG_OK, sizeof(resp), hdr.request_id);
            write_exact(fd, &resp_hdr, sizeof(resp_hdr));
            write_exact(fd, &resp, sizeof(resp));
            break;
        }
        case IPC_MSG_CLEAR_CACHE: {
            if (hdr.payload_len != 0) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid clear-cache payload");
                break;
            }
            cache_clear(srv->daemon->cache);
            ipc_header resp_hdr;
            ipc_ok_resp resp;
            resp.status = 0;
            ipc_write_header(&resp_hdr, IPC_MSG_OK, sizeof(resp), hdr.request_id);
            write_exact(fd, &resp_hdr, sizeof(resp_hdr));
            write_exact(fd, &resp, sizeof(resp));
            break;
        }
        case IPC_MSG_FUZZY_SUGGEST: {
            ipc_fuzzy_suggest_req req;
            if (hdr.payload_len != sizeof(req)) {
                drain_payload(fd, hdr.payload_len);
                send_error(fd, hdr.request_id, -3, "invalid fuzzy suggest payload");
                break;
            }
            if (read_exact(fd, &req, sizeof(req)) < 0) {
                break;
            }
            if (validate_string_field(req.query, sizeof(req.query)) != 0) {
                send_error(fd, hdr.request_id, -6, "invalid query: not null-terminated");
                break;
            }

            char expanded[4096];
            path_expand_abbrev(expanded, req.query, sizeof(expanded));

            completions* fc = daemon_get_fuzzy_completions(srv->daemon, expanded, req.limit);

            ipc_header resp_hdr;
            ipc_fuzzy_suggest_resp resp;
            resp.count = 0;
            memset(resp.suggestions, 0, sizeof(resp.suggestions));
            memset(resp.scores, 0, sizeof(resp.scores));

            if (fc) {
                uint32_t n = fc->count < 20 ? fc->count : 20;
                for (uint32_t i = 0; i < n; i++) {
                    strncpy(resp.suggestions[i], fc->paths[i], sizeof(resp.suggestions[i]) - 1);
                    resp.count++;
                }
                completions_free(fc);
            }

            ipc_write_header(&resp_hdr, IPC_MSG_FUZZY_SUGGEST_RESP, sizeof(resp), hdr.request_id);
            write_exact(fd, &resp_hdr, sizeof(resp_hdr));
            write_exact(fd, &resp, sizeof(resp));
            break;
        }
        default:
            drain_payload(fd, hdr.payload_len);
            send_error(fd, hdr.request_id, -4, "unknown message type");
            break;
        }
    }

    close(fd);
}

static void handle_client_threaded(void* arg) {
    client_ctx* ctx = (client_ctx*) arg;
    atomic_fetch_add(&ctx->srv->active_connections, 1);
    handle_client(ctx->srv, ctx->fd);
    atomic_fetch_sub(&ctx->srv->active_connections, 1);
    free(ctx);
}

static void* server_loop(void* arg) {
    ipc_server* srv = (ipc_server*) arg;

    while (atomic_load(&srv->running)) {
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == EAGAIN ||
                errno == ECONNABORTED)
                continue;
            if (!atomic_load(&srv->running))
                break;
            break;
        }
        client_ctx* ctx = malloc(sizeof(client_ctx));
        if (!ctx) {
            close(fd);
            continue;
        }
        ctx->srv = srv;
        ctx->fd = fd;
        if (threadpool_submit(srv->pool, handle_client_threaded, ctx) != 0) {
            close(fd);
            free(ctx);
        }
    }

    return NULL;
}

ipc_server* ipc_server_start(daemon_state* daemon, const char* sock_path) {
    ipc_server* srv = calloc(1, sizeof(ipc_server));
    if (!srv)
        return NULL;

    srv->daemon = daemon;
    atomic_store(&srv->running, true);
    strncpy(srv->sock_path, sock_path, sizeof(srv->sock_path) - 1);
    atomic_store(&srv->active_connections, 0);

    int pool_size;
    {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        pool_size = (nproc > 0) ? (int) (nproc * 2) : 8;
        if (pool_size > 16)
            pool_size = 16;
        if (pool_size < 4)
            pool_size = 4;
    }
    srv->pool = threadpool_init(pool_size);
    if (!srv->pool) {
        free(srv);
        return NULL;
    }

    /* A client disconnecting mid-reply must not kill the daemon. */
    signal(SIGPIPE, SIG_IGN);

    clock_gettime(CLOCK_MONOTONIC, &srv->start_time);

    unlink(sock_path);

    /* Restrict socket to owner-only before binding */
    mode_t old_umask = umask(S_IRWXG | S_IRWXO);

    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        umask(old_umask);
        threadpool_shutdown(srv->pool);
        free(srv);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(srv->listen_fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        umask(old_umask);
        threadpool_shutdown(srv->pool);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    umask(old_umask);

    if (listen(srv->listen_fd, 8) < 0) {
        threadpool_shutdown(srv->pool);
        close(srv->listen_fd);
        unlink(sock_path);
        free(srv);
        return NULL;
    }

    if (pthread_create(&srv->worker, NULL, server_loop, srv) != 0) {
        threadpool_shutdown(srv->pool);
        close(srv->listen_fd);
        unlink(sock_path);
        free(srv);
        return NULL;
    }

    return srv;
}

void ipc_server_stop(ipc_server* srv) {
    if (!srv)
        return;

    atomic_store(&srv->running, false);

    /* Wake up accept with a dummy connection */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, srv->sock_path, sizeof(addr.sun_path) - 1);
        connect(fd, (struct sockaddr*) &addr, sizeof(addr));
        close(fd);
    }

    pthread_join(srv->worker, NULL);

    if (srv->pool) {
        threadpool_shutdown(srv->pool);
        srv->pool = NULL;
    }

    close(srv->listen_fd);
    unlink(srv->sock_path);
    free(srv);
}
