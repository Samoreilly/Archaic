#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

typedef enum { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 } log_level;

void log_init(log_level level, FILE* output);
void log_set_level(log_level level);
void log_shutdown(void);

void log_msg(log_level level, const char* component, const char* fmt, ...);

#define LOG_DBG(comp, ...) log_msg(LOG_DEBUG, comp, __VA_ARGS__)
#define LOG_INFO(comp, ...) log_msg(LOG_INFO, comp, __VA_ARGS__)
#define LOG_WARN(comp, ...) log_msg(LOG_WARN, comp, __VA_ARGS__)
#define LOG_ERR(comp, ...) log_msg(LOG_ERROR, comp, __VA_ARGS__)
