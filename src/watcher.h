#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#define WATCHER_MAX_ROOTS 16
#define WATCHER_MAX_EVENTS 4096
#define WATCHER_FALLBACK_INTERVAL 30

typedef enum {
    WATCHER_EVENT_CREATE,
    WATCHER_EVENT_DELETE,
    WATCHER_EVENT_MODIFY,
    WATCHER_EVENT_MOVE,
} watcher_event_type;

typedef struct {
    watcher_event_type type;
    char path[4096];
    int is_dir;
} watcher_event;

typedef struct watcher_cb_ctx {
    void (*on_event)(watcher_event_type type, const char* path, int is_dir, void* userdata);
    void* userdata;
} watcher_cb_ctx;

typedef struct fs_watcher {
    int fd;
    pthread_t thread;
    atomic_bool running;
    atomic_bool initialized;

    char roots[WATCHER_MAX_ROOTS][4096];
    int root_count;

    int watch_descriptors[WATCHER_MAX_EVENTS];
    char watch_paths[WATCHER_MAX_EVENTS][4096];
    int watch_count;
    pthread_mutex_t watch_lock;

    watcher_cb_ctx callback;

    int fallback_interval;
    pthread_t fallback_thread;
    atomic_bool fallback_running;
} fs_watcher;

fs_watcher* watcher_create(void);
void watcher_destroy(fs_watcher* w);

int watcher_add_root(fs_watcher* w, const char* path);
int watcher_start(fs_watcher* w, watcher_cb_ctx cb);
void watcher_stop(fs_watcher* w);
int watcher_is_running(fs_watcher* w);

void watcher_notify_scan_complete(fs_watcher* w);
int watcher_rescan_requested(fs_watcher* w);
