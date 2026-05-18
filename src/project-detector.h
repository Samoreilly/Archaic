#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_PROJECT_ROOTS 100
#define MAX_PROJECT_PATH_LEN 512
#define MAX_PROJECT_TYPE_LEN 32

typedef struct project_root {
    char path[MAX_PROJECT_PATH_LEN];
    char type[MAX_PROJECT_TYPE_LEN];
    uint64_t last_access;
    bool is_git;
} project_root;

typedef struct project_detector {
    project_root roots[MAX_PROJECT_ROOTS];
    int count;
} project_detector;

/* Initialize project detector */
void project_detector_init(project_detector* pd);

/* Detect if a directory is a project root */
bool project_detector_scan_dir(project_detector* pd, const char* path);

/* Check if path is within a known project root */
const char* project_detector_find_root(project_detector* pd, const char* path);

/* Mark a project as recently accessed */
void project_detector_touch(project_detector* pd, const char* path);

/* Get list of detected projects */
int project_detector_get_projects(project_detector* pd, project_root** out);

/* Detect project type from directory contents */
const char* detect_project_type(const char* path);
