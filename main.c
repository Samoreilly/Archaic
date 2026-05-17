#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ipc/protocol.h"
#include "src/config.h"
#include "src/io/fileloader.h"
#include "src/log.h"
#include "src/systemd_notify.h"
#include "test/test.h"

static volatile int running = 1;

static void* watchdog_thread_func(void* arg) {
    daemon_state* daemon = (daemon_state*) arg;
    while (running) {
        if (!atomic_load(&daemon->scanner_healthy)) {
            sd_notify(0, "STATUS=error\nWATCHDOG=1");
        } else if (atomic_load(&daemon->scanning)) {
            sd_notify(0, "STATUS=scanning\nWATCHDOG=1");
        } else {
            sd_notify(0, "STATUS=idle\nWATCHDOG=1");
        }
        sleep(15);
    }
    return NULL;
}

static void handle_signal(int sig) {
    (void) sig;
    running = 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--daemon") == 0) {
        archaic_config cfg;
        config_init_defaults(&cfg);
        config_load_default(&cfg);

        /* CLI args override config */
        const char* scan_path = argc > 2 ? argv[2] : cfg.daemon.scan_path;
        const char* sock_path = argc > 3 ? argv[3] : cfg.daemon.socket_path;

        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);

        log_init((log_level) cfg.daemon.log_level, stderr);
        LOG_INFO("main", "archaic daemon starting (v%d)", IPC_PROTOCOL_VERSION);

        daemon_state* daemon = daemon_init();
        if (!daemon) {
            LOG_ERR("main", "init failed");
            return 1;
        }

        LOG_INFO("main", "starting IPC on %s", sock_path);
        if (daemon_start_ipc(daemon, sock_path) != 0) {
            LOG_ERR("main", "IPC start failed");
            daemon_shutdown(daemon);
            return 1;
        }

        LOG_INFO("scanner", "scanning %s (background)", scan_path);
        daemon_run_scan(daemon, scan_path);
        daemon->rescan_interval_seconds = cfg.daemon.rescan_interval_seconds;
        daemon_start_rescan_timer(daemon);

        sd_notify(0, "READY=1");
        pthread_t watchdog_thread;
        pthread_create(&watchdog_thread, NULL, watchdog_thread_func, daemon);

        LOG_INFO("main", "ready for queries. scan running in background.");
        fflush(stdout);

        while (running) {
            sleep(1);
        }

        pthread_join(watchdog_thread, NULL);
        LOG_INFO("main", "shutting down...");
        daemon_shutdown(daemon);
        LOG_INFO("main", "stopped.");
        return 0;
    }

    if (argc > 1) {
        set_test_scan_path(argv[1]);
    }
    test_main();
    perf_main(argc > 1 ? argv[1] : "/home/sam/samdev");

    return 0;
}
