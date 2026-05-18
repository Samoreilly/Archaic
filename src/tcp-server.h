#pragma once

#include "../ipc/protocol.h"
#include "io/fileloader.h"
#include "threadpool.h"

typedef struct tcp_server tcp_server;

tcp_server* tcp_server_start(daemon_state* daemon, const char* bind_addr, int port,
                             const char* auth_token);

void tcp_server_stop(tcp_server* srv);
