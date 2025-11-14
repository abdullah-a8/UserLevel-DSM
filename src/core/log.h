/**
 * @file log.h
 * @brief Thread-safe logging infrastructure for DSM
 *
 * Provides leveled logging with timestamps and thread information.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>

/* ============================ */
/*     Log Levels               */
/* ============================ */

typedef enum {
    LOG_LEVEL_NONE = 0,    /**< No logging */
    LOG_LEVEL_ERROR,       /**< Errors only */
    LOG_LEVEL_WARN,        /**< Warnings and errors */
    LOG_LEVEL_INFO,        /**< Informational messages */
    LOG_LEVEL_DEBUG        /**< Debug messages (verbose) */
} log_level_t;

/* ============================ */
/*     Global Log Level         */
/* ============================ */

extern log_level_t g_log_level;

/* ============================ */
/*     Function Declarations    */
/* ============================ */

/**
 * Initialize logging subsystem
 *
 * @param level Initial log level
 */
void log_init(log_level_t level);

/**
 * Set current log level
 *
 * @param level New log level
 */
void log_set_level(log_level_t level);

/**
 * Get current log level
 *
 * @return Current log level
 */
log_level_t log_get_level(void);

/**
 * Internal logging function (don't call directly, use macros)
 *
 * @param level Log level
 * @param file Source file name
 * @param line Line number
 * @param func Function name
 * @param fmt Printf-style format string
 * @param ... Variable arguments
 */
void log_message(log_level_t level, const char *file, int line,
                 const char *func, const char *fmt, ...);

/* ============================ */
/*     Logging Macros           */
/* ============================ */

/**
 * Log an error message
 * Always printed unless log level is NONE
 */
#define LOG_ERROR(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_ERROR) { \
            log_message(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, \
                       fmt, ##__VA_ARGS__); \
        } \
    } while(0)

/**
 * Log a warning message
 * Printed if log level is WARN or higher
 */
#define LOG_WARN(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_WARN) { \
            log_message(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, \
                       fmt, ##__VA_ARGS__); \
        } \
    } while(0)

/**
 * Log an informational message
 * Printed if log level is INFO or higher
 */
#define LOG_INFO(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_INFO) { \
            log_message(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, \
                       fmt, ##__VA_ARGS__); \
        } \
    } while(0)

/**
 * Log a debug message
 * Only printed if log level is DEBUG
 */
#define LOG_DEBUG(fmt, ...) \
    do { \
        if (g_log_level >= LOG_LEVEL_DEBUG) { \
            log_message(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, \
                       fmt, ##__VA_ARGS__); \
        } \
    } while(0)

#endif /* LOG_H */
