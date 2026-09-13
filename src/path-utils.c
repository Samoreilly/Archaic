#include "path-utils.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

size_t path_normalize(char* dst, const char* src, size_t dst_size) {
    if (!src || !dst || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return 0;
    }

    /* Hot path (every Tab): avoid 3 mallocs per call. Paths are capped at
     * 4096 by the IPC protocol, so fixed stacks are safe. Absurd inputs
     * fail closed instead of truncating. */
    size_t src_len = strlen(src);
    if (src_len >= 4096) {
        dst[0] = '\0';
        return 0;
    }
    size_t starts[4096];
    size_t lens[4096];

    int comp_count = 0;
    const char* p = src;
    int has_leading_slash = (*p == '/');

    if (*p == '/')
        p++;

    while (*p) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        const char* start = p;
        size_t len = 0;
        while (*p && *p != '/') {
            p++;
            len++;
        }

        if (len == 1 && start[0] == '.') {
            continue;
        }

        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (comp_count > 0) {
                comp_count--;
            }
            continue;
        }

        if (comp_count >= 4096)
            break;
        starts[comp_count] = (size_t) (start - src);
        lens[comp_count] = len;
        comp_count++;
    }

    size_t out_pos = 0;
    if (has_leading_slash && out_pos < dst_size - 1)
        dst[out_pos++] = '/';

    for (int i = 0; i < comp_count; i++) {
        if (i > 0 && out_pos < dst_size - 1)
            dst[out_pos++] = '/';
        for (size_t j = 0; j < lens[i] && out_pos < dst_size - 1; j++)
            dst[out_pos++] = src[starts[i] + j];
    }

    if (out_pos == 0 && out_pos < dst_size - 1) {
        dst[out_pos++] = has_leading_slash ? '/' : '.';
    }

    dst[out_pos] = '\0';

    return out_pos;
}

size_t path_expand_tilde(char* dst, const char* src, size_t dst_size) {
    if (!src || !dst || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return 0;
    }

    if (src[0] != '~') {
        size_t len = strlen(src);
        if (len >= dst_size)
            len = dst_size - 1;
        memcpy(dst, src, len);
        dst[len] = '\0';
        return len;
    }

    const char* rest = src + 1;
    char* home = getenv("HOME");
    if (!home)
        home = "/";

    if (*rest == '/' || *rest == '\0') {
        size_t home_len = strlen(home);
        size_t rest_len = strlen(rest);
        size_t total = home_len + rest_len;
        if (total >= dst_size)
            total = dst_size - 1;
        size_t copy_home = home_len < total ? home_len : total;
        memcpy(dst, home, copy_home);
        size_t copy_rest = total - copy_home;
        memcpy(dst + copy_home, rest, copy_rest);
        dst[copy_home + copy_rest] = '\0';
        return copy_home + copy_rest;
    }

    memcpy(dst, src, strlen(src) < dst_size ? strlen(src) : dst_size - 1);
    dst[strlen(src) < dst_size ? strlen(src) : dst_size - 1] = '\0';
    return strlen(dst);
}

size_t path_expand_dash(char* dst, const char* oldpwd, size_t dst_size) {
    if (!oldpwd || !dst || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return 0;
    }

    size_t len = strlen(oldpwd);
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, oldpwd, len);
    dst[len] = '\0';
    return len;
}

size_t path_expand_abbrev(char* dst, const char* src, size_t dst_size) {    if (!src || !dst || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return 0;
    }

    if (src[0] == '~') {
        return path_expand_tilde(dst, src, dst_size);
    }

    if (src[0] == '-' && (src[1] == '/' || src[1] == '\0')) {
        const char* oldpwd = getenv("OLDPWD");
        if (oldpwd && oldpwd[0]) {
            if (src[1] == '/') {
                size_t olen = strlen(oldpwd);
                size_t rest_len = strlen(src + 1);
                size_t total = olen + rest_len;
                if (total >= dst_size)
                    total = dst_size - 1;
                size_t copy_old = olen < total ? olen : total;
                memcpy(dst, oldpwd, copy_old);
                size_t copy_rest = total - copy_old;
                memcpy(dst + copy_old, src + 1, copy_rest);
                dst[copy_old + copy_rest] = '\0';
                return copy_old + copy_rest;
            }
            return path_expand_dash(dst, oldpwd, dst_size);
        }
    }

    size_t len = strlen(src);
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

int path_has_dotdot_component(const char* path) {
    if (!path || path[0] == '\0')
        return 0;
    const char* p = path;
    /* Leading ".." component. */
    if ((p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')))
        return 1;
    while (*p) {
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' &&
            (p[3] == '/' || p[3] == '\0'))
            return 1;
        p++;
    }
    return 0;
}