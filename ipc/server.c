#include "server.h"
#include "../src/io/fileloader.h"
#include "../src/metrics.h"
#include "../src/threadpool.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct ipc_server {
    daemon_state* daemon;
    int listen_fd;
    pthread_t worker;
    int running;
    char sock_path[256];
    threadpool* pool;
    struct timespec start_time;
};

typedef struct {
    ipc_server* srv;
    int fd;
} client_ctx;

static int read_exact(int fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char*) buf + total, len - total);
        if (n <= 0)
            return -1;
        total += n;
    }
    return 0;
}

static int write_exact(int fd, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char*) buf + total, len - total);
        if (n <= 0)
            return -1;
        total += n;
    }
    return 0;
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
    daemon_run_scan(srv->daemon, req->path);
    send_ok(fd, req_id);
}

static void handle_save(ipc_server* srv, int fd, uint32_t req_id, const ipc_save_req* req) {
    daemon_save_state(srv->daemon, req->save_path);
    send_ok(fd, req_id);
}

static void handle_query(ipc_server* srv, int fd, uint32_t req_id, const ipc_query_req* req) {
    path_validation v = daemon_process_query(srv->daemon, req->cwd, req->input);

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
    uint64_t now = (uint64_t) time(NULL);
    scored_result sr =
        daemon_get_scored_completions(srv->daemon, req->prefix, req->limit, now, req->cwd);
    const scored_completions* sc = sr.data;

    ipc_header hdr;
    ipc_completions_resp resp;
    resp.count = 0;
    memset(resp.paths, 0, sizeof(resp.paths));
    memset(resp.scores, 0, sizeof(resp.scores));
    memset(resp.freqs, 0, sizeof(resp.freqs));
    memset(resp.is_dirs, 0, sizeof(resp.is_dirs));

    if (sc) {
        uint32_t n = sc->count < 50 ? sc->count : 50;
        resp.count = n;
        for (uint32_t i = 0; i < n; i++) {
            const char* p = sc->entries[i].path;
            size_t plen = strlen(p);
            char clean[4096];
            if (plen > 0 && p[plen - 1] == '/') {
                memcpy(clean, p, plen - 1);
                clean[plen - 1] = '\0';
            } else {
                strncpy(clean, p, sizeof(clean) - 1);
                clean[sizeof(clean) - 1] = '\0';
            }
            strncpy(resp.paths[i], clean, sizeof(resp.paths[i]) - 1);
            resp.scores[i] = sc->entries[i].score;
            resp.freqs[i] = sc->entries[i].freq;
            resp.is_dirs[i] = sc->entries[i].is_dir ? 1 : 0;
        }
        daemon_release_scored(srv->daemon, sr);
    }

    ipc_write_header(&hdr, IPC_MSG_COMPLETIONS, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_suggest(ipc_server* srv, int fd, uint32_t req_id, const ipc_suggest_req* req) {
    uint64_t now = (uint64_t) time(NULL);
    scored_result sr = daemon_get_scored_completions(srv->daemon, req->prefix, 1, now, req->cwd);
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
        daemon_release_scored(srv->daemon, sr);
    }

    ipc_write_header(&hdr, IPC_MSG_SUGGESTION, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_ping(ipc_server* srv, int fd, uint32_t req_id) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t uptime_ms = (uint64_t) (now.tv_sec - srv->start_time.tv_sec) * 1000ULL +
                         (uint64_t) (now.tv_nsec - srv->start_time.tv_nsec) / 1000000ULL;

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
    completions* fc = daemon_get_fuzzy_completions(srv->daemon, req->prefix, req->limit);

    ipc_header hdr;
    ipc_completions_resp resp;
    resp.count = 0;
    memset(resp.paths, 0, sizeof(resp.paths));
    memset(resp.scores, 0, sizeof(resp.scores));
    memset(resp.freqs, 0, sizeof(resp.freqs));
    memset(resp.is_dirs, 0, sizeof(resp.is_dirs));

    if (fc) {
        uint32_t n = fc->count < 50 ? fc->count : 50;
        resp.count = n;
        for (uint32_t i = 0; i < n; i++) {
            const char* p = fc->paths[i];
            size_t plen = strlen(p);
            char clean[4096];
            if (plen > 0 && p[plen - 1] == '/') {
                memcpy(clean, p, plen - 1);
                clean[plen - 1] = '\0';
            } else {
                strncpy(clean, p, sizeof(clean) - 1);
                clean[sizeof(clean) - 1] = '\0';
            }
            strncpy(resp.paths[i], clean, sizeof(resp.paths[i]) - 1);
            resp.is_dirs[i] = 0;
        }
        completions_free(fc);
    }

    ipc_write_header(&hdr, IPC_MSG_FUZZY_COMPLETIONS, sizeof(resp), req_id);
    write_exact(fd, &hdr, sizeof(hdr));
    write_exact(fd, &resp, sizeof(resp));
}

static void handle_client(ipc_server* srv, int fd) {
    ipc_header hdr;

    while (srv->running) {
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
            if (hdr.payload_len != sizeof(req) || read_exact(fd, &req, sizeof(req)) < 0) {
                send_error(fd, hdr.request_id, -3, "invalid scan payload");
                break;
            }
            handle_scan(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_SAVE: {
            ipc_save_req req;
            if (hdr.payload_len != sizeof(req) || read_exact(fd, &req, sizeof(req)) < 0) {
                send_error(fd, hdr.request_id, -3, "invalid save payload");
                break;
            }
            handle_save(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_QUERY: {
            ipc_query_req req;
            if (hdr.payload_len != sizeof(req) || read_exact(fd, &req, sizeof(req)) < 0) {
                send_error(fd, hdr.request_id, -3, "invalid query payload");
                break;
            }
            handle_query(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_COMPLETE: {
            ipc_complete_req req;
            if (hdr.payload_len != sizeof(req) || read_exact(fd, &req, sizeof(req)) < 0) {
                send_error(fd, hdr.request_id, -3, "invalid complete payload");
                break;
            }
            handle_complete(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_SUGGEST: {
            ipc_suggest_req req;
            if (hdr.payload_len != sizeof(req) || read_exact(fd, &req, sizeof(req)) < 0) {
                send_error(fd, hdr.request_id, -3, "invalid suggest payload");
                break;
            }
            handle_suggest(srv, fd, hdr.request_id, &req);
            break;
        }
        case IPC_MSG_SHUTDOWN: {
            send_ok(fd, hdr.request_id);
            srv->running = 0;
            close(fd);
            return;
        }
        case IPC_MSG_PING: {
            handle_ping(srv, fd, hdr.request_id);
            break;
        }
        case IPC_MSG_METRICS: {
            handle_metrics(srv, fd, hdr.request_id);
            break;
        }
        case IPC_MSG_SCAN_STATUS: {
            handle_scan_status(srv, fd, hdr.request_id);
            break;
        }
        case IPC_MSG_FUZZY_COMPLETE: {
            ipc_complete_req req;
            if (hdr.payload_len != sizeof(req) || read_exact(fd, &req, sizeof(req)) < 0) {
                send_error(fd, hdr.request_id, -3, "invalid fuzzy complete payload");
                break;
            }
            handle_fuzzy_complete(srv, fd, hdr.request_id, &req);
            break;
        }
        default:
            send_error(fd, hdr.request_id, -4, "unknown message type");
            break;
        }
    }

    close(fd);
}

static void handle_client_threaded(void* arg) {
    client_ctx* ctx = (client_ctx*) arg;
    handle_client(ctx->srv, ctx->fd);
    free(ctx);
}

static void* server_loop(void* arg) {
    ipc_server* srv = (ipc_server*) arg;

    while (srv->running) {
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        client_ctx* ctx = malloc(sizeof(client_ctx));
        ctx->srv = srv;
        ctx->fd = fd;
        threadpool_submit(srv->pool, handle_client_threaded, ctx);
    }

    return NULL;
}

ipc_server* ipc_server_start(daemon_state* daemon, const char* sock_path) {
    ipc_server* srv = calloc(1, sizeof(ipc_server));
    if (!srv)
        return NULL;

    srv->daemon = daemon;
    srv->running = 1;
    strncpy(srv->sock_path, sock_path, sizeof(srv->sock_path) - 1);

    srv->pool = threadpool_init(8);
    if (!srv->pool) {
        free(srv);
        return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &srv->start_time);

    unlink(sock_path);

    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        threadpool_shutdown(srv->pool);
        free(srv);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(srv->listen_fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        threadpool_shutdown(srv->pool);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

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

    srv->running = 0;

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
