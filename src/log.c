#include "log.h"
#include <pthread.h>

static log_level g_level = LOG_INFO;
static FILE* g_output = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char* level_str(log_level level) {
    switch (level) {
    case LOG_DEBUG:
        return "DEBUG";
    case LOG_INFO:
        return "INFO";
    case LOG_WARN:
        return "WARN";
    case LOG_ERROR:
        return "ERROR";
    }
    return "UNKNOWN";
}

void log_init(log_level level, FILE* output) {
    g_level = level;
    g_output = output ? output : stderr;
}

void log_set_level(log_level level) {
    g_level = level;
}

void log_shutdown(void) {
    if (g_output && g_output != stderr && g_output != stdout) {
        fclose(g_output);
    }
    g_output = NULL;
}

void log_msg(log_level level, const char* component, const char* fmt, ...) {
    if (level < g_level || !g_output)
        return;

    pthread_mutex_lock(&g_lock);

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(g_output, "[%s] [%-5s] [%s] ", ts, level_str(level), component);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_output, fmt, args);
    va_end(args);

    fprintf(g_output, "\n");
    fflush(g_output);

    pthread_mutex_unlock(&g_lock);
}
