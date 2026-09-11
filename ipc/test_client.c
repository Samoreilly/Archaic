#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/config.h"
#include "client.h"
#include "protocol.h"

#ifndef ARCHAIC_VERSION
#define ARCHAIC_VERSION "0.9.0"
#endif

int main(int argc, char* argv[]) {
    const char* prog = argv[0];
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("archaic-cli " ARCHAIC_VERSION "\n");
        return 0;
    }

    const char* sock_override = NULL;
    if (argc >= 3 && strcmp(argv[1], "--sock") == 0) {
        sock_override = argv[2];
        argv += 2;
        argc -= 2;
        argv[0] = (char*) prog; /* keep usage/cli lines showing the binary, not the socket */
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: %s [--sock PATH] <command> [args...]\n", argv[0]);
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  scan <path>\n");
        fprintf(stderr, "  query <cwd> <input>\n");
        fprintf(stderr, "  complete <prefix> [limit] [cwd]\n");
        fprintf(stderr, "  suggest <prefix> [cwd]\n");
        fprintf(stderr, "  ping\n");
        fprintf(stderr, "  metrics\n");
        fprintf(stderr, "  scan-status\n");
        fprintf(stderr, "  fuzzy <query> [limit]\n");
        fprintf(stderr, "  stats\n");
        fprintf(stderr, "  clear-cache\n");
        fprintf(stderr, "  reindex [path]\n");
        fprintf(stderr, "  shutdown\n");
        fprintf(stderr, "  doctor\n");
        fprintf(stderr, "  explain <prefix> [cwd]\n");
        fprintf(stderr, "  watch <path>\n");
        fprintf(stderr, "  unwatch <path>\n");
        fprintf(stderr, "  roots\n");
        fprintf(stderr, "  recent [n]\n");
        return 1;
    }

    /* ── explain: why does this prefix complete (or not)? ────────────
     * Client-side only (no protocol change): checks roots, ignore rules,
     * filesystem state, daemon scanning flag, and live index results. */
    if (strcmp(argv[1], "explain") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s explain <prefix> [cwd]\n", argv[0]);
            return 1;
        }
        const char* prefix = argv[2];
        const char* cwd = argc > 3 ? argv[3] : "";
        archaic_config cfg;
        config_init_defaults(&cfg);
        config_load_default(&cfg);

        /* Resolve to absolute for root/ignore checks (mirror server join). */
        char abs_prefix[4096];
        if (prefix[0] == '/') {
            strncpy(abs_prefix, prefix, sizeof(abs_prefix) - 1);
            abs_prefix[sizeof(abs_prefix) - 1] = '\0';
        } else if (cwd[0] == '/') {
            snprintf(abs_prefix, sizeof(abs_prefix), "%s/%s", cwd, prefix);
        } else {
            strncpy(abs_prefix, prefix, sizeof(abs_prefix) - 1);
            abs_prefix[sizeof(abs_prefix) - 1] = '\0';
        }

        printf("explain %s\n", prefix);
        printf("  absolute:   %s\n", abs_prefix);

        /* Daemon live roots first: the daemon may have been started with
         * explicit scan paths that differ from this client's config file. */
        ipc_client* client =
            sock_override ? ipc_client_connect(sock_override) : ipc_client_connect_default();
        if (!client) {
            printf("  daemon:     not connected\n");
            return 1;
        }
        ipc_health_resp health;
        memset(&health, 0, sizeof(health));
        int have_health = (ipc_client_health(client, &health) == 0);

        /* Roots check (prefix itself or its parent under a root still
         * completes via filesystem fallback, but index hits need roots).
         * Checks client config roots AND the daemon's live scan roots. */
        int under_root = 0;
        const char* matched_root = NULL;
        char live_roots[10][4096];
        memset(live_roots, 0, sizeof(live_roots));
        int nlive = 0;
        const char* all_roots[32];
        int nroots = 0;
        if (cfg.daemon.scan_path[0])
            all_roots[nroots++] = cfg.daemon.scan_path;
        for (int i = 0; i < cfg.daemon.scan_path_count && nroots < 20; i++) {
            if (cfg.daemon.scan_paths[i][0])
                all_roots[nroots++] = cfg.daemon.scan_paths[i];
        }
        if (have_health) {
            for (int i = 0; i < health.scan_root_count && nlive < 10 && i < 10; i++) {
                if (!health.scan_roots[i][0])
                    continue;
                int dup = 0;
                for (int j = 0; j < nroots; j++) {
                    if (strcmp(all_roots[j], health.scan_roots[i]) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    strncpy(live_roots[nlive], health.scan_roots[i], sizeof(live_roots[0]) - 1);
                    all_roots[nroots++] = live_roots[nlive];
                    nlive++;
                }
            }
        }
        for (int i = 0; i < nroots && !under_root; i++) {
            size_t rl = strlen(all_roots[i]);
            if (rl == 0)
                continue;
            if (strncmp(abs_prefix, all_roots[i], rl) == 0 &&
                (abs_prefix[rl] == '\0' || abs_prefix[rl] == '/')) {
                under_root = 1;
                matched_root = all_roots[i];
            }
        }
        /* Also consider parent dir: completing "/root/newfile" lists parent. */
        char parent[4096];
        strncpy(parent, abs_prefix, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        size_t pl = strlen(parent);
        while (pl > 1 && parent[pl - 1] == '/')
            parent[--pl] = '\0';
        char* slash = strrchr(parent[1] ? parent + 1 : parent, '/');
        char parent_dir[4096];
        if (abs_prefix[strlen(abs_prefix) - 1] == '/') {
            strncpy(parent_dir, parent, sizeof(parent_dir) - 1);
        } else if (slash) {
            size_t dl = (size_t) (slash - parent);
            if (dl == 0)
                dl = 1;
            memcpy(parent_dir, parent, dl);
            parent_dir[dl] = '\0';
        } else {
            snprintf(parent_dir, sizeof(parent_dir), "%s",
                     cwd[0] == '/' ? cwd : ".");
        }
        parent_dir[sizeof(parent_dir) - 1] = '\0';
        int parent_under_root = under_root;
        if (!under_root) {
            for (int i = 0; i < nroots && !parent_under_root; i++) {
                size_t rl = strlen(all_roots[i]);
                if (rl == 0)
                    continue;
                if (strncmp(parent_dir, all_roots[i], rl) == 0 &&
                    (parent_dir[rl] == '\0' || parent_dir[rl] == '/'))
                    parent_under_root = 1;
            }
        }
        if (under_root && matched_root)
            printf("  under_root: yes (%s)\n", matched_root);
        else
            printf("  under_root: %s\n", under_root ? "yes" : "no");
        if (!under_root)
            printf("  hint:       outside scan roots; index misses expected, "
                   "filesystem fallback still applies; run `%s watch %s` to index\n",
                   prog, parent_dir);

        /* Ignore check: any path component matching ignore_dirs, or the
         * basename matching ignore_files, means the index skips it. */
        const char* ignored_by = NULL;
        char tmp[4096];
        strncpy(tmp, abs_prefix, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        for (char* tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
            for (int i = 0; i < cfg.scanner.ignore_dir_count; i++) {
                if (fnmatch(cfg.scanner.ignore_dirs[i], tok, 0) == 0)
                    ignored_by = cfg.scanner.ignore_dirs[i];
            }
        }
        const char* base = strrchr(abs_prefix, '/');
        base = base ? base + 1 : abs_prefix;
        if (!ignored_by && base[0]) {
            for (int i = 0; i < cfg.scanner.ignore_file_count; i++) {
                if (fnmatch(cfg.scanner.ignore_files[i], base, 0) == 0)
                    ignored_by = cfg.scanner.ignore_files[i];
            }
        }
        printf("  ignored:    %s%s%s\n", ignored_by ? "yes (" : "no",
               ignored_by ? ignored_by : "", ignored_by ? ")" : "");
        if (ignored_by)
            printf("  hint:       index skips this; filesystem fallback still "
                   "completes it if present on disk\n");

        /* Filesystem state. */
        struct stat st;
        int exists = (stat(abs_prefix, &st) == 0);
        int parent_exists = (stat(parent_dir, &st) == 0);
        printf("  on_disk:    %s\n", exists ? "yes" : "no");
        printf("  parent_dir: %s (%s)\n", parent_dir, parent_exists ? "exists" : "missing");

        /* Daemon state + live results probe (index and/or fallback). */
        if (have_health) {
            printf("  scanning:   %s\n", health.scanning ? "yes (results may be partial)" : "no");
            printf("  watcher:    %s\n", health.watcher_active ? "active" : "inactive");
        }
        ipc_completion_list* resp = calloc(1, sizeof(*resp));
        if (resp) {
            if (ipc_client_complete(client, abs_prefix, 5, cwd, 0, resp) == 0) {
                printf("  results:    %u (index and/or filesystem fallback)%s\n", resp->count,
                       resp->scanning ? " (scan in progress)" : "");
            } else {
                printf("  results:    query failed\n");
            }
            free(resp);
        }
        if (!parent_under_root)
            printf("  verdict:    outside roots — expect filesystem fallback only\n");
        else if (ignored_by)
            printf("  verdict:    ignored by index rules — expect filesystem fallback only\n");
        else if (!parent_exists)
            printf("  verdict:    parent missing on disk — no completions possible\n");
        else
            printf("  verdict:    should complete (index and/or filesystem fallback)\n");
        ipc_client_disconnect(client);
        return 0;
    }

    if (strcmp(argv[1], "watch") == 0 || strcmp(argv[1], "unwatch") == 0 ||
        strcmp(argv[1], "roots") == 0) {
        if (strcmp(argv[1], "roots") == 0) {
            archaic_config cfg;
            config_init_defaults(&cfg);
            config_load_default(&cfg);
            if (cfg.daemon.scan_path[0])
                printf("%s\n", cfg.daemon.scan_path);
            for (int i = 0; i < cfg.daemon.scan_path_count; i++)
                printf("%s\n", cfg.daemon.scan_paths[i]);
            return 0;
        }
        if (argc < 3) {
            fprintf(stderr, "Usage: %s %s <path>\n", argv[0], argv[1]);
            return 1;
        }
        int wr = strcmp(argv[1], "watch") == 0 ? config_roots_add(argv[2])
                                               : config_roots_remove(argv[2]);
        if (wr != 0) {
            fprintf(stderr, "Failed to %s %s\n", argv[1], argv[2]);
            return 1;
        }
        printf("%s %s\n", argv[1], argv[2]);
        ipc_client* client =
            sock_override ? ipc_client_connect(sock_override) : ipc_client_connect_default();
        if (client) {
            if (strcmp(argv[1], "watch") == 0)
                ipc_client_scan(client, argv[2]);
            ipc_client_disconnect(client);
        }
        return 0;
    }

    if (strcmp(argv[1], "doctor") == 0) {
        archaic_config cfg;
        config_init_defaults(&cfg);
        config_load_default(&cfg);
        /* Which file did the config actually come from? */
        const char* cfg_src = "(defaults)";
        const char* env_cfg = getenv("ARCHAIC_CONFIG");
        struct stat cfg_st;
        char home_cfg[4096] = {0};
        const char* home = getenv("HOME");
        if (env_cfg && env_cfg[0] && stat(env_cfg, &cfg_st) == 0)
            cfg_src = env_cfg;
        else if (home && home[0]) {
            snprintf(home_cfg, sizeof(home_cfg), "%s/.config/archaic/config.toml", home);
            if (stat(home_cfg, &cfg_st) == 0)
                cfg_src = home_cfg[0] ? home_cfg : "(defaults)";
            else if (stat("/etc/archaic/config.toml", &cfg_st) == 0)
                cfg_src = "/etc/archaic/config.toml";
        }
        const char* sock = sock_override ? sock_override : cfg.daemon.socket_path;
        printf("archaic doctor\n");
        printf("  cli:        %s\n", argv[0]);
        printf("  config:     %s\n", cfg_src);
        printf("  socket:     %s\n", sock);
        struct stat st;
        int sock_ok = (stat(sock, &st) == 0 && S_ISSOCK(st.st_mode));
        printf("  sock_exists:%s\n", sock_ok ? " yes" : " no");
        if (cfg.daemon.scan_path[0]) {
            int ok = (stat(cfg.daemon.scan_path, &st) == 0 && S_ISDIR(st.st_mode));
            printf("  scan_path:  %s (%s)\n", cfg.daemon.scan_path, ok ? "ok" : "MISSING");
        } else {
            printf("  scan_path:  (none)\n");
        }
        for (int i = 0; i < cfg.daemon.scan_path_count; i++) {
            int ok = (stat(cfg.daemon.scan_paths[i], &st) == 0 && S_ISDIR(st.st_mode));
            printf("  scan_paths: %s (%s)\n", cfg.daemon.scan_paths[i], ok ? "ok" : "MISSING");
        }
        /* State file age/size: proxy for last successful save. */
        char state_path[4096] = {0};
        const char* xdg_cache = getenv("XDG_CACHE_HOME");
        if (xdg_cache && xdg_cache[0])
            snprintf(state_path, sizeof(state_path), "%s/archaic/state.bin", xdg_cache);
        else if (home && home[0])
            snprintf(state_path, sizeof(state_path), "%s/.cache/archaic/state.bin", home);
        if (state_path[0] && stat(state_path, &st) == 0) {
            char ago[64];
            time_t now = time(NULL);
            long diff = (long) (now - st.st_mtime);
            if (diff < 0)
                diff = 0;
            if (diff < 90)
                snprintf(ago, sizeof(ago), "%lds ago", diff);
            else if (diff < 5400)
                snprintf(ago, sizeof(ago), "%ldm ago", diff / 60);
            else
                snprintf(ago, sizeof(ago), "%ldh ago", diff / 3600);
            printf("  state:      %s (%lld bytes, saved %s)\n", state_path,
                   (long long) st.st_size, ago);
        }
        ipc_client* client =
            sock_override ? ipc_client_connect(sock_override) : ipc_client_connect_default();
        if (!client) {
            printf("  daemon:     not connected\n");
            return 1;
        }
        uint64_t uptime_ms = 0;
        if (ipc_client_ping(client, &uptime_ms) == 0)
            printf("  ping:       ok (uptime %lu ms)\n", (unsigned long) uptime_ms);
        else
            printf("  ping:       fail\n");
        ipc_health_resp health;
        memset(&health, 0, sizeof(health));
        if (ipc_client_health(client, &health) == 0) {
            printf("  pid:        %d\n", health.daemon_pid);
            printf("  scanning:   %s\n", health.scanning ? "yes" : "no");
            uint64_t tot = health.cache_hits + health.cache_misses;
            double rate = tot ? (100.0 * (double) health.cache_hits / (double) tot) : 0.0;
            printf("  cache:      %d entries, %.1f%% hit (%lu/%lu)\n", health.cache_entries, rate,
                   (unsigned long) health.cache_hits, (unsigned long) tot);
            printf("  watcher:    %s\n", health.watcher_active ? "active" : "inactive");
            printf("  rescan:     every %us\n", health.rescan_interval);
            printf("  indexed:    %lu buckets, %lu files, %lu dirs\n",
                   (unsigned long) health.buckets_indexed, (unsigned long) health.files_scanned,
                   (unsigned long) health.dirs_scanned);
            printf("  rss:        %lu bytes\n", (unsigned long) health.estimated_memory_bytes);
            printf("  protocol:   %d\n", health.protocol_version);
            printf("  daemon_sock:%s\n", health.socket_path);
            if (health.scan_root_count > 0) {
                printf("  roots:      %d\n", health.scan_root_count);
                for (int i = 0; i < health.scan_root_count && i < 10; i++)
                    printf("    [%d] %s\n", i, health.scan_roots[i]);
            }
            printf("  tip:        run `%s explain <prefix> [cwd]` to debug empty completions\n",
                   argv[0]);
        } else {
            printf("  health:     fail\n");
        }
        ipc_client_disconnect(client);
        return 0;
    }

    ipc_client* client =
        sock_override ? ipc_client_connect(sock_override) : ipc_client_connect_default();
    if (!client) {
        fprintf(stderr, "Failed to connect to daemon\n");
        return 1;
    }

    int rc = 0;

    if (strcmp(argv[1], "scan") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s scan <path>\n", argv[0]);
            rc = 1;
            goto done;
        }
        rc = ipc_client_scan(client, argv[2]);
        if (rc == 0) {
            printf("Scan started for: %s\n", argv[2]);
        } else {
            fprintf(stderr, "Scan failed\n");
        }
    } else if (strcmp(argv[1], "query") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s query <cwd> <input>\n", argv[0]);
            rc = 1;
            goto done;
        }
        ipc_validation_resp resp;
        rc = ipc_client_query(client, argv[2], argv[3], &resp);
        if (rc == 0) {
            printf("exists: %s\n", resp.exists ? "yes" : "no");
            printf("is_dir: %s\n", resp.is_dir ? "yes" : "no");
            printf("is_file: %s\n", resp.is_file ? "yes" : "no");
            printf("path: %s\n", resp.full_path);
        } else {
            fprintf(stderr, "Query failed\n");
        }
    } else if (strcmp(argv[1], "complete") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s complete <prefix> [limit] [cwd]\n", argv[0]);
            rc = 1;
            goto done;
        }
        uint32_t limit = argc > 3 ? (uint32_t) atoi(argv[3]) : 10;
        const char* cwd = argc > 4 ? argv[4] : "";
        ipc_completion_list* resp = calloc(1, sizeof(*resp));
        int dirs_only = argc > 5 ? atoi(argv[5]) : 0;
        rc = resp ? ipc_client_complete(client, argv[2], limit, cwd, dirs_only, resp) : -1;
        if (rc == 0) {
            if (resp->scanning)
                printf("#scanning\n");
            for (uint32_t i = 0; i < resp->count; i++) {
                printf("%c %s\n", resp->is_dirs[i] ? 'D' : 'F', resp->paths[i]);
            }
        } else {
            fprintf(stderr, "Completions failed\n");
        }
        free(resp);
    } else if (strcmp(argv[1], "suggest") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s suggest <prefix> [cwd]\n", argv[0]);
            rc = 1;
            goto done;
        }
        const char* suggest_cwd = argc > 3 ? argv[3] : "";
        ipc_suggestion_resp resp;
        rc = ipc_client_suggest(client, argv[2], suggest_cwd, &resp);
        if (rc == 0 && resp.path[0] != '\0') {
            printf("%s", resp.path);
        } else {
            rc = 1;
        }
    } else if (strcmp(argv[1], "complete-raw") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s complete-raw <prefix> [limit] [cwd]\n", argv[0]);
            rc = 1;
            goto done;
        }
        uint32_t raw_limit = argc > 3 ? (uint32_t) atoi(argv[3]) : 10;
        const char* raw_cwd = argc > 4 ? argv[4] : "";
        ipc_completion_list* resp = calloc(1, sizeof(*resp));
        int dirs_only = 0;
        rc = resp ? ipc_client_complete(client, argv[2], raw_limit, raw_cwd, dirs_only, resp) : -1;
        if (rc == 0) {
            printf("Found %u completions:\n", resp->count);
            for (uint32_t i = 0; i < resp->count; i++) {
                printf("  [%d] score=%.4f dir=%s  %s\n", i, resp->scores[i],
                       resp->is_dirs[i] ? "yes" : "no", resp->paths[i]);
            }
        } else {
            fprintf(stderr, "Completions failed\n");
        }
        free(resp);
    } else if (strcmp(argv[1], "save") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s save <path>\n", argv[0]);
            rc = 1;
            goto done;
        }
        rc = ipc_client_save(client, argv[2]);
        if (rc == 0) {
            printf("State saved to: %s\n", argv[2]);
        } else {
            fprintf(stderr, "Save failed\n");
        }
    } else if (strcmp(argv[1], "shutdown") == 0) {
        rc = ipc_client_shutdown(client);
        if (rc == 0) {
            printf("Daemon shutdown requested\n");
        } else {
            fprintf(stderr, "Shutdown failed\n");
        }
    } else if (strcmp(argv[1], "ping") == 0) {
        uint64_t uptime_ms = 0;
        rc = ipc_client_ping(client, &uptime_ms);
        if (rc == 0) {
            printf("PONG: uptime=%lu ms\n", (unsigned long) uptime_ms);
        } else {
            fprintf(stderr, "Ping failed\n");
        }
    } else if (strcmp(argv[1], "metrics") == 0) {
        ipc_metrics_resp resp;
        rc = ipc_client_metrics(client, &resp);
        if (rc == 0) {
            printf("queries_total:       %lu\n", (unsigned long) resp.queries_total);
            printf("completions_total:   %lu\n", (unsigned long) resp.completions_total);
            printf("scans_total:         %lu\n", (unsigned long) resp.scans_total);
            printf("errors_total:        %lu\n", (unsigned long) resp.errors_total);
            printf("cache_hits:          %lu\n", (unsigned long) resp.cache_hits);
            printf("cache_misses:        %lu\n", (unsigned long) resp.cache_misses);
            printf("query_latency_avg_ms: %.3f\n", resp.query_latency_avg_ms);
        } else {
            fprintf(stderr, "Metrics failed\n");
        }
    } else if (strcmp(argv[1], "scan-status") == 0) {
        ipc_scan_status_resp resp;
        rc = ipc_client_scan_status(client, &resp);
        if (rc == 0) {
            printf("scanning: %s\n", resp.scanning ? "yes" : "no");
            printf("buckets: %lu\n", (unsigned long) resp.buckets_so_far);
        } else {
            fprintf(stderr, "Scan status failed\n");
        }
    } else if (strcmp(argv[1], "fuzzy") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s fuzzy <query> [limit]\n", argv[0]);
            rc = 1;
            goto done;
        }
        uint32_t limit = argc > 3 ? (uint32_t) atoi(argv[3]) : 20;
        ipc_completion_list* resp = calloc(1, sizeof(*resp));
        rc = resp ? ipc_client_fuzzy(client, argv[2], limit, resp) : -1;
        if (rc == 0) {
            for (uint32_t i = 0; i < resp->count; i++) {
                printf("%c %s\n", resp->is_dirs[i] ? 'D' : 'F', resp->paths[i]);
            }
        } else {
            fprintf(stderr, "Fuzzy completions failed\n");
        }
        free(resp);
    } else if (strcmp(argv[1], "select") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s select <path>\n", argv[0]);
            rc = 1;
            goto done;
        }
        rc = ipc_client_select(client, argv[2]);
        if (rc != 0)
            fprintf(stderr, "Select failed\n");
    } else if (strcmp(argv[1], "stats") == 0) {
        ipc_metrics_resp resp;
        rc = ipc_client_metrics(client, &resp);
        if (rc == 0) {
            printf("archaic daemon statistics\n");
            printf("─────────────────────────────\n");
            printf("queries_total:       %lu\n", (unsigned long) resp.queries_total);
            printf("completions_total:   %lu\n", (unsigned long) resp.completions_total);
            printf("scans_total:         %lu\n", (unsigned long) resp.scans_total);
            printf("errors_total:        %lu\n", (unsigned long) resp.errors_total);
            printf("cache_hits:          %lu\n", (unsigned long) resp.cache_hits);
            printf("cache_misses:         %lu\n", (unsigned long) resp.cache_misses);
            double hit_rate =
                (resp.cache_hits + resp.cache_misses > 0)
                    ? (double) resp.cache_hits / (resp.cache_hits + resp.cache_misses) * 100.0
                    : 0.0;
            printf("cache_hit_rate:      %.1f%%\n", hit_rate);
            printf("query_latency_avg:   %.3f ms\n", resp.query_latency_avg_ms);
            printf("paths_indexed:       %lu\n", (unsigned long) resp.total_paths_indexed);
            printf("dirs_indexed:        %lu\n", (unsigned long) resp.total_dirs_indexed);
        } else {
            fprintf(stderr, "Stats failed\n");
        }
    } else if (strcmp(argv[1], "clear-cache") == 0) {
        ipc_scan_req req;
        memset(&req, 0, sizeof(req));
        strncpy(req.path, "__clear_cache__", sizeof(req.path) - 1);
        if (ipc_client_scan(client, req.path) == 0) {
            printf("Cache cleared.\n");
        } else {
            fprintf(stderr, "Failed to clear cache (daemon may not support this)\n");
            rc = 1;
        }
    } else if (strcmp(argv[1], "reindex") == 0) {
        const char* reindex_path = (argc > 2) ? argv[2] : "/";
        rc = ipc_client_scan(client, reindex_path);
        if (rc == 0) {
            printf("Reindex started for: %s\n", reindex_path);
        } else {
            fprintf(stderr, "Reindex failed\n");
        }
    } else if (strcmp(argv[1], "bookmarks") == 0) {
        uint32_t limit = (argc > 2) ? (uint32_t) atoi(argv[2]) : 50;
        ipc_bookmarks_resp resp;
        memset(&resp, 0, sizeof(resp));
        rc = ipc_client_bookmarks(client, limit, &resp);
        if (rc == 0) {
            printf("Bookmarks (%u):\n", resp.count);
            for (uint32_t i = 0; i < resp.count; i++) {
                printf("  %s\n", resp.paths[i]);
            }
        } else {
            fprintf(stderr, "Failed to get bookmarks\n");
        }
    } else if (strcmp(argv[1], "recent") == 0) {
        uint32_t n = argc > 2 ? (uint32_t) atoi(argv[2]) : 10;
        ipc_recent_resp resp;
        memset(&resp, 0, sizeof(resp));
        rc = ipc_client_recent(client, n, &resp);
        if (rc == 0) {
            for (uint32_t i = 0; i < resp.count; i++) {
                printf("%c %s\n", resp.is_dirs[i] ? 'D' : 'F', resp.paths[i]);
            }
        } else {
            fprintf(stderr, "Recent failed\n");
        }
    } else if (strcmp(argv[1], "health") == 0) {
        ipc_health_resp resp;
        memset(&resp, 0, sizeof(resp));
        rc = ipc_client_health(client, &resp);
        if (rc == 0) {
            printf("Daemon Health:\n");
            printf("  Running: %s\n", resp.daemon_running ? "yes" : "no");
            printf("  Scanning: %s\n", resp.scanning ? "yes" : "no");
            printf("  Watcher: %s\n", resp.watcher_active ? "active" : "inactive");
            printf("  Cache entries: %d\n", resp.cache_entries);
            printf("  Buckets indexed: %lu\n", (unsigned long) resp.buckets_indexed);
            printf("  Files scanned: %lu\n", (unsigned long) resp.files_scanned);
            printf("  Dirs scanned: %lu\n", (unsigned long) resp.dirs_scanned);
            printf("  Queries total: %lu\n", (unsigned long) resp.queries_total);
            printf("  Cache hits: %lu\n", (unsigned long) resp.cache_hits);
            printf("  Cache misses: %lu\n", (unsigned long) resp.cache_misses);
            printf("  Rescan interval: %us\n", resp.rescan_interval);
            printf("  Bookmarks: %u\n", resp.bookmark_count);
            printf("  Recent files: %u\n", resp.recent_count);
            printf("  Uptime: %lus\n", (unsigned long) resp.uptime_seconds);
            printf("  Est. memory: %lu bytes\n", (unsigned long) resp.estimated_memory_bytes);
            printf("  PID: %d\n", resp.daemon_pid);
            printf("  Socket: %s\n", resp.socket_path);
            printf("  Active connections: %d\n", resp.active_connections);
            if (resp.scanning_progress_pct > 0)
                printf("  Scan progress: %d%%\n", resp.scanning_progress_pct);
            if (resp.scan_root_count > 0) {
                printf("  Scan roots (%d):\n", resp.scan_root_count);
                for (int i = 0; i < resp.scan_root_count && i < 10; i++) {
                    printf("    [%d] %s\n", i, resp.scan_roots[i]);
                }
            }
            printf("  Protocol version: %d\n", resp.protocol_version);
        } else {
            fprintf(stderr, "Failed to get health info\n");
        }
    } else if (strcmp(argv[1], "reload") == 0) {
        rc = ipc_client_reload(client);
        if (rc == 0) {
            printf("Config reload requested (send SIGHUP to daemon to apply)\n");
        } else {
            fprintf(stderr, "Reload failed\n");
        }
    } else if (strcmp(argv[1], "reset-stats") == 0) {
        rc = ipc_client_reset_stats(client);
        if (rc == 0) {
            printf("Stats reset complete\n");
        } else {
            fprintf(stderr, "Reset failed\n");
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        rc = 1;
    }

done:
    ipc_client_disconnect(client);
    return rc;
}
