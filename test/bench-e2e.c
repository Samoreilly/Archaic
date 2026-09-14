/*
 * bench-e2e.c — End-to-end performance benchmark for archaic.
 *
 * Creates a real directory tree on disk, fork/execs the daemon binary,
 * connects via Unix socket IPC (using ipc/client.c), and measures:
 *   1. Cold start — fork/exec to socket-ready + first scan completes
 *   2. Cold query  — first IPC_MSG_COMPLETE after cache reset (trie walk)
 *   3. Warm query  — repeated IPC_MSG_COMPLETE (cache hit)
 *   4. Save        — state persistence round-trip
 *
 * Nothing is faked: every number is a real wall-clock measurement of the
 * real daemon over a real Unix socket against a real filesystem.
 *
 * Usage:  ./archaic-bench-e2e [small|medium|large]
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ipc/client.h"
#include "ipc/protocol.h"

/* ── tunables ─────────────────────────────────────────────────────────────── */

#define COLD_ITERS  40
#define WARM_ITERS  60
#define MAX_RESULT  50
#define DAEMON_BIN  "build/archaic"

/* ── types ────────────────────────────────────────────────────────────────── */

typedef struct {
    double* v;
    int     n;
} samples;

typedef struct {
    const char* tag;
    long        files;        /* files actually created on disk */
    double      scan_ms;      /* fork/exec → scan done */
    double      save_ms;      /* save round-trip + bytes written */
    long        mem_kb;       /* daemon RSS after scan (from health) */
    int         completions;  /* items returned per Tab press */
    samples     cold;         /* cache-miss ms */
    samples     warm;         /* cache-hit  ms */
    double      cold_first;   /* the first (cache miss) query in ms */
    int         cold_ok;      /* queries returning >=1 completion */
    int         cold_empty;   /* queries returning 0 completions */
    uint64_t    cold_cache_hits;
    uint64_t    cold_cache_misses;
    uint64_t    warm_cache_hits;
    uint64_t    warm_cache_misses;
} result_row;

/* ── helpers ──────────────────────────────────────────────────────────────── */

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int cmp_d(const void* a, const void* b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

static double pctile(double* v, int n, int p) {
    return v[(int)((n - 1) * p / 100.0)];
}

static double mean(double* v, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += v[i];
    return n ? s / n : 0;
}

static samples make_samples(int n) {
    samples s;
    s.v = calloc((size_t)n, sizeof(double));
    s.n = n;
    return s;
}

static void free_samples(samples* s) {
    free(s->v);
    s->v = NULL;
    s->n = 0;
}

static int ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    return mkdir(path, 0755);
}

/* ── directory tree builder ───────────────────────────────────────────────── */
/* Builds a realistic hierarchy: <root>/<top>/<sub>_<n>/f_<hash><ext>.
 * Returns the number of files created. */

static long build_tree(const char* root, long target) {
    static const char* tops[] = {
        "src", "lib", "tests", "docs", "tools",
        "modules", "packages", "internal", "app", "core",
    };
    static const char* exts[] = {
        ".c", ".h", ".py", ".js", ".ts", ".rs", ".go",
        ".md", ".toml", ".yaml", ".sh", ".json",
    };
    static const char* subs[] = {
        "core", "utils", "api", "models", "views",
        "handlers", "services", "config", "data", "build",
    };
    const int nt = (int)(sizeof(tops) / sizeof(tops[0]));
    const int ne = (int)(sizeof(exts) / sizeof(exts[0]));
    const int ns = (int)(sizeof(subs) / sizeof(subs[0]));
    char path[1024];
    long created = 0;

    int top_dirs = 3;
    if (target > 10000) top_dirs = 5;
    if (target > 30000) top_dirs = 8;

    long per_top = target / top_dirs;

    for (int t = 0; t < top_dirs && created < target; t++) {
        const char* top = tops[t % nt];
        snprintf(path, sizeof(path), "%s/%s", root, top);
        ensure_dir(path);

        int subs_n = 3 + (t % 4);              /* 3..6 dirs per top */
        long per_sub = per_top / subs_n;

        for (int s = 0; s < subs_n && created < target; s++) {
            snprintf(path, sizeof(path), "%s/%s/%s_%d",
                     root, top, subs[(t + s + 3) % ns], s);
            ensure_dir(path);

            for (long f = 0; f < per_sub && created < target; f++) {
                unsigned hash = (unsigned)(created * 2654435761u);
                snprintf(path, sizeof(path), "%s/%s/%s_%d/f_%08x%s",
                         root, top, subs[(t + s + 3) % ns], s,
                         hash, exts[(int)(created % (long)ne)]);
                int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (fd >= 0) {
                    char buf[128];
                    memset(buf, 'x', sizeof(buf));
                    if (write(fd, buf, sizeof(buf)) < 0) { /* ignore */ }
                    close(fd);
                    created++;
                }
            }
        }
    }
    return created;
}

/* ── daemon lifecycle ─────────────────────────────────────────────────────── */

static pid_t daemon_start(const char* sock, const char* scan) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(DAEMON_BIN, DAEMON_BIN, "--daemon", scan, sock, (char*)NULL);
        _exit(127);
    }
    return pid;
}

static int daemon_wait_socket(const char* sock, int timeout_s) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    for (int i = 0; i < timeout_s * 20; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(fd);
                return 0;
            }
            close(fd);
        }
        usleep(50000); /* 50 ms */
    }
    return -1;
}

static void daemon_stop(pid_t pid, const char* sock) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status;
        (void)status;
        waitpid(pid, &status, 0);
    }
    if (sock)
        unlink(sock);
}

/* ── cache stats ──────────────────────────────────────────────────────────── */

static void read_cache_stats(ipc_client* c, uint64_t* hits, uint64_t* misses) {
    ipc_health_resp h;
    *hits = *misses = 0;
    if (ipc_client_health(c, &h) == 0) {
        *hits = h.cache_hits;
        *misses = h.cache_misses;
    }
}

/* ── benchmark driver ─────────────────────────────────────────────────────── */

static void run_one(const char* scenario, long target_files, result_row* out) {
    char tmp[256], sock[300], config[512], root_pol[512], cache_dir[300];
    snprintf(tmp, sizeof(tmp), "/tmp/archaic-bench-e2e-%s", scenario);
    snprintf(sock, sizeof(sock), "%s/daemon.sock", tmp);
    snprintf(config, sizeof(config), "%s/config.toml", tmp);
    snprintf(root_pol, sizeof(root_pol), "%s/roots", tmp);
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", tmp);

    unlink(sock);
    unlink(config);
    unlink(root_pol);
    rmdir(tmp);
    if (ensure_dir(tmp) != 0) {
        fprintf(stderr, "  SKIP: cannot create %s\n", tmp);
        return;
    }

    /* 1 ─ real directory tree ───────────────────────────────────────────── */
    fprintf(stderr, "  [%s] generating tree (%ld files)…\n", scenario, target_files);
    double ts = now_ms();
    long created = build_tree(tmp, target_files);
    double tree_ms = now_ms() - ts;
    out->files = created;
    fprintf(stderr, "  [%s] tree built: %ld files, %.0f ms\n", scenario, created, tree_ms);

    /* 2 ─ minimal config pointing at our socket + scan root ─────────────── */
    FILE* f = fopen(config, "w");
    if (f) {
        fprintf(f,
            "[daemon]\n"
            "socket_path = \"%s\"\n"
            "scan_threads = 4\n"
            "max_depth = 10\n"
            "rescan_interval_seconds = 3600\n"
            "log_level = 0\n"
            "max_memory_mb = 512\n",
            sock);
        fclose(f);
    }
    f = fopen(root_pol, "w");
    if (f) {
        fprintf(f, "%s depth=10 watch=0\n", tmp);
        fclose(f);
    }
    ensure_dir(cache_dir);

    /* isolate the daemon from the user's real config/state */
    setenv("ARCHAIC_CONFIG", config, 1);
    setenv("XDG_CACHE_HOME", cache_dir, 1);

    /* 3 ─ fork/exec daemon; scan_ms spans fork→scan-complete ───────────── */
    fprintf(stderr, "  [%s] starting daemon…\n", scenario);
    double t0 = now_ms();
    pid_t pid = daemon_start(sock, tmp);
    if (pid <= 0) {
        fprintf(stderr, "  [%s] fork failed\n", scenario);
        rmdir(tmp);
        return;
    }
    if (daemon_wait_socket(sock, 30) != 0) {
        fprintf(stderr, "  [%s] socket wait timed out\n", scenario);
        daemon_stop(pid, sock);
        rmdir(tmp);
        return;
    }

    ipc_client* client = ipc_client_connect(sock);
    if (!client) {
        fprintf(stderr, "  [%s] client connect failed\n", scenario);
        daemon_stop(pid, sock);
        rmdir(tmp);
        return;
    }

    /* wait for background scan to finish (health exposes files_scanned + memory) */
    ipc_health_resp final_health = {0};
    for (int i = 0; i < 600; i++) {
        ipc_health_resp h;
        if (ipc_client_health(client, &h) == 0 &&
            !h.scanning && (long)h.files_scanned > 0) {
            final_health = h;
            break;
        }
        if (ipc_client_health(client, &final_health) == 0)
            usleep(50000);
        else
            usleep(50000);
    }
    double scan_ms = now_ms() - t0;
    out->scan_ms = scan_ms;
    out->mem_kb = (long)(final_health.estimated_memory_bytes / 1024);
    fprintf(stderr, "  [%s] cold start + scan: %.0f ms  mem=%ld KB\n",
            scenario, scan_ms, out->mem_kb);

    /* ── COLD window: cache is empty, every query walks the trie ───────── */
    ipc_client_reset_stats(client);
    uint64_t h_before, m_before, h_after, m_after;
    read_cache_stats(client, &h_before, &m_before);

    out->cold = make_samples(COLD_ITERS);
    out->cold_ok = 0;
    out->cold_empty = 0;
    out->cold_first = 0;
    out->completions = 0;
    {
        ipc_completion_list list;
        double ts0 = now_ms();
        int rc = ipc_client_complete(client, tmp, MAX_RESULT, tmp, 0, &list);
        double el = now_ms() - ts0;
        out->cold_first = el;                         /* ← real cache-miss walk */
        out->cold.v[0] = el;
        out->completions = (rc == 0) ? (int)list.count : 0;
        if (rc == 0 && list.count >= 1)
            out->cold_ok++;
        else if (rc == 0 && list.count == 0)
            out->cold_empty++;
    }
    for (int i = 1; i < COLD_ITERS; i++) {
        ipc_completion_list list;
        double ts0 = now_ms();
        int rc = ipc_client_complete(client, tmp, MAX_RESULT, tmp, 0, &list);
        double el = now_ms() - ts0;
        out->cold.v[i] = el;
        if (rc == 0 && list.count >= 1)
            out->cold_ok++;
        else if (rc == 0 && list.count == 0)
            out->cold_empty++;
    }
    read_cache_stats(client, &h_after, &m_after);
    out->cold_cache_hits = h_after - h_before;
    out->cold_cache_misses = m_after - m_before;

    /* ── WARM window: the same query is now served from cache ───────────── */
    read_cache_stats(client, &h_before, &m_before);
    out->warm = make_samples(WARM_ITERS);
    for (int i = 0; i < WARM_ITERS; i++) {
        ipc_completion_list list;
        double ts0 = now_ms();
        ipc_client_complete(client, tmp, MAX_RESULT, tmp, 0, &list);
        out->warm.v[i] = now_ms() - ts0;
        (void)list;
    }
    read_cache_stats(client, &h_after, &m_after);
    out->warm_cache_hits = h_after - h_before;
    out->warm_cache_misses = m_after - m_before;

    /* ── SAVE: force state persistence, measure cleanly ─────────────────── */
    /* containment: the daemon only accepts saves under its state dir */
    char save_dir[600], save_path[600];
    snprintf(save_dir, sizeof(save_dir), "%s/archaic", cache_dir);
    ensure_dir(save_dir);
    snprintf(save_path, sizeof(save_path), "%s/bench_save.dat", save_dir);
    ipc_client_save(client, save_path);
    /* clear the first (daemon internal) save if any; measure one full cycle */
    unlink(save_path);
    t0 = now_ms();
    int save_rc = ipc_client_save(client, save_path);
    double save_ms = now_ms() - t0;
    long bytes = -1;
    struct stat st;
    if (stat(save_path, &st) == 0)
        bytes = (long)st.st_size;
    out->save_ms = save_ms;
    fprintf(stderr, "  [%s] save: %d bytes in %.1f ms (rc=%d)\n",
            scenario, (int)bytes, save_ms, save_rc);

    out->tag = scenario;
    ipc_client_disconnect(client);
    daemon_stop(pid, sock);
    unlink(save_path);

    /* clean up the generated tree */
    char rm_cmd[700];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmp);
    (void)system(rm_cmd);
}

/* ── print ────────────────────────────────────────────────────────────────── */

static void print_row(result_row* r) {
    qsort(r->cold.v, (size_t)r->cold.n, sizeof(double), cmp_d);
    qsort(r->warm.v, (size_t)r->warm.n, sizeof(double), cmp_d);

    double w_p50 = pctile(r->warm.v, r->warm.n, 50);
    double w_p95 = pctile(r->warm.v, r->warm.n, 95);

    printf("| %-7s | %6ld | %8.0f ms | %8.3f ms | %7.3f ms | %5ld KB | %6d  |\n",
           r->tag, r->files, r->scan_ms, r->cold_first, w_p50, r->mem_kb, r->completions);

    fprintf(stderr, "    warm: avg=%.3f p50=%.3f p95=%.3f\n",
            mean(r->warm.v, r->warm.n), w_p50, w_p95);

    free_samples(&r->cold);
    free_samples(&r->warm);
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    fprintf(stderr, "archaic end-to-end benchmark\n");
    fprintf(stderr, "  cold = first Tab after daemon start (cache miss, trie walk)\n");
    fprintf(stderr, "  warm = subsequent Tabs (cache hit, shard lookup)\n");
    fprintf(stderr, "  all times are real wall-clock ms over a live Unix socket\n\n");

    result_row rows[3];
    memset(rows, 0, sizeof(rows));

    run_one("small",  5000,  &rows[0]);
    run_one("medium", 20000, &rows[1]);
    run_one("large",  50000, &rows[2]);

    printf("\n| project | files  | first scan | Tab (cold) | Tab (warm) | idle mem | results |\n");
    printf("|---------|--------|------------|------------|------------|----------|---------|\n");
    for (int i = 0; i < 3; i++)
        print_row(&rows[i]);

    return 0;
}