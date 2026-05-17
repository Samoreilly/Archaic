#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "protocol.h"

int main(int argc, char* argv[]) {
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
        rc = ipc_client_complete(client, argv[2], limit, cwd, &resp);
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
        rc = ipc_client_complete(client, argv[2], raw_limit, raw_cwd, &resp);
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
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        rc = 1;
    }

done:
    ipc_client_disconnect(client);
    return rc;
}
