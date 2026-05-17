#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include "test/test.h"
#include "src/io/fileloader.h"
#include "ipc/protocol.h"

static volatile int running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--daemon") == 0) {
        const char* scan_path = argc > 2 ? argv[2] : "/home/sam/samdev";
        const char* sock_path = argc > 3 ? argv[3] : IPC_SOCK_PATH;

        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);

        printf("[daemon] initializing...\n");
        daemon_state* daemon = daemon_init();
        if (!daemon) {
            fprintf(stderr, "[daemon] init failed\n");
            return 1;
        }

        printf("[daemon] starting IPC on %s\n", sock_path);
        if (daemon_start_ipc(daemon, sock_path) != 0) {
            fprintf(stderr, "[daemon] IPC start failed\n");
            daemon_shutdown(daemon);
            return 1;
        }

        printf("[daemon] scanning %s (background)...\n", scan_path);
        daemon_run_scan(daemon, scan_path);
        printf("[daemon] ready for queries. scan running in background.\n");
        fflush(stdout);

        while (running) {
            sleep(1);
        }

        printf("[daemon] shutting down...\n");
        daemon_shutdown(daemon);
        printf("[daemon] stopped.\n");
        return 0;
    }

    if (argc > 1) {
        set_test_scan_path(argv[1]);
    }
    test_main();
    perf_main(argc > 1 ? argv[1] : "/home/sam/samdev");

    return 0;
}
