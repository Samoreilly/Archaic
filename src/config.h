#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_MAX_STRING 4096
#define CONFIG_MAX_COMMANDS 32
#define CONFIG_MAX_COMMAND_LEN 64
#define CONFIG_MAX_IGNORE 64
#define CONFIG_MAX_IGNORE_LEN 128

typedef struct {
    char scan_path[CONFIG_MAX_STRING];
    char socket_path[CONFIG_MAX_STRING];
    int scan_threads;
    int max_depth;
    int rescan_interval_seconds;
    int log_level;       /* 0=debug, 1=info, 2=warn, 3=error */
    bool colored_output; /* Enable ANSI colors in completions */
} config_daemon;

typedef struct {
    uint32_t max_buckets;
    uint32_t max_nodes_per_bucket;
    uint32_t cache_max_entries;
    uint32_t cache_ttl_seconds;
    bool colored_output;
    uint32_t recent_files_capacity;
    bool case_insensitive;
} config_storage;

typedef struct {
    double weight_frequency;
    double weight_recency;
    double weight_depth;
    double weight_type;
    double weight_cwd_proximity;
} config_scoring;

typedef struct {
    char commands[CONFIG_MAX_COMMANDS][CONFIG_MAX_COMMAND_LEN];
    int command_count;
} config_fish;

typedef struct {
    char ignore_dirs[CONFIG_MAX_IGNORE][CONFIG_MAX_IGNORE_LEN];
    int ignore_dir_count;
    char ignore_files[CONFIG_MAX_IGNORE][CONFIG_MAX_IGNORE_LEN];
    int ignore_file_count;
} config_scanner;

#define CONFIG_MAX_BOOKMARKS 32

typedef struct {
    char paths[CONFIG_MAX_BOOKMARKS][CONFIG_MAX_STRING];
    int count;
} config_bookmarks;

typedef struct {
    config_daemon daemon;
    config_storage storage;
    config_scoring scoring;
    config_fish fish;
    config_scanner scanner;
    config_bookmarks bookmarks;
} archaic_config;

/* Load config from file. Returns 0 on success, -1 on error.
   Missing keys use defaults. */
int config_load(archaic_config* cfg, const char* path);

/* Load config from search paths: $ARCHAIC_CONFIG, ~/.config/archaic/config.toml,
   /etc/archaic/config.toml. Returns 0 if any file found and parsed, -1 if none found. */
int config_load_default(archaic_config* cfg);

/* Initialize config with defaults (no file parsing). */
void config_init_defaults(archaic_config* cfg);

/* Load .archaicignore from a directory, merging patterns into the scanner config.
   Searches upward from start_path for .archaicignore until root or max_depth.
   Returns the number of patterns loaded. */
int config_load_archaicignore(archaic_config* cfg, const char* start_path);
