#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "protocol.h"

#ifndef ARCHAIC_VERSION
#define ARCHAIC_VERSION "0.9.0"
#endif

int main(int argc, char* argv[]) {
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("archaic-cli " ARCHAIC_VERSION "\n");
        return 0;
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
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
        return 1;
    }

    ipc_client* client = ipc_client_connect_default();
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
        ipc_completions_resp resp;
        int dirs_only = 0;
        rc = ipc_client_complete(client, argv[2], limit, cwd, dirs_only, &resp);
        if (rc == 0) {
            for (uint32_t i = 0; i < resp.count; i++) {
                printf("%c %s\n", resp.is_dirs[i] ? 'D' : 'F', resp.paths[i]);
            }
        } else {
            fprintf(stderr, "Completions failed\n");
        }
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
        ipc_completions_resp resp;
        int dirs_only = 0;
        rc = ipc_client_complete(client, argv[2], raw_limit, raw_cwd, dirs_only, &resp);
        if (rc == 0) {
            printf("Found %u completions:\n", resp.count);
            for (uint32_t i = 0; i < resp.count; i++) {
                printf("  [%d] score=%.4f freq=%lu dir=%s  %s\n", i, resp.scores[i],
                       (unsigned long) resp.freqs[i], resp.is_dirs[i] ? "yes" : "no",
                       resp.paths[i]);
            }
        } else {
            fprintf(stderr, "Completions failed\n");
        }
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
        ipc_completions_resp resp;
        rc = ipc_client_fuzzy(client, argv[2], limit, &resp);
        if (rc == 0) {
            for (uint32_t i = 0; i < resp.count; i++) {
                printf("%c %s\n", resp.is_dirs[i] ? 'D' : 'F', resp.paths[i]);
            }
        } else {
            fprintf(stderr, "Fuzzy completions failed\n");
        }
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
        } else {
            fprintf(stderr, "Failed to get health info\n");
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        rc = 1;
    }

done:
    ipc_client_disconnect(client);
    return rc;
}
