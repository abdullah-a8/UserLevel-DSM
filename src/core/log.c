/**
 * @file log.c
 * @brief Implementation of thread-safe logging
 */

#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>

/* ============================ */
/*     Global State             */
/* ============================ */

/** Current log level */
log_level_t g_log_level = LOG_LEVEL_INFO;

/** Mutex for thread-safe logging */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/** ANSI color codes for terminal output */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_GRAY    "\033[90m"

/* ============================ */
/*     Helper Functions         */
/* ============================ */

/**
 * Get current thread ID (Linux-specific)
 */
static long get_thread_id(void) {
    return (long)syscall(SYS_gettid);
}

/**
 * Get current timestamp string
 */
static void get_timestamp(char *buffer, size_t size) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    snprintf(buffer, size, "%02d:%02d:%02d.%03ld",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             ts.tv_nsec / 1000000);
}

/**
 * Get log level string and color
 */
static const char* get_level_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN:  return "WARN ";
        case LOG_LEVEL_INFO:  return "INFO ";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        default:              return "?????";
    }
}

static const char* get_level_color(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return COLOR_RED;
        case LOG_LEVEL_WARN:  return COLOR_YELLOW;
        case LOG_LEVEL_INFO:  return COLOR_GREEN;
        case LOG_LEVEL_DEBUG: return COLOR_CYAN;
        default:              return COLOR_RESET;
    }
}

/* ============================ */
/*     Public Functions         */
/* ============================ */

void log_init(log_level_t level) {
    g_log_level = level;
}

void log_set_level(log_level_t level) {
    pthread_mutex_lock(&log_mutex);
    g_log_level = level;
    pthread_mutex_unlock(&log_mutex);
}

log_level_t log_get_level(void) {
    log_level_t level;
    pthread_mutex_lock(&log_mutex);
    level = g_log_level;
    pthread_mutex_unlock(&log_mutex);
    return level;
}

void log_message(log_level_t level, const char *file, int line,
                 const char *func, const char *fmt, ...) {

    /* Check if we should log this level */
    if (level > g_log_level) {
        return;
    }

    /* Lock for thread safety */
    pthread_mutex_lock(&log_mutex);

    /* Get timestamp */
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    /* Get thread ID */
    long tid = get_thread_id();

    /* Extract just the filename (not full path) */
    const char *filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    /* Check if output is to a terminal (for colors) */
    int use_color = isatty(fileno(stderr));

    /* Print log prefix */
    if (use_color) {
        fprintf(stderr, "%s[%s]%s %s[%s]%s [%s%ld%s] %s%s:%d%s - ",
                COLOR_GRAY, timestamp, COLOR_RESET,
                get_level_color(level), get_level_string(level), COLOR_RESET,
                COLOR_GRAY, tid, COLOR_RESET,
                COLOR_GRAY, filename, line, COLOR_RESET);
    } else {
        fprintf(stderr, "[%s] [%s] [%ld] %s:%d - ",
                timestamp, get_level_string(level), tid, filename, line);
    }

    /* Print the actual message */
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    /* Newline */
    fprintf(stderr, "\n");

    /* Flush to ensure immediate output */
    fflush(stderr);

    /* Unlock */
    pthread_mutex_unlock(&log_mutex);
}
