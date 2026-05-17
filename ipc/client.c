#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

struct ipc_client {
    int fd;
    uint32_t next_req_id;
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

static int send_request(ipc_client* client, uint32_t type, const void* payload, size_t len) {
    ipc_header hdr;
    ipc_write_header(&hdr, type, len, client->next_req_id++);
    if (write_exact(client->fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (payload && len > 0) {
        if (write_exact(client->fd, payload, len) < 0) return -1;
    }
    return 0;
}

static int recv_response(ipc_client* client, ipc_header* hdr, void* payload, size_t max_len) {
    if (read_exact(client->fd, hdr, sizeof(*hdr)) < 0) return -1;
    if (!ipc_validate_header(hdr)) return -1;
    if (hdr->payload_len > max_len) return -1;
    if (hdr->payload_len > 0) {
        if (read_exact(client->fd, payload, hdr->payload_len) < 0) return -1;
    }
    return 0;
}

ipc_client* ipc_client_connect(const char* sock_path) {
    ipc_client* client = calloc(1, sizeof(ipc_client));
    if (!client) return NULL;

    client->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client->fd < 0) {
        free(client);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(client->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(client->fd);
        free(client);
        return NULL;
    }

    client->next_req_id = 1;
    return client;
}

void ipc_client_disconnect(ipc_client* client) {
    if (!client) return;
    if (client->fd >= 0) close(client->fd);
    free(client);
}

int ipc_client_scan(ipc_client* client, const char* path) {
    ipc_scan_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.path, path, sizeof(req.path) - 1);

    if (send_request(client, IPC_MSG_SCAN, &req, sizeof(req)) < 0) return -1;

    ipc_header hdr;
    ipc_ok_resp resp;
    if (recv_response(client, &hdr, &resp, sizeof(resp)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_OK) return -1;
    return 0;
}

int ipc_client_save(ipc_client* client, const char* path) {
    ipc_save_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.save_path, path, sizeof(req.save_path) - 1);

    if (send_request(client, IPC_MSG_SAVE, &req, sizeof(req)) < 0) return -1;

    ipc_header hdr;
    ipc_ok_resp resp;
    if (recv_response(client, &hdr, &resp, sizeof(resp)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_OK) return -1;
    return 0;
}

int ipc_client_query(ipc_client* client, const char* cwd, const char* input,
                     ipc_validation_resp* out) {
    ipc_query_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.cwd, cwd, sizeof(req.cwd) - 1);
    strncpy(req.input, input, sizeof(req.input) - 1);

    if (send_request(client, IPC_MSG_QUERY, &req, sizeof(req)) < 0) return -1;

    ipc_header hdr;
    if (recv_response(client, &hdr, out, sizeof(*out)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_VALIDATION) return -1;
    return 0;
}

int ipc_client_complete(ipc_client* client, const char* prefix, uint32_t limit,
                        ipc_completions_resp* out) {
    ipc_complete_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.prefix, prefix, sizeof(req.prefix) - 1);
    req.limit = limit;

    if (send_request(client, IPC_MSG_COMPLETE, &req, sizeof(req)) < 0) return -1;

    ipc_header hdr;
    if (recv_response(client, &hdr, out, sizeof(*out)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_COMPLETIONS) return -1;
    return 0;
}

int ipc_client_suggest(ipc_client* client, const char* prefix,
                       ipc_suggestion_resp* out) {
    ipc_suggest_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.prefix, prefix, sizeof(req.prefix) - 1);

    if (send_request(client, IPC_MSG_SUGGEST, &req, sizeof(req)) < 0) return -1;

    ipc_header hdr;
    if (recv_response(client, &hdr, out, sizeof(*out)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_SUGGESTION) return -1;
    return 0;
}

int ipc_client_shutdown(ipc_client* client) {
    if (send_request(client, IPC_MSG_SHUTDOWN, NULL, 0) < 0) return -1;

    ipc_header hdr;
    ipc_ok_resp resp;
    if (recv_response(client, &hdr, &resp, sizeof(resp)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_OK) return -1;
    return 0;
}

int ipc_client_ping(ipc_client* client, uint64_t* out_uptime_ms) {
    ipc_header hdr;
    ipc_pong_resp resp;
    if (send_request(client, IPC_MSG_PING, NULL, 0) < 0) return -1;
    if (recv_response(client, &hdr, &resp, sizeof(resp)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_PONG) return -1;
    if (out_uptime_ms) *out_uptime_ms = resp.uptime_ms;
    return 0;
}

int ipc_client_metrics(ipc_client* client, ipc_metrics_resp* out) {
    ipc_header hdr;
    if (send_request(client, IPC_MSG_METRICS, NULL, 0) < 0) return -1;
    if (recv_response(client, &hdr, out, sizeof(*out)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_METRICS_RESP) return -1;
    return 0;
}

int ipc_client_scan_status(ipc_client* client, ipc_scan_status_resp* out) {
    ipc_header hdr;
    if (send_request(client, IPC_MSG_SCAN_STATUS, NULL, 0) < 0) return -1;
    if (recv_response(client, &hdr, out, sizeof(*out)) < 0) return -1;
    if (hdr.msg_type != IPC_MSG_SCAN_STATUS_RESP) return -1;
    return 0;
}
