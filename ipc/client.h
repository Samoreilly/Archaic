#pragma once

#include "protocol.h"

typedef struct ipc_client ipc_client;

/*
    Connect to the daemon IPC server.
    Returns NULL on failure.
*/
ipc_client* ipc_client_connect(const char* sock_path);

/*
    Disconnect and free the client.
*/
void ipc_client_disconnect(ipc_client* client);

/*
    Send a scan request. Returns 0 on success.
*/
int ipc_client_scan(ipc_client* client, const char* path);
int ipc_client_save(ipc_client* client, const char* path);

/*
    Send a query request. Fills validation response.
    Returns 0 on success.
*/
int ipc_client_query(ipc_client* client, const char* cwd, const char* input,
                     ipc_validation_resp* out);

/*
    Request completions for a prefix.
    Returns 0 on success.
*/
int ipc_client_complete(ipc_client* client, const char* prefix, uint32_t limit,
                        ipc_completions_resp* out);

/*
    Get the single best suggestion for a prefix.
    Returns 0 on success.
*/
int ipc_client_suggest(ipc_client* client, const char* prefix,
                       ipc_suggestion_resp* out);

/*
    Send shutdown request. Returns 0 on success.
*/
int ipc_client_shutdown(ipc_client* client);
