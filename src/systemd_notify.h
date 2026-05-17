#pragma once

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#else

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static inline int sd_notify(int unset_environment, const char* state) {
    const char* sock_path = getenv("NOTIFY_SOCKET");
    if (!sock_path)
        return 0;

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    if (SOCK_CLOEXEC == 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0';
    }

    size_t len = strlen(state);
    ssize_t rc = sendto(fd, state, len, MSG_NOSIGNAL, (struct sockaddr*) &addr, sizeof(addr));
    close(fd);

    if (rc < 0)
        return -1;

    if (unset_environment) {
        unsetenv("NOTIFY_SOCKET");
    }
    return 1;
}

#endif
