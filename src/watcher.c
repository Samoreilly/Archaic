#include "watcher.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#endif

#ifdef __APPLE__
#include <sys/event.h>
#endif

fs_watcher* watcher_create(void) {
    fs_watcher* w = calloc(1, sizeof(fs_watcher));
    if (!w)
        return NULL;

    w->fd = -1;
    w->root_count = 0;
    w->watch_count = 0;
    atomic_store(&w->running, false);
    atomic_store(&w->initialized, false);
    atomic_store(&w->fallback_running, false);
    w->fallback_interval = WATCHER_FALLBACK_INTERVAL;
    pthread_mutex_init(&w->watch_lock, NULL);
    return w;
}

void watcher_destroy(fs_watcher* w) {
    if (!w)
        return;
    if (atomic_load(&w->running))
        watcher_stop(w);
    pthread_mutex_destroy(&w->watch_lock);
    free(w);
}

int watcher_add_root(fs_watcher* w, const char* path) {
    if (!w || !path || w->root_count >= WATCHER_MAX_ROOTS)
        return -1;
    strncpy(w->roots[w->root_count], path, sizeof(w->roots[0]) - 1);
    w->roots[w->root_count][sizeof(w->roots[0]) - 1] = '\0';
    w->root_count++;
    return 0;
}

#ifdef __linux__

static int watcher_add_tree_linux(fs_watcher* w, const char* root) {
    struct stat st;
    if (stat(root, &st) < 0 || !S_ISDIR(st.st_mode))
        return -1;

    int wd = inotify_add_watch(w->fd, root,
                               IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY |
                                   IN_DELETE_SELF | IN_MOVE_SELF);
    if (wd < 0) {
        LOG_WARN("watcher", "inotify_add_watch failed for %s: %s", root, strerror(errno));
        return 0;
    }

    pthread_mutex_lock(&w->watch_lock);
    if (w->watch_count < WATCHER_MAX_EVENTS) {
        w->watch_descriptors[w->watch_count] = wd;
        strncpy(w->watch_paths[w->watch_count], root, sizeof(w->watch_paths[0]) - 1);
        w->watch_paths[w->watch_count][sizeof(w->watch_paths[0]) - 1] = '\0';
        w->watch_count++;
    }
    pthread_mutex_unlock(&w->watch_lock);

    DIR* dir = opendir(root);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        char child_path[4096];
        snprintf(child_path, sizeof(child_path), "%s/%s", root, entry->d_name);

        struct stat child_st;
        if (stat(child_path, &child_st) == 0 && S_ISDIR(child_st.st_mode)) {
            if (strcmp(entry->d_name, ".git") == 0 || strcmp(entry->d_name, "node_modules") == 0 ||
                strcmp(entry->d_name, ".venv") == 0 || strcmp(entry->d_name, "__pycache__") == 0 ||
                strcmp(entry->d_name, "build") == 0 || strcmp(entry->d_name, "dist") == 0) {
                continue;
            }
            watcher_add_tree_linux(w, child_path);
        }
    }
    closedir(dir);
    return 0;
}

static void* watcher_thread_linux(void* arg) {
    fs_watcher* w = (fs_watcher*) arg;
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));

    LOG_INFO("watcher", "inotify watcher started, monitoring %d roots", w->root_count);

    for (int i = 0; i < w->root_count; i++) {
        watcher_add_tree_linux(w, w->roots[i]);
    }

    atomic_store(&w->initialized, true);

    while (atomic_load(&w->running)) {
        ssize_t len = read(w->fd, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EINTR)
                continue;
            if (!atomic_load(&w->running))
                break;
            LOG_WARN("watcher", "inotify read error: %s", strerror(errno));
            sleep(1);
            continue;
        }

        char* ptr = buf;
        while (ptr < buf + len) {
            struct inotify_event* event = (struct inotify_event*) ptr;

            char event_path[4096] = {0};
            int found = 0;
            pthread_mutex_lock(&w->watch_lock);
            for (int i = 0; i < w->watch_count; i++) {
                if (w->watch_descriptors[i] == event->wd) {
                    if (event->len > 0) {
                        snprintf(event_path, sizeof(event_path), "%s/%s", w->watch_paths[i],
                                 event->name);
                    } else {
                        strncpy(event_path, w->watch_paths[i], sizeof(event_path) - 1);
                    }
                    found = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&w->watch_lock);

            if (!found) {
                ptr += sizeof(struct inotify_event) + event->len;
                continue;
            }

            watcher_event_type type;
            if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                type = WATCHER_EVENT_CREATE;

                if (event->mask & IN_ISDIR) {
                    watcher_add_tree_linux(w, event_path);
                }
            } else if (event->mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF)) {
                type = WATCHER_EVENT_DELETE;

                if (event->mask & IN_ISDIR) {
                    pthread_mutex_lock(&w->watch_lock);
                    for (int i = 0; i < w->watch_count; i++) {
                        if (w->watch_descriptors[i] == event->wd) {
                            inotify_rm_watch(w->fd, event->wd);
                            if (i < w->watch_count - 1) {
                                w->watch_descriptors[i] = w->watch_descriptors[w->watch_count - 1];
                                memcpy(w->watch_paths[i], w->watch_paths[w->watch_count - 1],
                                       sizeof(w->watch_paths[0]));
                            }
                            w->watch_count--;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&w->watch_lock);
                }
            } else if (event->mask & IN_MODIFY) {
                type = WATCHER_EVENT_MODIFY;
            } else if (event->mask & IN_MOVED_FROM) {
                type = WATCHER_EVENT_MOVE;
            } else {
                ptr += sizeof(struct inotify_event) + event->len;
                continue;
            }

            if (w->callback.on_event) {
                w->callback.on_event(type, event_path, event->mask & IN_ISDIR ? 1 : 0,
                                     w->callback.userdata);
            }

            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    LOG_INFO("watcher", "inotify watcher stopped");
    return NULL;
}

#endif

#ifdef __APPLE__

typedef struct {
    int kq;
    int pipefd[2];
} kq_watched_dir;

static int watcher_add_tree_kqueue(fs_watcher* w, const char* root) {
    struct stat st;
    if (stat(root, &st) < 0 || !S_ISDIR(st.st_mode))
        return -1;

    int dirfd = open(root, O_RDONLY);
    if (dirfd < 0)
        return -1;

    struct kevent change;
    EV_SET(&change, dirfd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE, 0, (void*) strdup(root));

    if (kevent(w->fd, &change, 1, NULL, 0, NULL) < 0) {
        LOG_WARN("watcher", "kevent add failed for %s: %s", root, strerror(errno));
        close(dirfd);
        free(change.udata);
        return 0;
    }

    pthread_mutex_lock(&w->watch_lock);
    if (w->watch_count < WATCHER_MAX_EVENTS) {
        w->watch_descriptors[w->watch_count] = dirfd;
        strncpy(w->watch_paths[w->watch_count], root, sizeof(w->watch_paths[0]) - 1);
        w->watch_count++;
    }
    pthread_mutex_unlock(&w->watch_lock);

    DIR* dir = opendir(root);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        char child_path[4096];
        snprintf(child_path, sizeof(child_path), "%s/%s", root, entry->d_name);

        struct stat child_st;
        if (stat(child_path, &child_st) == 0 && S_ISDIR(child_st.st_mode)) {
            if (strcmp(entry->d_name, ".git") == 0 || strcmp(entry->d_name, "node_modules") == 0 ||
                strcmp(entry->d_name, ".venv") == 0 || strcmp(entry->d_name, "__pycache__") == 0 ||
                strcmp(entry->d_name, "build") == 0 || strcmp(entry->d_name, "dist") == 0) {
                continue;
            }
            watcher_add_tree_kqueue(w, child_path);
        }
    }
    closedir(dir);
    return 0;
}

static void* watcher_thread_kqueue(void* arg) {
    fs_watcher* w = (fs_watcher*) arg;
    struct kevent event;
    struct timespec timeout;
    timeout.tv_sec = 1;
    timeout.tv_nsec = 0;

    LOG_INFO("watcher", "kqueue watcher started, monitoring %d roots", w->root_count);

    for (int i = 0; i < w->root_count; i++) {
        watcher_add_tree_kqueue(w, w->roots[i]);
    }

    atomic_store(&w->initialized, true);

    while (atomic_load(&w->running)) {
        int nev = kevent(w->fd, NULL, 0, &event, 1, &timeout);
        if (nev < 0) {
            if (errno == EINTR)
                continue;
            if (!atomic_load(&w->running))
                break;
            LOG_WARN("watcher", "kevent error: %s", strerror(errno));
            sleep(1);
            continue;
        }

        if (nev == 0)
            continue;

        char* watch_path = (char*) event.udata;
        if (!watch_path)
            continue;

        watcher_event_type type;
        if (event.fflags & (NOTE_WRITE | NOTE_REVOKE)) {
            type = WATCHER_EVENT_MODIFY;
        } else if (event.fflags & NOTE_DELETE) {
            type = WATCHER_EVENT_DELETE;
        } else if (event.fflags & NOTE_RENAME) {
            type = WATCHER_EVENT_MOVE;
        } else {
            continue;
        }

        if (w->callback.on_event) {
            w->callback.on_event(type, watch_path, 1, w->callback.userdata);
        }

        free(watch_path);
    }

    LOG_INFO("watcher", "kqueue watcher stopped");

    pthread_mutex_lock(&w->watch_lock);
    for (int i = 0; i < w->watch_count; i++) {
        close(w->watch_descriptors[i]);
    }
    pthread_mutex_unlock(&w->watch_lock);

    return NULL;
}

#endif

static void* fallback_timer_thread(void* arg) {
    fs_watcher* w = (fs_watcher*) arg;

    LOG_INFO("watcher", "fallback polling started (interval: %ds)", w->fallback_interval);

    while (atomic_load(&w->fallback_running)) {
        for (int i = 0; i < w->fallback_interval && atomic_load(&w->fallback_running); i++) {
            sleep(1);
        }

        if (!atomic_load(&w->fallback_running))
            break;

        if (w->callback.on_event) {
            w->callback.on_event(WATCHER_EVENT_MODIFY, "", 0, w->callback.userdata);
        }
    }

    LOG_INFO("watcher", "fallback polling stopped");
    return NULL;
}

int watcher_start(fs_watcher* w, watcher_cb_ctx cb) {
    if (!w || w->root_count == 0)
        return -1;

    w->callback = cb;
    atomic_store(&w->running, true);

    int started = 0;

#ifdef __linux__
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd >= 0) {
        LOG_INFO("watcher", "using inotify for filesystem watching");
        if (pthread_create(&w->thread, NULL, watcher_thread_linux, w) == 0) {
            started = 1;
        } else {
            LOG_WARN("watcher", "inotify thread creation failed, falling back to polling");
            close(w->fd);
            w->fd = -1;
        }
    } else {
        LOG_WARN("watcher", "inotify_init failed: %s, falling back to polling", strerror(errno));
    }
#endif

#ifdef __APPLE__
    w->fd = kqueue();
    if (w->fd >= 0) {
        LOG_INFO("watcher", "using kqueue for filesystem watching");
        if (pthread_create(&w->thread, NULL, watcher_thread_kqueue, w) == 0) {
            started = 1;
        } else {
            LOG_WARN("watcher", "kqueue thread creation failed, falling back to polling");
            close(w->fd);
            w->fd = -1;
        }
    } else {
        LOG_WARN("watcher", "kqueue creation failed: %s, falling back to polling", strerror(errno));
    }
#endif

    if (!started) {
        LOG_INFO("watcher", "no native filesystem watcher available, using polling fallback");
        atomic_store(&w->fallback_running, true);
        if (pthread_create(&w->fallback_thread, NULL, fallback_timer_thread, w) != 0) {
            atomic_store(&w->fallback_running, false);
            atomic_store(&w->running, false);
            return -1;
        }
        atomic_store(&w->initialized, true);
    }

    return started ? 0 : 0;
}

void watcher_stop(fs_watcher* w) {
    if (!w)
        return;

    if (!atomic_load(&w->running))
        return;

    atomic_store(&w->running, false);

#ifdef __linux__
    if (w->fd >= 0) {
        pthread_mutex_lock(&w->watch_lock);
        for (int i = 0; i < w->watch_count; i++) {
            inotify_rm_watch(w->fd, w->watch_descriptors[i]);
        }
        pthread_mutex_unlock(&w->watch_lock);
    }
#endif

    if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;
    }

    if (atomic_load(&w->initialized)) {
        pthread_join(w->thread, NULL);
        atomic_store(&w->initialized, false);
    }

    if (atomic_load(&w->fallback_running)) {
        atomic_store(&w->fallback_running, false);
        pthread_join(w->fallback_thread, NULL);
    }
}

int watcher_is_running(fs_watcher* w) {
    if (!w)
        return 0;
    return atomic_load(&w->running) ? 1 : 0;
}

void watcher_notify_scan_complete(fs_watcher* w) {
    if (!w)
        return;

#ifdef __linux__
    if (w->fd >= 0 && atomic_load(&w->running)) {
        pthread_mutex_lock(&w->watch_lock);
        for (int i = 0; i < w->watch_count; i++) {
            inotify_rm_watch(w->fd, w->watch_descriptors[i]);
        }
        w->watch_count = 0;
        pthread_mutex_unlock(&w->watch_lock);

        for (int i = 0; i < w->root_count; i++) {
            watcher_add_tree_linux(w, w->roots[i]);
        }
    }
#endif

#ifdef __APPLE__
    if (w->fd >= 0 && atomic_load(&w->running)) {
        for (int i = 0; i < w->root_count; i++) {
            watcher_add_tree_kqueue(w, w->roots[i]);
        }
    }
#endif
}

int watcher_rescan_requested(fs_watcher* w) {
    (void) w;
    return 0;
}