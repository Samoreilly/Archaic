#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Ignore helpers ──────────────────────────────────────────────────────── */

static void add_ignore_dir(archaic_config* cfg, const char* dir) {
    if (cfg->scanner.ignore_dir_count < CONFIG_MAX_IGNORE) {
        strncpy(cfg->scanner.ignore_dirs[cfg->scanner.ignore_dir_count], dir,
                CONFIG_MAX_IGNORE_LEN - 1);
        cfg->scanner.ignore_dirs[cfg->scanner.ignore_dir_count][CONFIG_MAX_IGNORE_LEN - 1] = '\0';
        cfg->scanner.ignore_dir_count++;
    }
}

static void add_ignore_file(archaic_config* cfg, const char* file) {
    if (cfg->scanner.ignore_file_count < CONFIG_MAX_IGNORE) {
        strncpy(cfg->scanner.ignore_files[cfg->scanner.ignore_file_count], file,
                CONFIG_MAX_IGNORE_LEN - 1);
        cfg->scanner.ignore_files[cfg->scanner.ignore_file_count][CONFIG_MAX_IGNORE_LEN - 1] = '\0';
        cfg->scanner.ignore_file_count++;
    }
}

static void set_default_ignores(archaic_config* cfg) {
    add_ignore_dir(cfg, ".git");
    add_ignore_dir(cfg, ".svn");
    add_ignore_dir(cfg, ".hg");
    add_ignore_dir(cfg, "node_modules");
    add_ignore_dir(cfg, ".next");
    add_ignore_dir(cfg, "dist");
    add_ignore_dir(cfg, "build");
    add_ignore_dir(cfg, "target");
    add_ignore_dir(cfg, "__pycache__");
    add_ignore_dir(cfg, ".venv");
    add_ignore_dir(cfg, "venv");
    add_ignore_dir(cfg, ".tox");
    add_ignore_dir(cfg, ".cache");
    add_ignore_dir(cfg, ".cargo");
    add_ignore_dir(cfg, "vendor");
    add_ignore_dir(cfg, ".idea");
    add_ignore_dir(cfg, ".vscode");
    add_ignore_dir(cfg, ".eclipse");
    add_ignore_file(cfg, "*.pyc");
    add_ignore_file(cfg, "*.pyo");
    add_ignore_file(cfg, "*.o");
    add_ignore_file(cfg, "*.so");
    add_ignore_file(cfg, "*.dylib");
    add_ignore_file(cfg, "*.class");
    add_ignore_file(cfg, "*.exe");
    add_ignore_file(cfg, "*.dll");
    add_ignore_file(cfg, "*.log");
    add_ignore_file(cfg, ".DS_Store");
    add_ignore_file(cfg, "Thumbs.db");
}

/* ── Defaults ────────────────────────────────────────────────────────────── */

void config_default_socket_path(char* buf, size_t n) {
    if (!buf || n == 0)
        return;
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) {
        snprintf(buf, n, "%s/archaic.sock", rt);
        return;
    }
    snprintf(buf, n, "/tmp/archaic-%d.sock", (int) getuid());
}

void config_default_state_path(char* buf, size_t n) {
    if (!buf || n == 0)
        return;
    const char* cache = getenv("XDG_CACHE_HOME");
    if (cache && cache[0]) {
        snprintf(buf, n, "%s/archaic/state.bin", cache);
        return;
    }
    const char* home = getenv("HOME");
    if (home && home[0]) {
        snprintf(buf, n, "%s/.cache/archaic/state.bin", home);
        return;
    }
    snprintf(buf, n, "/tmp/archaic-%d.state", (int) getuid());
}

void config_ensure_parent_dir(const char* file) {
    if (!file || file[0] == '\0')
        return;
    char tmp[4096];
    strncpy(tmp, file, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

void config_default_roots_path(char* buf, size_t n) {
    if (!buf || n == 0)
        return;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(buf, n, "%s/archaic/roots", xdg);
        return;
    }
    const char* home = getenv("HOME");
    if (home && home[0]) {
        snprintf(buf, n, "%s/.config/archaic/roots", home);
        return;
    }
    snprintf(buf, n, "/tmp/archaic-%d.roots", (int) getuid());
}

static int config_has_root(const archaic_config* cfg, const char* path) {
    if (!cfg || !path || path[0] == '\0')
        return 0;
    if (cfg->daemon.scan_path[0] && strcmp(cfg->daemon.scan_path, path) == 0)
        return 1;
    for (int i = 0; i < cfg->daemon.scan_path_count; i++) {
        if (strcmp(cfg->daemon.scan_paths[i], path) == 0)
            return 1;
    }
    return 0;
}

static int config_append_root(archaic_config* cfg, const char* path) {
    if (!cfg || !path || path[0] == '\0' || config_has_root(cfg, path))
        return 0;
    if (cfg->daemon.scan_path_count >= CONFIG_MAX_ROOTS)
        return -1;
    strncpy(cfg->daemon.scan_paths[cfg->daemon.scan_path_count], path, CONFIG_MAX_STRING - 1);
    cfg->daemon.scan_paths[cfg->daemon.scan_path_count][CONFIG_MAX_STRING - 1] = '\0';
    cfg->daemon.scan_path_count++;
    return 0;
}

void config_pick_workspace_roots(archaic_config* cfg) {
    if (!cfg)
        return;
    const char* home = getenv("HOME");
    if (!home || home[0] == '\0') {
        strncpy(cfg->daemon.scan_path, "/", sizeof(cfg->daemon.scan_path) - 1);
        cfg->daemon.scan_path_count = 0;
        return;
    }
    static const char* const names[] = {"src",     "projects", "project", "dev", "code",
                                        "git",     "repos",    "samdev",  "work"};
    cfg->daemon.scan_path[0] = '\0';
    cfg->daemon.scan_path_count = 0;
    char buf[4096];
    struct stat st;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int n = snprintf(buf, sizeof(buf), "%s/%s", home, names[i]);
        if (n < 0 || (size_t) n >= sizeof(buf))
            continue;
        if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
            config_append_root(cfg, buf);
    }
    if (cfg->daemon.scan_path_count == 0)
        strncpy(cfg->daemon.scan_path, home, sizeof(cfg->daemon.scan_path) - 1);
}

void config_load_roots_file(archaic_config* cfg) {
    if (!cfg)
        return;
    char path[4096];
    config_default_roots_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#' || *p == '\n')
            continue;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
            p[--n] = '\0';
        if (n == 0)
            continue;
        config_append_root(cfg, p);
        if (cfg->daemon.scan_path[0] && strcmp(cfg->daemon.scan_path, p) == 0)
            continue;
    }
    fclose(f);
}

int config_roots_add(const char* path) {
    if (!path || path[0] == '\0')
        return -1;
    char resolved[4096];
    if (!realpath(path, resolved))
        strncpy(resolved, path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;

    char list[4096];
    config_default_roots_path(list, sizeof(list));
    config_ensure_parent_dir(list);

    FILE* in = fopen(list, "r");
    if (in) {
        char line[4096];
        while (fgets(line, sizeof(line), in)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
            if (strcmp(line, resolved) == 0) {
                fclose(in);
                return 0;
            }
        }
        fclose(in);
    }
    FILE* out = fopen(list, "a");
    if (!out)
        return -1;
    fprintf(out, "%s\n", resolved);
    fclose(out);
    return 0;
}

int config_roots_remove(const char* path) {
    if (!path || path[0] == '\0')
        return -1;
    char resolved[4096];
    if (!realpath(path, resolved))
        strncpy(resolved, path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';

    char list[4096];
    config_default_roots_path(list, sizeof(list));
    FILE* in = fopen(list, "r");
    if (!in)
        return 0;
    char tmp[4104];
    snprintf(tmp, sizeof(tmp), "%s.tmp", list);
    FILE* out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return -1;
    }
    char line[4096];
    while (fgets(line, sizeof(line), in)) {
        char keep[4096];
        strncpy(keep, line, sizeof(keep) - 1);
        keep[sizeof(keep) - 1] = '\0';
        size_t n = strlen(keep);
        while (n > 0 && (keep[n - 1] == '\n' || keep[n - 1] == '\r'))
            keep[--n] = '\0';
        if (strcmp(keep, resolved) == 0 || strcmp(keep, path) == 0)
            continue;
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (rename(tmp, list) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void config_init_defaults(archaic_config* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->daemon.scan_threads = 4;
    cfg->daemon.max_depth = 10;
    cfg->daemon.rescan_interval_seconds = 300;
    cfg->daemon.log_level = 1;
    cfg->daemon.colored_output = true;
    config_pick_workspace_roots(cfg);
    config_default_socket_path(cfg->daemon.socket_path, sizeof(cfg->daemon.socket_path));

    cfg->storage.max_buckets = 65536;
    cfg->storage.max_nodes_per_bucket = 100000;
    cfg->storage.cache_max_entries = 1024;
    cfg->storage.cache_ttl_seconds = 2;
    cfg->storage.recent_files_capacity = 50;
    cfg->storage.case_insensitive = false;
    cfg->storage.max_total_nodes = 0;
    cfg->storage.max_memory_mb = 512;

    cfg->scoring.weight_frequency = 0.40;
    cfg->scoring.weight_recency = 0.30;
    cfg->scoring.weight_depth = 0.15;
    cfg->scoring.weight_type = 0.10;
    cfg->scoring.weight_cwd_proximity = 0.05;
    cfg->scoring.min_score_threshold = 0.0;
    cfg->scoring.hidden_file_penalty = 0.50;

    cfg->fish.command_count = 0;

    cfg->scanner.ignore_dir_count = 0;
    cfg->scanner.ignore_file_count = 0;
    set_default_ignores(cfg);

    cfg->bookmarks.count = 0;
}

void config_sandbox_validate(archaic_config* cfg) {
    if (cfg->daemon.scan_threads < 1)
        cfg->daemon.scan_threads = 1;
    if (cfg->daemon.scan_threads > 32)
        cfg->daemon.scan_threads = 32;
    if (cfg->daemon.max_depth < 1)
        cfg->daemon.max_depth = 1;
    if (cfg->daemon.max_depth > 20)
        cfg->daemon.max_depth = 20;
    if (cfg->daemon.rescan_interval_seconds < 0)
        cfg->daemon.rescan_interval_seconds = 0;
    if (cfg->daemon.rescan_interval_seconds > 86400)
        cfg->daemon.rescan_interval_seconds = 86400;
    if (cfg->daemon.log_level < 0)
        cfg->daemon.log_level = 0;
    if (cfg->daemon.log_level > 3)
        cfg->daemon.log_level = 3;
    if (cfg->storage.max_buckets < 1024)
        cfg->storage.max_buckets = 1024;
    if (cfg->storage.max_buckets > 1048576)
        cfg->storage.max_buckets = 1048576;
    if (cfg->storage.cache_max_entries < 16)
        cfg->storage.cache_max_entries = 16;
    if (cfg->storage.cache_max_entries > 65536)
        cfg->storage.cache_max_entries = 65536;
    if (cfg->storage.cache_ttl_seconds < 1)
        cfg->storage.cache_ttl_seconds = 1;
    if (cfg->storage.cache_ttl_seconds > 300)
        cfg->storage.cache_ttl_seconds = 300;
    if (cfg->storage.max_memory_mb < 64)
        cfg->storage.max_memory_mb = 64;
    if (cfg->storage.max_memory_mb > 8192)
        cfg->storage.max_memory_mb = 8192;
    if (cfg->scoring.weight_frequency < 0.0)
        cfg->scoring.weight_frequency = 0.0;
    if (cfg->scoring.weight_frequency > 1.0)
        cfg->scoring.weight_frequency = 1.0;
    if (cfg->scoring.weight_recency < 0.0)
        cfg->scoring.weight_recency = 0.0;
    if (cfg->scoring.weight_recency > 1.0)
        cfg->scoring.weight_recency = 1.0;
}

/* ── Environment variable expansion ──────────────────────────────────────── */

/* Expand environment variables in a path string.
   Supports $VAR and ${VAR} syntax, expanding to the variable's value.
   Falls back to the literal string if the variable is not set.
   Writes the result to dst (at most dst_size bytes including NUL). */
void expand_env_vars(const char* src, char* dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return;
    }

    size_t si = 0, di = 0;
    size_t src_len = strlen(src);

    while (si < src_len && di < dst_size - 1) {
        if (src[si] == '$') {
            if (si + 1 < src_len && src[si + 1] == '$') {
                dst[di++] = '$';
                si += 2;
                continue;
            }

            si++;

            if (si < src_len && src[si] == '{') {
                si++;
                char varname[256];
                size_t vi = 0;
                while (si < src_len && src[si] != '}' && vi < sizeof(varname) - 1)
                    varname[vi++] = src[si++];
                varname[vi] = '\0';
                if (si < src_len && src[si] == '}')
                    si++;

                const char* val = getenv(varname);
                if (val) {
                    size_t vlen = strlen(val);
                    for (size_t k = 0; k < vlen && di < dst_size - 1; k++)
                        dst[di++] = val[k];
                } else {
                    dst[di++] = '$';
                    dst[di++] = '{';
                    for (size_t k = 0; k < vi && di < dst_size - 2; k++)
                        dst[di++] = varname[k];
                    if (di < dst_size - 1)
                        dst[di++] = '}';
                }
            } else {
                char varname[256];
                size_t vi = 0;
                while (si < src_len && (isalnum((unsigned char) src[si]) || src[si] == '_') &&
                       vi < sizeof(varname) - 1)
                    varname[vi++] = src[si++];
                varname[vi] = '\0';

                if (vi > 0) {
                    const char* val = getenv(varname);
                    if (val) {
                        size_t vlen = strlen(val);
                        for (size_t k = 0; k < vlen && di < dst_size - 1; k++)
                            dst[di++] = val[k];
                    }
                } else {
                    dst[di++] = '$';
                }
            }
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static char* trim_left(char* s) {
    while (*s && isspace((unsigned char) *s))
        s++;
    return s;
}

static void trim_right(char* s) {
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char) *(end - 1)))
        end--;
    *end = '\0';
}

static char* trim(char* s) {
    s = trim_left(s);
    trim_right(s);
    return s;
}

/* Parse a quoted string in-place. Strips surrounding quotes.
   Handles \\, \n, \t escapes. Returns pointer inside buf. */
static char* parse_string(char* buf) {
    char* p = trim(buf);
    size_t len = strlen(p);
    if (len < 2 || p[0] != '"' || p[len - 1] != '"')
        return NULL;

    /* Strip quotes */
    p[len - 1] = '\0';
    char* src = p + 1;
    char* dst = p;

    while (*src) {
        if (*src == '\\' && src[1]) {
            src++;
            switch (*src) {
            case 'n':
                *dst++ = '\n';
                break;
            case 't':
                *dst++ = '\t';
                break;
            case '\\':
                *dst++ = '\\';
                break;
            case '"':
                *dst++ = '"';
                break;
            default:
                *dst++ = '\\';
                *dst++ = *src;
                break;
            }
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return p;
}

/* Parse an integer. Returns 0 on success, -1 on error. */
static int parse_int(const char* s, int* out) {
    char* end;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (errno || end == s || *trim(end) != '\0')
        return -1;
    *out = (int) val;
    return 0;
}

/* Parse a double. Returns 0 on success, -1 on error. */
static int parse_double(const char* s, double* out) {
    char* end;
    errno = 0;
    double val = strtod(s, &end);
    if (errno || end == s || *trim(end) != '\0')
        return -1;
    *out = val;
    return 0;
}

/* ── Section / key dispatch ──────────────────────────────────────────────── */

typedef enum {
    TYPE_STRING,
    TYPE_INT,
    TYPE_DOUBLE,
    TYPE_BOOL,
    TYPE_STRING_ARRAY,
    TYPE_SCANNER_ARRAY,
    TYPE_DAEMON_PATHS_ARRAY
} value_type;

typedef struct {
    const char* key;
    value_type type;
    void* offset; /* offset into archaic_config */
} field_map;

#define FOFFSET(section, field)                                                                    \
    ((void*) (offsetof(archaic_config, section) +                                                  \
              offsetof(typeof(((archaic_config*) 0)->section), field)))

static int set_field(archaic_config* cfg, const field_map* map, int map_len, const char* key,
                     char* value) {
    for (int i = 0; i < map_len; i++) {
        if (strcmp(map[i].key, key) != 0)
            continue;

        char* base = (char*) cfg;
        switch (map[i].type) {
        case TYPE_STRING: {
            char* dest = (char*) (base + (size_t) map[i].offset);
            char* parsed = parse_string(value);
            if (!parsed)
                return -1;
            strncpy(dest, parsed, CONFIG_MAX_STRING - 1);
            dest[CONFIG_MAX_STRING - 1] = '\0';
            return 0;
        }
        case TYPE_INT: {
            int* dest = (int*) (base + (size_t) map[i].offset);
            return parse_int(trim(value), dest);
        }
        case TYPE_DOUBLE: {
            double* dest = (double*) (base + (size_t) map[i].offset);
            return parse_double(trim(value), dest);
        }
        case TYPE_BOOL: {
            /* bools not used in current schema but parsed for completeness */
            (void) trim(value);
            return 0;
        }
        case TYPE_STRING_ARRAY: {
            config_fish* fish = (config_fish*) (base + (size_t) map[i].offset);
            char* p = trim(value);
            if (*p != '[')
                return -1;
            p++; /* skip [ */
            char* end = strchr(p, ']');
            if (!end)
                return -1;
            *end = '\0';

            fish->command_count = 0;
            while (*p && fish->command_count < CONFIG_MAX_COMMANDS) {
                p = trim(p);
                if (*p == ',') {
                    p++;
                    continue;
                }
                if (*p == '"') {
                    char* item = parse_string(p);
                    if (!item)
                        return -1;
                    strncpy(fish->commands[fish->command_count], item, CONFIG_MAX_COMMAND_LEN - 1);
                    fish->commands[fish->command_count][CONFIG_MAX_COMMAND_LEN - 1] = '\0';
                    fish->command_count++;
                    /* advance past the parsed string */
                    p = strchr(p, '"');
                    if (p)
                        p++;
                } else {
                    /* skip non-quoted token */
                    while (*p && *p != ',')
                        p++;
                }
            }
            return 0;
        }
        case TYPE_SCANNER_ARRAY: {
            config_scanner* sc = (config_scanner*) (base + (size_t) map[i].offset);
            char* p = trim(value);
            if (*p != '[')
                return -1;
            p++;
            char* end = strchr(p, ']');
            if (!end)
                return -1;
            *end = '\0';

            while (*p && sc->ignore_dir_count < CONFIG_MAX_IGNORE &&
                   sc->ignore_file_count < CONFIG_MAX_IGNORE) {
                p = trim(p);
                if (*p == ',') {
                    p++;
                    continue;
                }
                if (*p == '"') {
                    char* item = parse_string(p);
                    if (!item)
                        return -1;
                    if (map[i].key[7] == 'd') {
                        strncpy(sc->ignore_dirs[sc->ignore_dir_count], item,
                                CONFIG_MAX_IGNORE_LEN - 1);
                        sc->ignore_dirs[sc->ignore_dir_count][CONFIG_MAX_IGNORE_LEN - 1] = '\0';
                        sc->ignore_dir_count++;
                    } else {
                        strncpy(sc->ignore_files[sc->ignore_file_count], item,
                                CONFIG_MAX_IGNORE_LEN - 1);
                        sc->ignore_files[sc->ignore_file_count][CONFIG_MAX_IGNORE_LEN - 1] = '\0';
                        sc->ignore_file_count++;
                    }
                    p = strchr(p, '"');
                    if (p)
                        p++;
                } else {
                    while (*p && *p != ',')
                        p++;
                }
            }
            return 0;
        }
        case TYPE_DAEMON_PATHS_ARRAY: {
            config_daemon* dm = (config_daemon*) (base + (size_t) map[i].offset);
            char* p = trim(value);
            if (*p != '[')
                return -1;
            p++;
            char* end = strchr(p, ']');
            if (!end)
                return -1;
            *end = '\0';

            dm->scan_path_count = 0;
            while (*p && dm->scan_path_count < CONFIG_MAX_ROOTS) {
                p = trim(p);
                if (*p == ',') {
                    p++;
                    continue;
                }
                if (*p == '"') {
                    char* item = parse_string(p);
                    if (!item)
                        return -1;
                    strncpy(dm->scan_paths[dm->scan_path_count], item, CONFIG_MAX_STRING - 1);
                    dm->scan_paths[dm->scan_path_count][CONFIG_MAX_STRING - 1] = '\0';
                    dm->scan_path_count++;
                    p = strchr(p, '"');
                    if (p)
                        p++;
                } else {
                    while (*p && *p != ',')
                        p++;
                }
            }
            return 0;
        }
        }
    }
    return 0; /* unknown key: silently ignore */
}

static const field_map daemon_map[] = {
    {"scan_path", TYPE_STRING, FOFFSET(daemon, scan_path)},
    {"scan_paths", TYPE_DAEMON_PATHS_ARRAY, FOFFSET(daemon, scan_paths)},
    {"socket_path", TYPE_STRING, FOFFSET(daemon, socket_path)},
    {"scan_threads", TYPE_INT, FOFFSET(daemon, scan_threads)},
    {"max_depth", TYPE_INT, FOFFSET(daemon, max_depth)},
    {"rescan_interval_seconds", TYPE_INT, FOFFSET(daemon, rescan_interval_seconds)},
    {"log_level", TYPE_INT, FOFFSET(daemon, log_level)},
    {"colored_output", TYPE_BOOL, FOFFSET(daemon, colored_output)},
};

static const field_map storage_map[] = {
    {"max_buckets", TYPE_INT, FOFFSET(storage, max_buckets)},
    {"max_nodes_per_bucket", TYPE_INT, FOFFSET(storage, max_nodes_per_bucket)},
    {"cache_max_entries", TYPE_INT, FOFFSET(storage, cache_max_entries)},
    {"cache_ttl_seconds", TYPE_INT, FOFFSET(storage, cache_ttl_seconds)},
    {"recent_files_capacity", TYPE_INT, FOFFSET(storage, recent_files_capacity)},
    {"case_insensitive", TYPE_BOOL, FOFFSET(storage, case_insensitive)},
    {"max_total_nodes", TYPE_INT, FOFFSET(storage, max_total_nodes)},
    {"max_memory_mb", TYPE_INT, FOFFSET(storage, max_memory_mb)},
};

static const field_map scoring_map[] = {
    {"weight_frequency", TYPE_DOUBLE, FOFFSET(scoring, weight_frequency)},
    {"weight_recency", TYPE_DOUBLE, FOFFSET(scoring, weight_recency)},
    {"weight_depth", TYPE_DOUBLE, FOFFSET(scoring, weight_depth)},
    {"weight_type", TYPE_DOUBLE, FOFFSET(scoring, weight_type)},
    {"weight_cwd_proximity", TYPE_DOUBLE, FOFFSET(scoring, weight_cwd_proximity)},
    {"min_score_threshold", TYPE_DOUBLE, FOFFSET(scoring, min_score_threshold)},
    {"hidden_file_penalty", TYPE_DOUBLE, FOFFSET(scoring, hidden_file_penalty)},
};

static const field_map fish_map[] = {
    {"commands", TYPE_STRING_ARRAY, FOFFSET(fish, commands)},
};

static const field_map scanner_map[] = {
    {"ignore_dirs", TYPE_SCANNER_ARRAY, FOFFSET(scanner, ignore_dirs)},
    {"ignore_files", TYPE_SCANNER_ARRAY, FOFFSET(scanner, ignore_files)},
};

static const field_map bookmarks_map[] = {
    {"paths", TYPE_STRING_ARRAY, FOFFSET(bookmarks, paths)},
};

typedef struct {
    const char* name;
    const field_map* map;
    int map_len;
} section_map;

static const section_map sections[] = {
    {"daemon", daemon_map, (int) (sizeof(daemon_map) / sizeof(daemon_map[0]))},
    {"storage", storage_map, (int) (sizeof(storage_map) / sizeof(storage_map[0]))},
    {"scoring", scoring_map, (int) (sizeof(scoring_map) / sizeof(scoring_map[0]))},
    {"fish", fish_map, (int) (sizeof(fish_map) / sizeof(fish_map[0]))},
    {"scanner", scanner_map, (int) (sizeof(scanner_map) / sizeof(scanner_map[0]))},
    {"bookmarks", bookmarks_map, (int) (sizeof(bookmarks_map) / sizeof(bookmarks_map[0]))},
};

static const section_map* find_section(const char* name) {
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        if (strcmp(sections[i].name, name) == 0)
            return &sections[i];
    }
    return NULL;
}

/* ── Parser ──────────────────────────────────────────────────────────────── */

int config_load(archaic_config* cfg, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f)
        return -1;

    config_init_defaults(cfg);

    char line[8192];
    const section_map* current = NULL;

    while (fgets(line, sizeof(line), f)) {
        char* p = trim(line);

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '#')
            continue;

        /* Section header */
        if (*p == '[') {
            char* end = strchr(p, ']');
            if (!end) {
                fclose(f);
                return -1;
            }
            *end = '\0';
            char* name = trim(p + 1);
            current = find_section(name);
            continue;
        }

        /* Key = value */
        char* eq = strchr(p, '=');
        if (!eq) {
            fclose(f);
            return -1;
        }

        *eq = '\0';
        char* key = trim(p);
        char* value = trim(eq + 1);

        /* Strip inline comments (not inside strings/arrays) */
        if (*value == '"') {
            /* find closing quote */
            char* q = strchr(value + 1, '"');
            if (q) {
                q++;
                char* comment = strchr(q, '#');
                if (comment)
                    *comment = '\0';
            }
        } else if (*value == '[') {
            char* bracket = strchr(value, ']');
            if (bracket) {
                char* comment = strchr(bracket, '#');
                if (comment)
                    *comment = '\0';
            }
        } else {
            char* comment = strchr(value, '#');
            if (comment)
                *comment = '\0';
        }
        value = trim(value);

        if (!current) {
            fclose(f);
            return -1;
        }
        if (set_field(cfg, current->map, current->map_len, key, value) != 0) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

/* ── Default search paths ────────────────────────────────────────────────── */

int config_load_default(archaic_config* cfg) {
    int found = -1;
    const char* env = getenv("ARCHAIC_CONFIG");
    if (env && *env && config_load(cfg, env) == 0)
        found = 0;

    const char* home = getenv("HOME");
    if (found != 0 && home && *home) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/.config/archaic/config.toml", home);
        if (n > 0 && n < (int) sizeof(path) && config_load(cfg, path) == 0)
            found = 0;
    }

    if (found != 0 && config_load(cfg, "/etc/archaic/config.toml") == 0)
        found = 0;

    if (found != 0)
        config_init_defaults(cfg);

    config_load_roots_file(cfg);
    return found;
}

void config_expand_vars(archaic_config* cfg) {
    char buf[CONFIG_MAX_STRING];

    expand_env_vars(cfg->daemon.scan_path, buf, sizeof(buf));
    strncpy(cfg->daemon.scan_path, buf, sizeof(cfg->daemon.scan_path) - 1);
    cfg->daemon.scan_path[sizeof(cfg->daemon.scan_path) - 1] = '\0';

    for (int i = 0; i < cfg->daemon.scan_path_count; i++) {
        expand_env_vars(cfg->daemon.scan_paths[i], buf, sizeof(buf));
        strncpy(cfg->daemon.scan_paths[i], buf, sizeof(cfg->daemon.scan_paths[i]) - 1);
        cfg->daemon.scan_paths[i][sizeof(cfg->daemon.scan_paths[i]) - 1] = '\0';
    }

    expand_env_vars(cfg->daemon.socket_path, buf, sizeof(buf));
    strncpy(cfg->daemon.socket_path, buf, sizeof(cfg->daemon.socket_path) - 1);
    cfg->daemon.socket_path[sizeof(cfg->daemon.socket_path) - 1] = '\0';
}

int config_validate_paths(archaic_config* cfg) {
    int errors = 0;
    struct stat st;

    if (cfg->daemon.scan_path[0] != '\0') {
        if (stat(cfg->daemon.scan_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "archaic: scan_path not found or not a directory: %s\n",
                    cfg->daemon.scan_path);
            errors++;
        }
    }
    for (int i = 0; i < cfg->daemon.scan_path_count; i++) {
        if (stat(cfg->daemon.scan_paths[i], &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "archaic: scan_paths[%d] not found or not a directory: %s\n", i,
                    cfg->daemon.scan_paths[i]);
            errors++;
        }
    }

    if (cfg->daemon.scan_path[0] == '\0' && cfg->daemon.scan_path_count == 0) {
        fprintf(stderr, "archaic: no scan_path or scan_paths configured\n");
        errors++;
    }

    return errors;
}

/* ── .archaicignore support ──────────────────────────────────────────────── */

static void add_ignore_dir_if_new(archaic_config* cfg, const char* dir) {
    for (int i = 0; i < cfg->scanner.ignore_dir_count; i++) {
        if (strcmp(cfg->scanner.ignore_dirs[i], dir) == 0)
            return;
    }
    add_ignore_dir(cfg, dir);
}

static void add_ignore_file_if_new(archaic_config* cfg, const char* file) {
    for (int i = 0; i < cfg->scanner.ignore_file_count; i++) {
        if (strcmp(cfg->scanner.ignore_files[i], file) == 0)
            return;
    }
    add_ignore_file(cfg, file);
}

static int load_single_archaicignore(archaic_config* cfg, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;

    int loaded = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && isspace((unsigned char) *p))
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char) p[len - 1]))
            p[--len] = '\0';
        if (len == 0)
            continue;

        if (p[len - 1] == '/') {
            p[len - 1] = '\0';
            add_ignore_dir_if_new(cfg, p);
        } else {
            add_ignore_file_if_new(cfg, p);
        }
        loaded++;
    }

    fclose(f);
    return loaded;
}

int config_load_archaicignore(archaic_config* cfg, const char* start_path) {
    if (!cfg || !start_path)
        return 0;

    int total_loaded = 0;
    char path[8192];
    strncpy(path, start_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    for (int depth = 0; depth < 32; depth++) {
        char ignore_path[8192];
        snprintf(ignore_path, sizeof(ignore_path), "%s/.archaicignore", path);

        total_loaded += load_single_archaicignore(cfg, ignore_path);

        char* last_slash = strrchr(path, '/');
        if (!last_slash || last_slash == path)
            break;
        *last_slash = '\0';
    }

    return total_loaded;
}
