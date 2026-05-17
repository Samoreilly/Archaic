/*
 * archaic-helper: Persistent helper binary for fast fish shell integration.
 *
 * Maintains a persistent Unix socket connection to the archaic daemon.
 * Reads commands from stdin, writes responses to stdout.
 * Handles daemon disconnection with automatic reconnect.
 *
 * Usage:
 *   archaic-helper [socket_path]
 *
 * Commands (one per line from stdin):
 *   complete <prefix> [limit]   - Path completions
 *   suggest <prefix>            - Single best suggestion
 *   query <cwd> <input>         - Path validation
 *   ping                        - Daemon liveness check
 *   metrics                     - Daemon statistics
 *   scan-status                 - Scan progress
 *   quit / exit                 - Terminate helper
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include "ipc/protocol.h"
#include "config.h"

/* ------------------------------------------------------------------ */
/* Signal handling                                                     */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/* Persistent connection state                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    uint32_t req_id;
    char sock_path[256];
} helper_conn;

static void helper_conn_init(helper_conn* conn) {
    conn->fd = -1;
    conn->req_id = 1;
    conn->sock_path[0] = '\0';
}

static int helper_connect(helper_conn* conn) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "helper: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, conn->sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "helper: connect(%s) failed: %s\n",
                conn->sock_path, strerror(errno));
        close(fd);
        return -1;
    }

    conn->fd = fd;
    conn->req_id = 1;
    return 0;
}

static void helper_disconnect(helper_conn* conn) {
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}

static int helper_ensure_connected(helper_conn* conn) {
    if (conn->fd >= 0) return 0;
    return helper_connect(conn);
}

/* ------------------------------------------------------------------ */
/* Low-level I/O                                                       */
/* ------------------------------------------------------------------ */

static int read_exact(int fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char*)buf + total, len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int write_exact(int fd, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char*)buf + total, len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int send_request(helper_conn* conn, uint32_t type,
                        const void* payload, size_t len) {
    ipc_header hdr;
    ipc_write_header(&hdr, type, (uint32_t)len, conn->req_id++);
    if (write_exact(conn->fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (payload && len > 0) {
        if (write_exact(conn->fd, payload, len) < 0) return -1;
    }
    return 0;
}

static int recv_response(helper_conn* conn, ipc_header* hdr,
                         void* payload, size_t max_len) {
    if (read_exact(conn->fd, hdr, sizeof(*hdr)) < 0) return -1;
    if (!ipc_validate_header(hdr)) return -1;
    if (hdr->payload_len > max_len) return -1;
    if (hdr->payload_len > 0) {
        if (read_exact(conn->fd, payload, hdr->payload_len) < 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static int cmd_complete(helper_conn* conn, const char* prefix, uint32_t limit) {
    if (helper_ensure_connected(conn) < 0) return -1;

    ipc_complete_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.prefix, prefix, sizeof(req.prefix) - 1);
    req.limit = limit;

    if (send_request(conn, IPC_MSG_COMPLETE, &req, sizeof(req)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    ipc_header hdr;
    ipc_completions_resp resp;
    if (recv_response(conn, &hdr, &resp, sizeof(resp)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    if (hdr.msg_type == IPC_MSG_COMPLETIONS) {
        for (uint32_t i = 0; i < resp.count; i++) {
            printf("%c %s\n", resp.is_dirs[i] ? 'D' : 'F', resp.paths[i]);
        }
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memcpy(&err, &resp, sizeof(err));
        fprintf(stderr, "helper: complete error: %s\n", err.message);
        return -1;
    }
    return 0;
}

static int cmd_suggest(helper_conn* conn, const char* prefix) {
    if (helper_ensure_connected(conn) < 0) return -1;

    ipc_suggest_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.prefix, prefix, sizeof(req.prefix) - 1);

    if (send_request(conn, IPC_MSG_SUGGEST, &req, sizeof(req)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    ipc_header hdr;
    ipc_suggestion_resp resp;
    if (recv_response(conn, &hdr, &resp, sizeof(resp)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    if (hdr.msg_type == IPC_MSG_SUGGESTION) {
        printf("%s\n", resp.path);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memcpy(&err, &resp, sizeof(err));
        fprintf(stderr, "helper: suggest error: %s\n", err.message);
        return -1;
    }
    return 0;
}

static int cmd_query(helper_conn* conn, const char* cwd, const char* input) {
    if (helper_ensure_connected(conn) < 0) return -1;

    ipc_query_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.cwd, cwd, sizeof(req.cwd) - 1);
    strncpy(req.input, input, sizeof(req.input) - 1);

    if (send_request(conn, IPC_MSG_QUERY, &req, sizeof(req)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    ipc_header hdr;
    ipc_validation_resp resp;
    if (recv_response(conn, &hdr, &resp, sizeof(resp)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    if (hdr.msg_type == IPC_MSG_VALIDATION) {
        printf("%d %d %d %s\n", resp.exists, resp.is_dir, resp.is_file, resp.full_path);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memcpy(&err, &resp, sizeof(err));
        fprintf(stderr, "helper: query error: %s\n", err.message);
        return -1;
    }
    return 0;
}

static int cmd_ping(helper_conn* conn) {
    if (helper_ensure_connected(conn) < 0) return -1;

    if (send_request(conn, IPC_MSG_PING, NULL, 0) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    ipc_header hdr;
    ipc_pong_resp resp;
    if (recv_response(conn, &hdr, &resp, sizeof(resp)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    if (hdr.msg_type == IPC_MSG_PONG) {
        printf("%lu\n", (unsigned long)resp.uptime_ms);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memcpy(&err, &resp, sizeof(err));
        fprintf(stderr, "helper: ping error: %s\n", err.message);
        return -1;
    }
    return 0;
}

static int cmd_metrics(helper_conn* conn) {
    if (helper_ensure_connected(conn) < 0) return -1;

    if (send_request(conn, IPC_MSG_METRICS, NULL, 0) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    ipc_header hdr;
    ipc_metrics_resp resp;
    if (recv_response(conn, &hdr, &resp, sizeof(resp)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    if (hdr.msg_type == IPC_MSG_METRICS_RESP) {
        printf("queries_total=%lu\n", (unsigned long)resp.queries_total);
        printf("completions_total=%lu\n", (unsigned long)resp.completions_total);
        printf("scans_total=%lu\n", (unsigned long)resp.scans_total);
        printf("errors_total=%lu\n", (unsigned long)resp.errors_total);
        printf("cache_hits=%lu\n", (unsigned long)resp.cache_hits);
        printf("cache_misses=%lu\n", (unsigned long)resp.cache_misses);
        printf("query_latency_avg_ms=%.2f\n", resp.query_latency_avg_ms);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memcpy(&err, &resp, sizeof(err));
        fprintf(stderr, "helper: metrics error: %s\n", err.message);
        return -1;
    }
    return 0;
}

static int cmd_scan_status(helper_conn* conn) {
    if (helper_ensure_connected(conn) < 0) return -1;

    if (send_request(conn, IPC_MSG_SCAN_STATUS, NULL, 0) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    ipc_header hdr;
    ipc_scan_status_resp resp;
    if (recv_response(conn, &hdr, &resp, sizeof(resp)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    if (hdr.msg_type == IPC_MSG_SCAN_STATUS_RESP) {
        printf("scanning %d buckets_so_far=%lu\n",
               resp.scanning, (unsigned long)resp.buckets_so_far);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memcpy(&err, &resp, sizeof(err));
        fprintf(stderr, "helper: scan-status error: %s\n", err.message);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int main(int argc, char* argv[]) {
    /* Determine socket path: CLI arg > config > default */
    const char* sock_path = IPC_SOCK_PATH;

    if (argc > 1) {
        sock_path = argv[1];
    } else {
        archaic_config cfg;
        config_init_defaults(&cfg);
        if (config_load_default(&cfg) == 0 && cfg.daemon.socket_path[0] != '\0') {
            sock_path = cfg.daemon.socket_path;
        }
    }

    helper_conn conn;
    helper_conn_init(&conn);
    strncpy(conn.sock_path, sock_path, sizeof(conn.sock_path) - 1);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    char line[8192];
    while (running && fgets(line, sizeof(line), stdin)) {
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        /* Parse command name */
        char cmd[64] = {0};
        if (sscanf(line, "%63s", cmd) < 1) continue;

        int rc = -1;

        if (strcmp(cmd, "complete") == 0) {
            char prefix[4096] = {0};
            uint32_t limit = 50;
            int n = sscanf(line, "%*s %4095s %u", prefix, &limit);
            if (n >= 1) {
                rc = cmd_complete(&conn, prefix, limit);
            }
        } else if (strcmp(cmd, "suggest") == 0) {
            char prefix[4096] = {0};
            if (sscanf(line, "%*s %4095s", prefix) == 1) {
                rc = cmd_suggest(&conn, prefix);
            }
        } else if (strcmp(cmd, "query") == 0) {
            char cwd[4096] = {0};
            char input[4096] = {0};
            /* query <cwd> <input> - input may contain spaces */
            if (sscanf(line, "%*s %4095s", cwd) == 1) {
                /* Find the input after "query <cwd> " */
                const char* p = line + 5; /* skip "query" */
                while (*p == ' ') p++;
                while (*p && *p != ' ') p++; /* skip cwd */
                while (*p == ' ') p++;
                if (*p) {
                    strncpy(input, p, sizeof(input) - 1);
                }
                rc = cmd_query(&conn, cwd, input);
            }
        } else if (strcmp(cmd, "ping") == 0) {
            rc = cmd_ping(&conn);
        } else if (strcmp(cmd, "metrics") == 0) {
            rc = cmd_metrics(&conn);
        } else if (strcmp(cmd, "scan-status") == 0) {
            rc = cmd_scan_status(&conn);
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else {
            fprintf(stderr, "helper: unknown command: %s\n", cmd);
            rc = 0; /* not a fatal error */
        }

        if (rc < 0) {
            fprintf(stderr, "helper: command '%s' failed\n", cmd);
        }
        fflush(stdout);
    }

    helper_disconnect(&conn);
    return 0;
}
