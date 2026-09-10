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
 *   complete <prefix> [limit] [cwd] - Path completions (CWD boosts nearby paths)
 *   fuzzy <prefix> [limit]          - Fuzzy path completions
 *   suggest <prefix> [cwd]         - Single best suggestion
 *   query <cwd> <input>            - Path validation
 *   ping                        - Daemon liveness check
 *   metrics                     - Daemon statistics
 *   scan-status                 - Scan progress
 *   quit / exit                 - Terminate helper
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../ipc/protocol.h"
#include "config.h"
#include "src/scanner.h"

/* ------------------------------------------------------------------ */
/* Path abbreviation support                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Color output support                                                */
/* ------------------------------------------------------------------ */

/* Helper NEVER emits ANSI colors. Shell plugins handle all display coloring.
 * Fish captures stdout literally in completions, so escape codes appear as
 * raw text (e.g. ␛[34;1m). Always output plain text from the helper. */
static volatile int g_color_enabled = 0;

static void check_color_env(void) {
    /* Force-disable: helper must never emit ANSI regardless of environment.
     * This function exists as a safety net — g_color_enabled starts at 0
     * and no code path should re-enable it. */
    g_color_enabled = 0;
}

#define COLOR_DIR "\033[1;34m"    /* Bold blue */
#define COLOR_EXEC "\033[32m"     /* Green */
#define COLOR_HIDDEN "\033[2;37m" /* Dim white */
#define COLOR_RESET "\033[0m"

static void print_colored(const char* type, const char* path) {
    printf("%c %s\n", type[0], path);
}

/* ------------------------------------------------------------------ */
/* Signal handling                                                     */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void) sig;
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

    if (connect(fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        fprintf(stderr, "helper: connect(%s) failed: %s\n", conn->sock_path, strerror(errno));
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
    if (conn->fd >= 0)
        return 0;
    return helper_connect(conn);
}

/* ------------------------------------------------------------------ */
/* Low-level I/O                                                       */
/* ------------------------------------------------------------------ */

static int read_exact(int fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char*) buf + total, len - total);
        if (n <= 0)
            return -1;
        total += (size_t) n;
    }
    return 0;
}

static int write_exact(int fd, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char*) buf + total, len - total);
        if (n <= 0)
            return -1;
        total += (size_t) n;
    }
    return 0;
}

static int send_request(helper_conn* conn, uint32_t type, const void* payload, size_t len) {
    ipc_header hdr;
    ipc_write_header(&hdr, type, (uint32_t) len, conn->req_id++);
    if (write_exact(conn->fd, &hdr, sizeof(hdr)) < 0)
        return -1;
    if (payload && len > 0) {
        if (write_exact(conn->fd, payload, len) < 0)
            return -1;
    }
    return 0;
}

static int recv_response(helper_conn* conn, ipc_header* hdr, void* payload, size_t max_len) {
    if (read_exact(conn->fd, hdr, sizeof(*hdr)) < 0)
        return -1;
    if (!ipc_validate_header(hdr))
        return -1;
    if (hdr->payload_len > max_len)
        return -1;
    if (hdr->payload_len > 0) {
        if (read_exact(conn->fd, payload, hdr->payload_len) < 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static int cmd_complete(helper_conn* conn, const char* prefix, uint32_t limit, const char* cwd,
                        int dirs_only) {
    if (helper_ensure_connected(conn) < 0)
        return -1;

    ipc_complete_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.prefix, prefix, sizeof(req.prefix) - 1);
    strncpy(req.cwd, cwd, sizeof(req.cwd) - 1);
    req.dirs_only = dirs_only ? 1 : 0;
    req.limit = limit;

    if (send_request(conn, IPC_MSG_COMPLETE, &req, sizeof(req)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    ipc_header hdr;
    if (read_exact(conn->fd, &hdr, sizeof(hdr)) < 0) {
        helper_disconnect(conn);
        return -1;
    }
    if (!ipc_validate_header(&hdr)) {
        helper_disconnect(conn);
        return -1;
    }
    uint8_t* buf = malloc(hdr.payload_len ? hdr.payload_len : 1);
    if (!buf) {
        helper_disconnect(conn);
        return -1;
    }
    if (hdr.payload_len > 0 && read_exact(conn->fd, buf, hdr.payload_len) < 0) {
        free(buf);
        helper_disconnect(conn);
        return -1;
    }
    int rc = 0;
    if (hdr.msg_type == IPC_MSG_COMPLETIONS) {
        ipc_completion_list* list = calloc(1, sizeof(*list));
        if (list && ipc_unpack_completions(buf, hdr.payload_len, list) == 0) {
            if (list->scanning)
                printf("#scanning\n");
            for (uint32_t i = 0; i < list->count; i++)
                print_colored(list->is_dirs[i] ? "D" : "F", list->paths[i]);
        } else {
            rc = -1;
        }
        free(list);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memset(&err, 0, sizeof(err));
        memcpy(&err, buf, hdr.payload_len < sizeof(err) ? hdr.payload_len : sizeof(err));
        fprintf(stderr, "helper: complete error: %s\n", err.message);
        rc = -1;
    }
    free(buf);
    return rc;
}

static int cmd_suggest(helper_conn* conn, const char* prefix, const char* cwd) {
    if (helper_ensure_connected(conn) < 0)
        return -1;

    ipc_suggest_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.prefix, prefix, sizeof(req.prefix) - 1);
    strncpy(req.cwd, cwd, sizeof(req.cwd) - 1);

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
    if (helper_ensure_connected(conn) < 0)
        return -1;

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
    if (helper_ensure_connected(conn) < 0)
        return -1;

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
        printf("%lu\n", (unsigned long) resp.uptime_ms);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memcpy(&err, &resp, sizeof(err));
        fprintf(stderr, "helper: ping error: %s\n", err.message);
        return -1;
    }
    return 0;
}

static int cmd_metrics(helper_conn* conn) {
    if (helper_ensure_connected(conn) < 0)
        return -1;

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
        printf("queries_total=%lu\n", (unsigned long) resp.queries_total);
        printf("completions_total=%lu\n", (unsigned long) resp.completions_total);
        printf("scans_total=%lu\n", (unsigned long) resp.scans_total);
        printf("errors_total=%lu\n", (unsigned long) resp.errors_total);
        printf("cache_hits=%lu\n", (unsigned long) resp.cache_hits);
        printf("cache_misses=%lu\n", (unsigned long) resp.cache_misses);
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
    if (helper_ensure_connected(conn) < 0)
        return -1;

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
        printf("scanning %d buckets_so_far=%lu\n", resp.scanning,
               (unsigned long) resp.buckets_so_far);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memcpy(&err, &resp, sizeof(err));
        fprintf(stderr, "helper: scan-status error: %s\n", err.message);
        return -1;
    }
    return 0;
}

static int cmd_fuzzy(helper_conn* conn, const char* prefix, uint32_t limit) {
    if (helper_ensure_connected(conn) < 0)
        return -1;

    ipc_complete_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.prefix, prefix, sizeof(req.prefix) - 1);
    req.limit = limit;

    if (send_request(conn, IPC_MSG_FUZZY_COMPLETE, &req, sizeof(req)) < 0) {
        helper_disconnect(conn);
        return -1;
    }

    ipc_header hdr;
    if (read_exact(conn->fd, &hdr, sizeof(hdr)) < 0) {
        helper_disconnect(conn);
        return -1;
    }
    if (!ipc_validate_header(&hdr)) {
        helper_disconnect(conn);
        return -1;
    }
    uint8_t* buf = malloc(hdr.payload_len ? hdr.payload_len : 1);
    if (!buf) {
        helper_disconnect(conn);
        return -1;
    }
    if (hdr.payload_len > 0 && read_exact(conn->fd, buf, hdr.payload_len) < 0) {
        free(buf);
        helper_disconnect(conn);
        return -1;
    }
    int rc = 0;
    if (hdr.msg_type == IPC_MSG_FUZZY_COMPLETIONS) {
        ipc_completion_list* list = calloc(1, sizeof(*list));
        if (list && ipc_unpack_completions(buf, hdr.payload_len, list) == 0) {
            if (list->scanning)
                printf("#scanning\n");
            for (uint32_t i = 0; i < list->count; i++)
                print_colored(list->is_dirs[i] ? "D" : "F", list->paths[i]);
        } else {
            rc = -1;
        }
        free(list);
    } else if (hdr.msg_type == IPC_MSG_ERROR) {
        ipc_error_resp err;
        memset(&err, 0, sizeof(err));
        memcpy(&err, buf, hdr.payload_len < sizeof(err) ? hdr.payload_len : sizeof(err));
        fprintf(stderr, "helper: fuzzy error: %s\n", err.message);
        rc = -1;
    }
    free(buf);
    return rc;
}

static int cmd_select(helper_conn* conn, const char* path) {
    if (helper_ensure_connected(conn) < 0)
        return -1;
    ipc_select_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.path, path, sizeof(req.path) - 1);
    if (send_request(conn, IPC_MSG_SELECT, &req, sizeof(req)) < 0) {
        helper_disconnect(conn);
        return -1;
    }
    ipc_header hdr;
    ipc_ok_resp resp;
    if (recv_response(conn, &hdr, &resp, sizeof(resp)) < 0) {
        helper_disconnect(conn);
        return -1;
    }
    return hdr.msg_type == IPC_MSG_OK ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int main(int argc, char* argv[]) {
    const char* sock_path = NULL;
    const char* cmd_fifo = NULL;
    const char* out_fifo = NULL;
    int fifo_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("archaic-helper 0.9.0\n");
            return 0;
        }
        if (strcmp(argv[i], "--cmd-fifo") == 0 && i + 1 < argc) {
            cmd_fifo = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--out-fifo") == 0 && i + 1 < argc) {
            out_fifo = argv[++i];
            continue;
        }
        if (argv[i][0] != '-')
            sock_path = argv[i];
    }

    if (!sock_path) {
        sock_path = IPC_SOCK_PATH;
        archaic_config cfg;
        config_init_defaults(&cfg);
        if (config_load_default(&cfg) == 0 && cfg.daemon.socket_path[0] != '\0') {
            sock_path = cfg.daemon.socket_path;
        }
    }

    if (cmd_fifo) {
        int fd = open(cmd_fifo, O_RDWR);
        if (fd < 0)
            return 1;
        if (dup2(fd, STDIN_FILENO) < 0)
            return 1;
        if (fd != STDIN_FILENO)
            close(fd);
        fifo_mode = 1;
    }
    if (out_fifo) {
        int fd = open(out_fifo, O_RDWR);
        if (fd < 0)
            return 1;
        if (dup2(fd, STDOUT_FILENO) < 0)
            return 1;
        if (fd != STDOUT_FILENO)
            close(fd);
        fifo_mode = 1;
    }

    check_color_env();

    helper_conn conn;
    helper_conn_init(&conn);
    strncpy(conn.sock_path, sock_path, sizeof(conn.sock_path) - 1);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    char line[8192];
    while (running && fgets(line, sizeof(line), stdin)) {
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0')
            continue;

        /* Parse command name */
        char cmd[64] = {0};
        if (sscanf(line, "%63s", cmd) < 1)
            continue;

        int rc = -1;

        if (strcmp(cmd, "complete") == 0) {
            char prefix[4096] = {0};
            char cwd_path[4096] = {0};
            char dirs_only_str[8] = {0};
            uint32_t limit = 50;
            int dirs_only = 0;
            if (strchr(line, '\t')) {
                char* save = NULL;
                char buf[8192];
                strncpy(buf, line, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                strtok_r(buf, "\t", &save);
                char* d_tok = strtok_r(NULL, "\t", &save);
                char* l_tok = strtok_r(NULL, "\t", &save);
                char* c_tok = strtok_r(NULL, "\t", &save);
                char* p_tok = strtok_r(NULL, "\t", &save);
                if (d_tok && (strcmp(d_tok, "1") == 0 || strcmp(d_tok, "true") == 0))
                    dirs_only = 1;
                if (l_tok)
                    limit = (uint32_t) atoi(l_tok);
                if (c_tok)
                    strncpy(cwd_path, c_tok, sizeof(cwd_path) - 1);
                if (p_tok)
                    strncpy(prefix, p_tok, sizeof(prefix) - 1);
                if (p_tok)
                    rc = cmd_complete(&conn, prefix, limit, cwd_path, dirs_only);
            } else {
                int n = sscanf(line, "%*s %4095s %u %4095s %7s", prefix, &limit, cwd_path,
                               dirs_only_str);
                if (n >= 4 && (strcmp(dirs_only_str, "1") == 0 || strcmp(dirs_only_str, "true") == 0))
                    dirs_only = 1;
                if (n >= 1)
                    rc = cmd_complete(&conn, prefix, limit, cwd_path, dirs_only);
            }
        } else if (strcmp(cmd, "suggest") == 0) {
            char prefix[4096] = {0};
            char cwd_path[4096] = {0};
            if (sscanf(line, "%*s %4095s %4095s", prefix, cwd_path) >= 1) {
                rc = cmd_suggest(&conn, prefix, cwd_path);
            }
        } else if (strcmp(cmd, "query") == 0) {
            char cwd[4096] = {0};
            char input[4096] = {0};
            /* query <cwd> <input> - input may contain spaces */
            if (sscanf(line, "%*s %4095s", cwd) == 1) {
                /* Find the input after "query <cwd> " */
                const char* p = line + 5; /* skip "query" */
                while (*p == ' ')
                    p++;
                while (*p && *p != ' ')
                    p++; /* skip cwd */
                while (*p == ' ')
                    p++;
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
        } else if (strcmp(cmd, "fuzzy") == 0) {
            char prefix[4096] = {0};
            uint32_t limit = 50;
            int n = sscanf(line, "%*s %4095s %u", prefix, &limit);
            if (n >= 1) {
                rc = cmd_fuzzy(&conn, prefix, limit);
            }
        } else if (strcmp(cmd, "select") == 0) {
            char path[4096] = {0};
            if (sscanf(line, "%*s %4095s", path) >= 1)
                rc = cmd_select(&conn, path);
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else {
            fprintf(stderr, "helper: unknown command: %s\n", cmd);
            rc = 0; /* not a fatal error */
        }

        if (rc < 0) {
            fprintf(stderr, "helper: command '%s' failed\n", cmd);
        }
        if (fifo_mode)
            fputs(".\n", stdout);
        fflush(stdout);
    }

    helper_disconnect(&conn);
    return 0;
}
