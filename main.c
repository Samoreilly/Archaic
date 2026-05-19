#ifndef ARCHAIC_VERSION
#define ARCHAIC_VERSION "0.9.0"
#endif
#ifndef ARCHAIC_COMMIT
#define ARCHAIC_COMMIT "unknown"
#endif
#ifndef ARCHAIC_BUILD_DATE
#define ARCHAIC_BUILD_DATE __DATE__
#endif

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc/protocol.h"
#include "src/config.h"
#include "src/io/fileloader.h"
#include "src/log.h"
#include "src/systemd_notify.h"
#include "test/test.h"

static volatile int running = 1;
static volatile int sighup_received = 0;
static volatile int sigterm_received = 0;
static volatile int sigusr1_received = 0;
static volatile int sigusr2_received = 0;
static char pid_file_path[4096] = {0};

static void cleanup_pid_file(void) {
    if (pid_file_path[0] != '\0') {
        unlink(pid_file_path);
        pid_file_path[0] = '\0';
    }
}

static void write_pid_file(const char* sock_path) {
    snprintf(pid_file_path, sizeof(pid_file_path), "%s.pid", sock_path);
    FILE* f = fopen(pid_file_path, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

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

static void handle_term(int sig) {
    (void) sig;
    sigterm_received = 1;
    running = 0;
}

static void handle_sighup(int sig) {
    (void) sig;
    sighup_received = 1;
}

static void handle_sigusr1(int sig) {
    (void) sig;
    sigusr1_received = 1;
}

static void handle_sigusr2(int sig) {
    (void) sig;
    sigusr2_received = 1;
}

static void apply_config_reload(daemon_state* daemon) {
    archaic_config cfg;
    config_init_defaults(&cfg);
    if (config_load_default(&cfg) == 0) {
        config_expand_vars(&cfg);
        config_sandbox_validate(&cfg);
        config_validate_paths(&cfg);
        LOG_INFO("main", "config reloaded from disk");

        /* Apply live-reloadable settings */
        daemon->rescan_interval_seconds = cfg.daemon.rescan_interval_seconds;
        log_init((log_level) cfg.daemon.log_level, stderr);

        LOG_INFO("main", "rescan interval: %ds, log level: %d", cfg.daemon.rescan_interval_seconds,
                 cfg.daemon.log_level);
    } else {
        LOG_WARN("main", "no config file found for reload, keeping current settings");
    }
}

int main(int argc, char* argv[]) {
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("archaic " ARCHAIC_VERSION " (commit: " ARCHAIC_COMMIT ", date: " ARCHAIC_BUILD_DATE
               ")\n");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--daemon") == 0) {
        archaic_config cfg;
        config_init_defaults(&cfg);
        config_load_default(&cfg);
        config_expand_vars(&cfg);
        config_sandbox_validate(&cfg);

        /* CLI args override config */
        const char* scan_path = (argc > 2 && argv[2][0] != '\0') ? argv[2] : NULL;
        const char* sock_path = argc > 3 ? argv[3] : cfg.daemon.socket_path;

        const char* scan_roots[CONFIG_MAX_ROOTS];
        int scan_root_count = 0;

        if (scan_path) {
            scan_roots[0] = scan_path;
            scan_root_count = 1;
        } else if (cfg.daemon.scan_path_count > 0) {
            scan_root_count = cfg.daemon.scan_path_count;
            for (int i = 0; i < scan_root_count; i++) {
                scan_roots[i] = cfg.daemon.scan_paths[i];
            }
        } else {
            scan_roots[0] = cfg.daemon.scan_path;
            scan_root_count = 1;
        }

        /* Auto-detect scanner threads if set to 0 */
        if (cfg.daemon.scan_threads <= 0) {
            long nproc = sysconf(_SC_NPROCESSORS_ONLN);
            cfg.daemon.scan_threads = (nproc > 0 && nproc < SCANNER_MAX_THREADS) ? (int) nproc : 4;
            LOG_INFO("main", "auto-detected %d scanner threads", cfg.daemon.scan_threads);
        }

        struct sigaction sa_int, sa_term, sa_hup, sa_pipe;
        memset(&sa_int, 0, sizeof(sa_int));
        sa_int.sa_handler = handle_signal;
        sigaction(SIGINT, &sa_int, NULL);
        memset(&sa_term, 0, sizeof(sa_term));
        sa_term.sa_handler = handle_term;
        sigaction(SIGTERM, &sa_term, NULL);
        memset(&sa_hup, 0, sizeof(sa_hup));
        sa_hup.sa_handler = handle_sighup;
        sigaction(SIGHUP, &sa_hup, NULL);
        memset(&sa_pipe, 0, sizeof(sa_pipe));
        sa_pipe.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa_pipe, NULL);
        {
            struct sigaction sa_usr2;
            memset(&sa_usr2, 0, sizeof(sa_usr2));
            sa_usr2.sa_handler = handle_sigusr2;
            sigaction(SIGUSR2, &sa_usr2, NULL);
        }
        {
            struct sigaction sa_usr1;
            memset(&sa_usr1, 0, sizeof(sa_usr1));
            sa_usr1.sa_handler = handle_sigusr1;
            sigaction(SIGUSR1, &sa_usr1, NULL);
        }

        log_init((log_level) cfg.daemon.log_level, stderr);
        LOG_INFO("main", "archaic daemon starting (v%d)", IPC_PROTOCOL_VERSION);

        daemon_state* daemon = daemon_init();
        if (!daemon) {
            LOG_ERR("main", "init failed");
            return 1;
        }

        LOG_INFO("main", "starting IPC on %s", sock_path);

        if (access(sock_path, F_OK) == 0) {
            int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (test_fd >= 0) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
                if (connect(test_fd, (struct sockaddr*) &addr, sizeof(addr)) == 0) {
                    close(test_fd);
                    LOG_ERR("main", "another daemon is already running on %s", sock_path);
                    return 1;
                }
                close(test_fd);
                LOG_WARN("main", "removing stale socket %s", sock_path);
                unlink(sock_path);
            }
        }

        umask(077);

        if (daemon_start_ipc(daemon, sock_path) != 0) {
            LOG_ERR("main", "IPC start failed");
            daemon_shutdown(daemon);
            cleanup_pid_file();
            return 1;
        }

        write_pid_file(sock_path);
        atexit(cleanup_pid_file);

        if (scan_root_count == 1) {
            LOG_INFO("scanner", "scanning %s (background)", scan_roots[0]);
            daemon_run_scan(daemon, scan_roots[0]);
        } else {
            LOG_INFO("scanner", "scanning %d roots (background)", scan_root_count);
            for (int i = 0; i < scan_root_count; i++) {
                LOG_INFO("scanner", "  root[%d]: %s", i, scan_roots[i]);
            }
            daemon_run_scan_multi(daemon, scan_roots, scan_root_count);
        }
        daemon->rescan_interval_seconds = cfg.daemon.rescan_interval_seconds;
        daemon_start_rescan_timer(daemon);

        daemon->watcher = watcher_create();
        if (daemon->watcher) {
            for (int i = 0; i < scan_root_count; i++) {
                watcher_add_root(daemon->watcher, scan_roots[i]);
            }
            watcher_cb_ctx wcb;
            wcb.on_event = NULL;
            wcb.userdata = daemon;
            atomic_store(&daemon->watcher_dirty, false);

            if (watcher_start(daemon->watcher, wcb) == 0) {
                LOG_INFO("main", "filesystem watcher started");
            } else {
                LOG_WARN("main", "filesystem watcher failed to start");
                watcher_destroy(daemon->watcher);
                daemon->watcher = NULL;
            }
        }

        sd_notify(0, "READY=1");
        daemon_prefetch_common_prefixes(daemon);

        pthread_t watchdog_thread;
        pthread_create(&watchdog_thread, NULL, watchdog_thread_func, daemon);

        LOG_INFO("main", "ready for queries. scan running in background.");
        LOG_INFO("main", "send SIGHUP to reload configuration");
        fflush(stdout);

        while (running) {
            if (sighup_received) {
                sighup_received = 0;
                LOG_INFO("main", "SIGHUP received, reloading configuration...");
                apply_config_reload(daemon);
                atomic_store(&daemon->config_reload_requested, true);
            }
            if (sigusr2_received) {
                sigusr2_received = 0;
                LOG_INFO("main", "SIGUSR2 received, dumping state...");
                char state_path[4096];
                snprintf(state_path, sizeof(state_path), "%s.state", sock_path);
                daemon_save_state(daemon, state_path);
                LOG_INFO("main", "state saved to %s", state_path);
            }
            if (sigusr1_received) {
                sigusr1_received = 0;
                LOG_INFO("main", "SIGUSR1 received, saving state and continuing...");
                char state_path[4096];
                snprintf(state_path, sizeof(state_path), "%s.state", sock_path);
                daemon_save_state(daemon, state_path);
            }
            sleep(1);
        }

        pthread_join(watchdog_thread, NULL);
        LOG_INFO("main", "shutting down...");

        if (daemon->watcher) {
            watcher_stop(daemon->watcher);
            watcher_destroy(daemon->watcher);
            daemon->watcher = NULL;
        }

        if (sigterm_received) {
            LOG_INFO("main", "signal-safe shutdown: saving state before exit...");
            char state_path[4096];
            snprintf(state_path, sizeof(state_path), "%s.state", sock_path);
            daemon_save_state(daemon, state_path);
        } else {
            LOG_INFO("main", "saving state before exit...");
            char state_path[4096];
            snprintf(state_path, sizeof(state_path), "%s.state", sock_path);
            daemon_save_state(daemon, state_path);
        }

        daemon_shutdown(daemon);
        cleanup_pid_file();
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
