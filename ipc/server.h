#pragma once

#include "../src/io/fileloader.h"
#include "../src/threadpool.h"
#include "protocol.h"

typedef struct ipc_server ipc_server;

/*
    Start the IPC server on a Unix domain socket.
    Returns NULL on failure.
    The server runs in a background thread.
*/
ipc_server* ipc_server_start(daemon_state* daemon, const char* sock_path);

/*
    Stop the IPC server and clean up.
    Blocks until the server thread exits.
*/
void ipc_server_stop(ipc_server* srv);
