#pragma once

#include <stddef.h>
#include <stdint.h>

#define IPC_SOCK_PATH "/tmp/archaic-daemon.sock"
#define IPC_MAX_PAYLOAD 262144

/*
    Protocol versioning: magic = 0x4152VVVV where VVVV is the version.
    Upper 16 bits (0x4152) identify the protocol family.
    Lower 16 bits carry the protocol version for backward-compatible negotiation.
*/
#define IPC_MAGIC_PREFIX 0x4152
#define IPC_PROTOCOL_VERSION 1
#define IPC_MAGIC ((IPC_MAGIC_PREFIX << 16) | IPC_PROTOCOL_VERSION)

/*
    Message types
*/
typedef enum {
    IPC_MSG_SCAN = 1,
    IPC_MSG_QUERY = 2,
    IPC_MSG_COMPLETE = 3,
    IPC_MSG_SHUTDOWN = 4,
    IPC_MSG_SUGGEST = 5,
    IPC_MSG_SAVE = 6,
    IPC_MSG_PING = 7,
    IPC_MSG_METRICS = 8,
    IPC_MSG_SCAN_STATUS = 9,
    IPC_MSG_FUZZY_COMPLETE = 10,

    IPC_MSG_OK = 100,
    IPC_MSG_ERROR = 101,
    IPC_MSG_COMPLETIONS = 102,
    IPC_MSG_VALIDATION = 103,
    IPC_MSG_SUGGESTION = 104,
    IPC_MSG_PONG = 105,
    IPC_MSG_METRICS_RESP = 106,
    IPC_MSG_SCAN_STATUS_RESP = 107,
    IPC_MSG_FUZZY_COMPLETIONS = 108,
} ipc_msg_type;

/*
    Binary message header
*/
typedef struct {
    uint32_t magic;
    uint32_t msg_type;
    uint32_t payload_len;
    uint32_t request_id;
} __attribute__((packed)) ipc_header;

/*
    Payload structures
*/
typedef struct {
    char path[4096];
} __attribute__((packed)) ipc_scan_req;

typedef struct {
    char save_path[4096];
} __attribute__((packed)) ipc_save_req;

typedef struct {
    char cwd[4096];
    char input[4096];
} __attribute__((packed)) ipc_query_req;

typedef struct {
    char prefix[4096];
    char cwd[4096];
    uint32_t limit;
    uint32_t dirs_only;
} __attribute__((packed)) ipc_complete_req;

typedef struct {
    char prefix[4096];
    char cwd[4096];
} __attribute__((packed)) ipc_suggest_req;

typedef struct {
    int32_t error_code;
    char message[256];
} __attribute__((packed)) ipc_error_resp;

typedef struct {
    int32_t exists;
    int32_t is_dir;
    int32_t is_file;
    char full_path[4096];
} __attribute__((packed)) ipc_validation_resp;

typedef struct {
    uint32_t count;
    char paths[50][4096];
    double scores[50];
    uint64_t freqs[50];
    uint32_t is_dirs[50];
} __attribute__((packed)) ipc_completions_resp;

typedef struct {
    char path[4096];
    double score;
    uint64_t freq;
    uint32_t is_dir;
} __attribute__((packed)) ipc_suggestion_resp;

typedef struct {
    int32_t status;
} __attribute__((packed)) ipc_ok_resp;

typedef struct {
    uint64_t uptime_ms;
} __attribute__((packed)) ipc_pong_resp;

typedef struct {
    uint64_t queries_total;
    uint64_t completions_total;
    uint64_t scans_total;
    uint64_t errors_total;
    uint64_t cache_hits;
    uint64_t cache_misses;
    double query_latency_avg_ms;
} __attribute__((packed)) ipc_metrics_resp;

#define MAX_PROJECTS 100

typedef struct {
    char path[512];
    char language[32];
} ipc_project_info;

typedef struct {
    int32_t scanning;
    uint64_t buckets_so_far;
    uint32_t project_count;
    ipc_project_info projects[MAX_PROJECTS];
} __attribute__((packed)) ipc_scan_status_resp;

/*
    Serialization helpers
*/
static inline size_t ipc_header_size(void) {
    return sizeof(ipc_header);
}

static inline void ipc_write_header(ipc_header* hdr, uint32_t type, uint32_t len, uint32_t req_id) {
    hdr->magic = IPC_MAGIC;
    hdr->msg_type = type;
    hdr->payload_len = len;
    hdr->request_id = req_id;
}

static inline uint16_t ipc_header_version(const ipc_header* hdr) {
    return (uint16_t) (hdr->magic & 0xFFFF);
}

static inline int ipc_validate_header(const ipc_header* hdr) {
    if ((hdr->magic >> 16) != IPC_MAGIC_PREFIX)
        return 0;
    uint16_t ver = ipc_header_version(hdr);
    if (ver < 1 || ver > IPC_PROTOCOL_VERSION)
        return 0;
    if (hdr->payload_len >= IPC_MAX_PAYLOAD)
        return 0;
    return 1;
}
