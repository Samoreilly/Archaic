#include "tcp-server.h"
#include "io/fileloader.h"
#include "log.h"
#include "metrics.h"
#include "threadpool.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct tcp_server {
    daemon_state* daemon;
    int listen_fd;
    pthread_t worker;
    int running;
    threadpool* pool;
    struct timespec start_time;
    char auth_token[256];
    int port;
};

typedef struct {
    tcp_server* srv;
    int fd;
} tcp_client_ctx;

static int tcp_read_exact(int fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char*) buf + total, len - total);
        if (n <= 0)
            return -1;
        total += (size_t) n;
    }
    return 0;
}

static int tcp_write_exact(int fd, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char*) buf + total, len - total);
        if (n <= 0)
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

static void tcp_send_error(int fd, uint32_t req_id, int32_t code, const char* msg) {
    ipc_header hdr;
    ipc_error_resp resp;
    resp.error_code = code;
    memset(resp.message, 0, sizeof(resp.message));
    strncpy(resp.message, msg, sizeof(resp.message) - 1);

    ipc_write_header(&hdr, IPC_MSG_ERROR, sizeof(resp), req_id);
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_send_ok(int fd, uint32_t req_id) {
    ipc_header hdr;
    ipc_ok_resp resp;
    resp.status = 0;

    ipc_write_header(&hdr, IPC_MSG_OK, sizeof(resp), req_id);
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_handle_scan(tcp_server* srv, int fd, uint32_t req_id, const ipc_scan_req* req) {
    if (validate_string_field(req->path, sizeof(req->path)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid path: not null-terminated");
        return;
    }
    if (strstr(req->path, "..") != NULL) {
        tcp_send_error(fd, req_id, -8, "path traversal not allowed");
        return;
    }
    daemon_run_scan(srv->daemon, req->path);
    tcp_send_ok(fd, req_id);
}

static void tcp_handle_save(tcp_server* srv, int fd, uint32_t req_id, const ipc_save_req* req) {
    if (validate_string_field(req->save_path, sizeof(req->save_path)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid save path: not null-terminated");
        return;
    }
    daemon_save_state(srv->daemon, req->save_path);
    tcp_send_ok(fd, req_id);
}

static void tcp_handle_query(tcp_server* srv, int fd, uint32_t req_id, const ipc_query_req* req) {
    if (validate_string_field(req->cwd, sizeof(req->cwd)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid cwd: not null-terminated");
        return;
    }
    if (validate_string_field(req->input, sizeof(req->input)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid input: not null-terminated");
        return;
    }
    if (strstr(req->cwd, "..") != NULL || strstr(req->input, "..") != NULL) {
        tcp_send_error(fd, req_id, -8, "path traversal not allowed");
        return;
    }

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
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));

    free_path_validation(&v);
}

static void tcp_handle_complete(tcp_server* srv, int fd, uint32_t req_id,
                                const ipc_complete_req* req) {
    if (validate_string_field(req->prefix, sizeof(req->prefix)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid prefix: not null-terminated");
        return;
    }
    if (validate_string_field(req->cwd, sizeof(req->cwd)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid cwd: not null-terminated");
        return;
    }
    if (strstr(req->prefix, "..") != NULL || strstr(req->cwd, "..") != NULL) {
        tcp_send_error(fd, req_id, -8, "path traversal not allowed");
        return;
    }

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
        uint32_t out_idx = 0;
        for (uint32_t i = 0; i < n && out_idx < 50; i++) {
            if (req->dirs_only && !sc->entries[i].is_dir)
                continue;

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
            strncpy(resp.paths[out_idx], clean, sizeof(resp.paths[out_idx]) - 1);
            resp.scores[out_idx] = sc->entries[i].score;
            resp.freqs[out_idx] = sc->entries[i].freq;
            resp.is_dirs[out_idx] = sc->entries[i].is_dir ? 1 : 0;
            out_idx++;
        }
        resp.count = out_idx;
        daemon_release_scored(srv->daemon, sr);
    }

    ipc_write_header(&hdr, IPC_MSG_COMPLETIONS, sizeof(resp), req_id);
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_handle_suggest(tcp_server* srv, int fd, uint32_t req_id,
                               const ipc_suggest_req* req) {
    if (validate_string_field(req->prefix, sizeof(req->prefix)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid prefix: not null-terminated");
        return;
    }
    if (validate_string_field(req->cwd, sizeof(req->cwd)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid cwd: not null-terminated");
        return;
    }
    if (strstr(req->prefix, "..") != NULL || strstr(req->cwd, "..") != NULL) {
        tcp_send_error(fd, req_id, -8, "path traversal not allowed");
        return;
    }

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
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_handle_recent(tcp_server* srv, int fd, uint32_t req_id, const ipc_recent_req* req) {
    ipc_header hdr;
    ipc_recent_resp resp;
    resp.count = 0;
    memset(resp.paths, 0, sizeof(resp.paths));
    memset(resp.is_dirs, 0, sizeof(resp.is_dirs));

    uint32_t limit = req->limit > 0 && req->limit <= 50 ? req->limit : 50;
    char* paths[50];
    for (int i = 0; i < 50; i++) {
        paths[i] = malloc(4096);
    }
    bool is_dirs[50];

    int count = daemon_get_recent_files(srv->daemon, paths, is_dirs, (int) limit);

    resp.count = count < 50 ? (uint32_t) count : 50;
    for (uint32_t i = 0; i < resp.count; i++) {
        strncpy(resp.paths[i], paths[i], sizeof(resp.paths[i]) - 1);
        resp.paths[i][sizeof(resp.paths[i]) - 1] = '\0';
        resp.is_dirs[i] = is_dirs[i] ? 1 : 0;
    }

    for (int i = 0; i < 50; i++) {
        free(paths[i]);
    }

    ipc_write_header(&hdr, IPC_MSG_RECENT_RESP, sizeof(resp), req_id);
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_handle_ping(tcp_server* srv, int fd, uint32_t req_id) {
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
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_handle_metrics(tcp_server* srv, int fd, uint32_t req_id) {
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
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_handle_scan_status(tcp_server* srv, int fd, uint32_t req_id) {
    scan_status s = daemon_scan_status(srv->daemon);

    ipc_header hdr;
    ipc_scan_status_resp resp;
    resp.scanning = s.scanning ? 1 : 0;
    resp.buckets_so_far = s.buckets_so_far;

    ipc_write_header(&hdr, IPC_MSG_SCAN_STATUS_RESP, sizeof(resp), req_id);
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_handle_fuzzy_complete(tcp_server* srv, int fd, uint32_t req_id,
                                      const ipc_complete_req* req) {
    if (validate_string_field(req->prefix, sizeof(req->prefix)) != 0) {
        tcp_send_error(fd, req_id, -6, "invalid prefix: not null-terminated");
        return;
    }
    if (strstr(req->prefix, "..") != NULL) {
        tcp_send_error(fd, req_id, -8, "path traversal not allowed");
        return;
    }

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
            resp.is_dirs[i] = (fc->is_dirs && fc->is_dirs[i]) ? 1 : 0;
        }
        completions_free(fc);
    }

    ipc_write_header(&hdr, IPC_MSG_FUZZY_COMPLETIONS, sizeof(resp), req_id);
    tcp_write_exact(fd, &hdr, sizeof(hdr));
    tcp_write_exact(fd, &resp, sizeof(resp));
}

static void tcp_dispatch_message(tcp_server* srv, int fd, ipc_header* hdr) {
    switch (hdr->msg_type) {
    case IPC_MSG_SCAN: {
        ipc_scan_req req;
        if (hdr->payload_len != sizeof(req) || tcp_read_exact(fd, &req, sizeof(req)) < 0) {
            tcp_send_error(fd, hdr->request_id, -3, "invalid scan payload");
            break;
        }
        tcp_handle_scan(srv, fd, hdr->request_id, &req);
        break;
    }
    case IPC_MSG_SAVE: {
        ipc_save_req req;
        if (hdr->payload_len != sizeof(req) || tcp_read_exact(fd, &req, sizeof(req)) < 0) {
            tcp_send_error(fd, hdr->request_id, -3, "invalid save payload");
            break;
        }
        tcp_handle_save(srv, fd, hdr->request_id, &req);
        break;
    }
    case IPC_MSG_QUERY: {
        ipc_query_req req;
        if (hdr->payload_len != sizeof(req) || tcp_read_exact(fd, &req, sizeof(req)) < 0) {
            tcp_send_error(fd, hdr->request_id, -3, "invalid query payload");
            break;
        }
        tcp_handle_query(srv, fd, hdr->request_id, &req);
        break;
    }
    case IPC_MSG_COMPLETE: {
        ipc_complete_req req;
        if (hdr->payload_len != sizeof(req) || tcp_read_exact(fd, &req, sizeof(req)) < 0) {
            tcp_send_error(fd, hdr->request_id, -3, "invalid complete payload");
            break;
        }
        tcp_handle_complete(srv, fd, hdr->request_id, &req);
        break;
    }
    case IPC_MSG_SUGGEST: {
        ipc_suggest_req req;
        if (hdr->payload_len != sizeof(req) || tcp_read_exact(fd, &req, sizeof(req)) < 0) {
            tcp_send_error(fd, hdr->request_id, -3, "invalid suggest payload");
            break;
        }
        tcp_handle_suggest(srv, fd, hdr->request_id, &req);
        break;
    }
    case IPC_MSG_SHUTDOWN: {
        tcp_send_ok(fd, hdr->request_id);
        srv->running = 0;
        close(fd);
        return;
    }
    case IPC_MSG_PING: {
        tcp_handle_ping(srv, fd, hdr->request_id);
        break;
    }
    case IPC_MSG_METRICS: {
        tcp_handle_metrics(srv, fd, hdr->request_id);
        break;
    }
    case IPC_MSG_SCAN_STATUS: {
        tcp_handle_scan_status(srv, fd, hdr->request_id);
        break;
    }
    case IPC_MSG_FUZZY_COMPLETE: {
        ipc_complete_req req;
        if (hdr->payload_len != sizeof(req) || tcp_read_exact(fd, &req, sizeof(req)) < 0) {
            tcp_send_error(fd, hdr->request_id, -3, "invalid fuzzy complete payload");
            break;
        }
        tcp_handle_fuzzy_complete(srv, fd, hdr->request_id, &req);
        break;
    }
    case IPC_MSG_RECENT: {
        ipc_recent_req req;
        if (hdr->payload_len != sizeof(req) || tcp_read_exact(fd, &req, sizeof(req)) < 0) {
            tcp_send_error(fd, hdr->request_id, -3, "invalid recent payload");
            break;
        }
        tcp_handle_recent(srv, fd, hdr->request_id, &req);
        break;
    }
    default:
        tcp_send_error(fd, hdr->request_id, -4, "unknown message type");
        break;
    }
}

static int tcp_check_auth(tcp_server* srv, int fd) {
    if (srv->auth_token[0] == '\0') {
        return 0;
    }

    ipc_header hdr;
    if (tcp_read_exact(fd, &hdr, sizeof(hdr)) < 0) {
        return -1;
    }

    if (!ipc_validate_header(&hdr)) {
        return -1;
    }

    if (hdr.msg_type != IPC_MSG_AUTH) {
        tcp_send_error(fd, hdr.request_id, -10, "authentication required");
        return -1;
    }

    if (hdr.payload_len != sizeof(ipc_auth_req)) {
        tcp_send_error(fd, hdr.request_id, -10, "invalid auth payload");
        return -1;
    }

    ipc_auth_req auth;
    if (tcp_read_exact(fd, &auth, sizeof(auth)) < 0) {
        return -1;
    }

    if (validate_string_field(auth.token, sizeof(auth.token)) != 0) {
        tcp_send_error(fd, hdr.request_id, -10, "invalid auth token");
        return -1;
    }

    ipc_header resp_hdr;
    if (strcmp(auth.token, srv->auth_token) == 0) {
        ipc_write_header(&resp_hdr, IPC_MSG_AUTH_OK, 0, hdr.request_id);
        tcp_write_exact(fd, &resp_hdr, sizeof(resp_hdr));
        return 0;
    } else {
        ipc_write_header(&resp_hdr, IPC_MSG_AUTH_FAIL, 0, hdr.request_id);
        tcp_write_exact(fd, &resp_hdr, sizeof(resp_hdr));
        return -1;
    }
}

static void tcp_handle_client(tcp_server* srv, int fd) {
    if (tcp_check_auth(srv, fd) < 0) {
        close(fd);
        return;
    }

    ipc_header hdr;

    while (srv->running) {
        if (tcp_read_exact(fd, &hdr, sizeof(hdr)) < 0) {
            break;
        }

        if (!ipc_validate_header(&hdr)) {
            if ((hdr.magic >> 16) == IPC_MAGIC_PREFIX) {
                char msg[64];
                snprintf(msg, sizeof(msg), "unsupported protocol version %u (server supports 1-%u)",
                         ipc_header_version(&hdr), IPC_PROTOCOL_VERSION);
                tcp_send_error(fd, 0, -5, msg);
            } else {
                tcp_send_error(fd, 0, -1, "invalid message header");
            }
            break;
        }

        if (hdr.payload_len > IPC_MAX_PAYLOAD) {
            tcp_send_error(fd, hdr.request_id, -2, "payload too large");
            break;
        }

        tcp_dispatch_message(srv, fd, &hdr);
    }

    close(fd);
}

static void tcp_handle_client_threaded(void* arg) {
    tcp_client_ctx* ctx = (tcp_client_ctx*) arg;
    tcp_handle_client(ctx->srv, ctx->fd);
    free(ctx);
}

static void* tcp_server_loop(void* arg) {
    tcp_server* srv = (tcp_server*) arg;

    while (srv->running) {
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (!srv->running)
                break;
            LOG_ERR("tcp", "accept failed: %s", strerror(errno));
            continue;
        }

        tcp_client_ctx* ctx = malloc(sizeof(tcp_client_ctx));
        if (!ctx) {
            close(fd);
            continue;
        }
        ctx->srv = srv;
        ctx->fd = fd;

        if (threadpool_submit(srv->pool, tcp_handle_client_threaded, ctx) < 0) {
            close(fd);
            free(ctx);
        }
    }

    return NULL;
}

tcp_server* tcp_server_start(daemon_state* daemon, const char* bind_addr, int port,
                             const char* auth_token) {
    tcp_server* srv = calloc(1, sizeof(tcp_server));
    if (!srv)
        return NULL;

    srv->daemon = daemon;
    srv->running = 1;
    srv->port = port;
    if (auth_token && auth_token[0] != '\0') {
        strncpy(srv->auth_token, auth_token, sizeof(srv->auth_token) - 1);
    }

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

    clock_gettime(CLOCK_MONOTONIC, &srv->start_time);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        LOG_ERR("tcp", "socket failed: %s", strerror(errno));
        threadpool_shutdown(srv->pool);
        free(srv);
        return NULL;
    }

    int opt = 1;
    if (setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARN("tcp", "setsockopt SO_REUSEADDR failed: %s", strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);

    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        LOG_ERR("tcp", "invalid bind address: %s", bind_addr);
        threadpool_shutdown(srv->pool);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    if (bind(srv->listen_fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        LOG_ERR("tcp", "bind failed on %s:%d: %s", bind_addr, port, strerror(errno));
        threadpool_shutdown(srv->pool);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    if (listen(srv->listen_fd, 8) < 0) {
        LOG_ERR("tcp", "listen failed: %s", strerror(errno));
        threadpool_shutdown(srv->pool);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    if (pthread_create(&srv->worker, NULL, tcp_server_loop, srv) != 0) {
        LOG_ERR("tcp", "pthread_create failed");
        threadpool_shutdown(srv->pool);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    LOG_INFO("tcp", "listening on %s:%d (auth: %s)", bind_addr, port,
             auth_token && auth_token[0] ? "enabled" : "disabled");
    return srv;
}

void tcp_server_stop(tcp_server* srv) {
    if (!srv)
        return;

    srv->running = 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t) srv->port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        int wake_ret = connect(fd, (struct sockaddr*) &addr, sizeof(addr));
        (void) wake_ret;
        close(fd);
    }

    pthread_join(srv->worker, NULL);

    if (srv->pool) {
        threadpool_shutdown(srv->pool);
        srv->pool = NULL;
    }

    close(srv->listen_fd);
    free(srv);
}
