/**
 * @file perf_log.h
 * @brief Performance logging and metrics tracking (Task 8.6)
 *
 * This module provides performance logging capabilities including:
 * - CSV export of statistics
 * - Latency tracking
 * - False sharing detection
 */

#ifndef PERF_LOG_H
#define PERF_LOG_H

#include "dsm/types.h"
#include <stdint.h>
#include <time.h>

/**
 * Initialize performance logging
 *
 * @param log_file Path to CSV log file (NULL to disable)
 * @return DSM_SUCCESS on success, error code on failure
 */
int perf_log_init(const char *log_file);

/**
 * Cleanup performance logging
 */
void perf_log_cleanup(void);

/**
 * Record a page fault event
 *
 * @param page_id Page that faulted
 * @param access_type Read or write
 * @param latency_ns Latency in nanoseconds
 * @param was_queued True if request was queued
 */
void perf_log_fault(page_id_t page_id, access_type_t access_type,
                    uint64_t latency_ns, bool was_queued);

/**
 * Record a potential false sharing event
 *
 * @param page_id Page ID
 */
void perf_log_false_sharing(page_id_t page_id);

/**
 * Record a network retry
 */
void perf_log_network_retry(void);

/**
 * Record a network failure
 */
void perf_log_network_failure(void);

/**
 * Record a timeout
 */
void perf_log_timeout(void);

/**
 * Export statistics to CSV file
 *
 * @return DSM_SUCCESS on success, error code on failure
 */
int perf_log_export_stats(void);

/**
 * Print statistics summary to console
 */
void perf_log_print_summary(void);

/**
 * Get current timestamp in nanoseconds
 *
 * @return Timestamp in nanoseconds
 */
static inline uint64_t perf_get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#endif /* PERF_LOG_H */
