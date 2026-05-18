#include "project-detector.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void project_detector_init(project_detector* pd) {
    if (!pd)
        return;
    pd->count = 0;
    for (int i = 0; i < MAX_PROJECT_ROOTS; i++) {
        pd->roots[i].path[0] = '\0';
        pd->roots[i].type[0] = '\0';
        pd->roots[i].last_access = 0;
        pd->roots[i].is_git = false;
    }
}

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

const char* detect_project_type(const char* path) {
    char check_path[MAX_PROJECT_PATH_LEN];

    snprintf(check_path, sizeof(check_path), "%s/.git", path);
    if (file_exists(check_path))
        return "git";

    snprintf(check_path, sizeof(check_path), "%s/Cargo.toml", path);
    if (file_exists(check_path))
        return "rust";

    snprintf(check_path, sizeof(check_path), "%s/package.json", path);
    if (file_exists(check_path))
        return "node";

    snprintf(check_path, sizeof(check_path), "%s/setup.py", path);
    if (file_exists(check_path))
        return "python";

    snprintf(check_path, sizeof(check_path), "%s/pyproject.toml", path);
    if (file_exists(check_path))
        return "python";

    snprintf(check_path, sizeof(check_path), "%s/Makefile", path);
    if (file_exists(check_path))
        return "c";

    snprintf(check_path, sizeof(check_path), "%s/CMakeLists.txt", path);
    if (file_exists(check_path))
        return "cpp";

    snprintf(check_path, sizeof(check_path), "%s/go.mod", path);
    if (file_exists(check_path))
        return "go";

    return "unknown";
}

bool project_detector_scan_dir(project_detector* pd, const char* path) {
    if (!pd || !path || pd->count >= MAX_PROJECT_ROOTS)
        return false;

    char git_path[MAX_PROJECT_PATH_LEN];
    snprintf(git_path, sizeof(git_path), "%s/.git", path);

    bool is_git = file_exists(git_path);
    const char* proj_type = detect_project_type(path);

    if (is_git || strcmp(proj_type, "unknown") != 0) {
        int idx = pd->count;
        strncpy(pd->roots[idx].path, path, MAX_PROJECT_PATH_LEN - 1);
        pd->roots[idx].path[MAX_PROJECT_PATH_LEN - 1] = '\0';

        strncpy(pd->roots[idx].type, proj_type, MAX_PROJECT_TYPE_LEN - 1);
        pd->roots[idx].type[MAX_PROJECT_TYPE_LEN - 1] = '\0';

        pd->roots[idx].is_git = is_git;
        pd->roots[idx].last_access = 0;

        pd->count++;
        return true;
    }

    return false;
}

const char* project_detector_find_root(project_detector* pd, const char* path) {
    if (!pd || !path)
        return NULL;

    for (int i = 0; i < pd->count; i++) {
        size_t root_len = strlen(pd->roots[i].path);
        if (strncmp(path, pd->roots[i].path, root_len) == 0) {
            return pd->roots[i].path;
        }
    }

    return NULL;
}

void project_detector_touch(project_detector* pd, const char* path) {
    if (!pd || !path)
        return;

    for (int i = 0; i < pd->count; i++) {
        if (strcmp(pd->roots[i].path, path) == 0) {
            pd->roots[i].last_access++;
            return;
        }
    }
}

int project_detector_get_projects(project_detector* pd, project_root** out) {
    if (!pd || !out)
        return 0;
    *out = pd->roots;
    return pd->count;
}
