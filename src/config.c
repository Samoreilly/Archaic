#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void config_init_defaults(archaic_config* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->daemon.scan_threads = 4;
    cfg->daemon.max_depth = 10;
    cfg->daemon.rescan_interval_seconds = 300;
    cfg->daemon.log_level = 1;
    cfg->daemon.colored_output = true;
    strncpy(cfg->daemon.scan_path, "/home/sam/samdev", sizeof(cfg->daemon.scan_path) - 1);
    cfg->daemon.scan_path_count = 0;
    strncpy(cfg->daemon.socket_path, "/tmp/archaic-daemon.sock",
            sizeof(cfg->daemon.socket_path) - 1);

    cfg->storage.max_buckets = 65536;
    cfg->storage.max_nodes_per_bucket = 100000;
    cfg->storage.cache_max_entries = 1024;
    cfg->storage.cache_ttl_seconds = 2;
    cfg->storage.recent_files_capacity = 50;
    cfg->storage.case_insensitive = false;

    cfg->scoring.weight_frequency = 0.40;
    cfg->scoring.weight_recency = 0.30;
    cfg->scoring.weight_depth = 0.15;
    cfg->scoring.weight_type = 0.10;
    cfg->scoring.weight_cwd_proximity = 0.05;

    cfg->fish.command_count = 0;

    cfg->scanner.ignore_dir_count = 0;
    cfg->scanner.ignore_file_count = 0;
    set_default_ignores(cfg);

    cfg->bookmarks.count = 0;
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
};

static const field_map scoring_map[] = {
    {"weight_frequency", TYPE_DOUBLE, FOFFSET(scoring, weight_frequency)},
    {"weight_recency", TYPE_DOUBLE, FOFFSET(scoring, weight_recency)},
    {"weight_depth", TYPE_DOUBLE, FOFFSET(scoring, weight_depth)},
    {"weight_type", TYPE_DOUBLE, FOFFSET(scoring, weight_type)},
    {"weight_cwd_proximity", TYPE_DOUBLE, FOFFSET(scoring, weight_cwd_proximity)},
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
    /* 1. $ARCHAIC_CONFIG */
    const char* env = getenv("ARCHAIC_CONFIG");
    if (env && *env) {
        if (config_load(cfg, env) == 0)
            return 0;
    }

    /* 2. ~/.config/archaic/config.toml */
    const char* home = getenv("HOME");
    if (home && *home) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/.config/archaic/config.toml", home);
        if (n > 0 && n < (int) sizeof(path)) {
            if (config_load(cfg, path) == 0)
                return 0;
        }
    }

    /* 3. /etc/archaic/config.toml */
    if (config_load(cfg, "/etc/archaic/config.toml") == 0)
        return 0;

    /* None found — use defaults */
    config_init_defaults(cfg);
    return -1;
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
