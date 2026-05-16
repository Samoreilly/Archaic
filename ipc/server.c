#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <errno.h>

struct ipc_server {
    daemon_state* daemon;
    int listen_fd;
    pthread_t worker;
    int running;
    char sock_path[256];
};

static int read_exact(int fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char*)buf + total, len - total);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int write_exact(int fd, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char*)buf + total, len - total);
        if (n <= 0) return -1;
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
    completions* c = daemon_get_completions(srv->daemon, req->prefix, req->limit);

    ipc_header hdr;
    ipc_completions_resp resp;
    resp.count = 0;
    memset(resp.paths, 0, sizeof(resp.paths));

    if (c) {
        uint32_t n = c->count < 50 ? c->count : 50;
        resp.count = n;
        for (uint32_t i = 0; i < n; i++) {
            strncpy(resp.paths[i], c->paths[i], sizeof(resp.paths[i]) - 1);
        }
        completions_free(c);
    }

    ipc_write_header(&hdr, IPC_MSG_COMPLETIONS, sizeof(resp), req_id);
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
            send_error(fd, 0, -1, "invalid message header");
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
        case IPC_MSG_SHUTDOWN: {
            send_ok(fd, hdr.request_id);
            srv->running = 0;
            close(fd);
            return;
        }
        default:
            send_error(fd, hdr.request_id, -4, "unknown message type");
            break;
        }
    }

    close(fd);
}

static void* server_loop(void* arg) {
    ipc_server* srv = (ipc_server*)arg;

    while (srv->running) {
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(srv, fd);
    }

    return NULL;
}

ipc_server* ipc_server_start(daemon_state* daemon, const char* sock_path) {
    ipc_server* srv = calloc(1, sizeof(ipc_server));
    if (!srv) return NULL;

    srv->daemon = daemon;
    srv->running = 1;
    strncpy(srv->sock_path, sock_path, sizeof(srv->sock_path) - 1);

    unlink(sock_path);

    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        free(srv);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(srv->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    if (listen(srv->listen_fd, 8) < 0) {
        close(srv->listen_fd);
        unlink(sock_path);
        free(srv);
        return NULL;
    }

    if (pthread_create(&srv->worker, NULL, server_loop, srv) != 0) {
        close(srv->listen_fd);
        unlink(sock_path);
        free(srv);
        return NULL;
    }

    return srv;
}

void ipc_server_stop(ipc_server* srv) {
    if (!srv) return;

    srv->running = 0;

    /* Wake up accept with a dummy connection */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, srv->sock_path, sizeof(addr.sun_path) - 1);
        connect(fd, (struct sockaddr*)&addr, sizeof(addr));
        close(fd);
    }

    pthread_join(srv->worker, NULL);
    close(srv->listen_fd);
    unlink(srv->sock_path);
    free(srv);
}
