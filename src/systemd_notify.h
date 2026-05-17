#pragma once

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#else

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>

static inline int sd_notify(int unset_environment, const char *state) {
    const char *sock_path = getenv("NOTIFY_SOCKET");
    if (!sock_path) return 0; // NOTIFY_SOCKET not set, so return 0 (no systemd)

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    
    // Check for abstract socket
    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0';
    }

    size_t len = strlen(state);
    if (sendto(fd, state, len, MSG_NOSIGNAL, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    close(fd);
    if (unset_environment) {
        unsetenv("NOTIFY_SOCKET");
    }
    return 1;
}

#endif
