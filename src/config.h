#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_MAX_STRING 4096
#define CONFIG_MAX_COMMANDS 32
#define CONFIG_MAX_COMMAND_LEN 64

typedef struct {
    char scan_path[CONFIG_MAX_STRING];
    char socket_path[CONFIG_MAX_STRING];
    int scan_threads;
    int max_depth;
    int rescan_interval_seconds;
    int log_level; /* 0=debug, 1=info, 2=warn, 3=error */
} config_daemon;

typedef struct {
    int max_buckets;
    int max_nodes_per_bucket;
    int cache_max_entries;
    int cache_ttl_seconds;
} config_storage;

typedef struct {
    double weight_frequency;
    double weight_recency;
    double weight_depth;
    double weight_type;
} config_scoring;

typedef struct {
    char commands[CONFIG_MAX_COMMANDS][CONFIG_MAX_COMMAND_LEN];
    int command_count;
} config_fish;

typedef struct {
    config_daemon daemon;
    config_storage storage;
    config_scoring scoring;
    config_fish fish;
} archaic_config;

/* Load config from file. Returns 0 on success, -1 on error.
   Missing keys use defaults. */
int config_load(archaic_config* cfg, const char* path);

/* Load config from search paths: $ARCHAIC_CONFIG, ~/.config/archaic/config.toml,
   /etc/archaic/config.toml. Returns 0 if any file found and parsed, -1 if none found. */
int config_load_default(archaic_config* cfg);

/* Initialize config with defaults (no file parsing). */
void config_init_defaults(archaic_config* cfg);
