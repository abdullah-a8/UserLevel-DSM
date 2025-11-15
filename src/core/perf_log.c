/**
 * @file perf_log.c
 * @brief Performance logging implementation (Task 8.6)
 */

#include "perf_log.h"
#include "log.h"
#include "dsm_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Global performance log state */
static struct {
    FILE *log_file;
    pthread_mutex_t lock;
    bool initialized;
    char log_path[256];
} g_perf_log = {
    .log_file = NULL,
    .initialized = false
};

int perf_log_init(const char *log_file) {
    if (g_perf_log.initialized) {
        LOG_WARN("Performance logging already initialized");
        return DSM_SUCCESS;
    }

    pthread_mutex_init(&g_perf_log.lock, NULL);

    if (log_file) {
        g_perf_log.log_file = fopen(log_file, "w");
        if (!g_perf_log.log_file) {
            LOG_ERROR("Failed to open performance log file: %s", log_file);
            return DSM_ERROR_INIT;
        }

        strncpy(g_perf_log.log_path, log_file, sizeof(g_perf_log.log_path) - 1);

        /* Write CSV header */
        fprintf(g_perf_log.log_file,
                "timestamp_ns,event_type,page_id,access_type,latency_ns,was_queued\n");
        fflush(g_perf_log.log_file);

        LOG_INFO("Performance logging initialized: %s", log_file);
    }

    g_perf_log.initialized = true;
    return DSM_SUCCESS;
}

void perf_log_cleanup(void) {
    if (!g_perf_log.initialized) {
        return;
    }

    pthread_mutex_lock(&g_perf_log.lock);

    if (g_perf_log.log_file) {
        fclose(g_perf_log.log_file);
        g_perf_log.log_file = NULL;
    }

    g_perf_log.initialized = false;
    pthread_mutex_unlock(&g_perf_log.lock);

    pthread_mutex_destroy(&g_perf_log.lock);
}

void perf_log_fault(page_id_t page_id, access_type_t access_type,
                    uint64_t latency_ns, bool was_queued) {
    if (!g_perf_log.initialized) {
        return;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx) {
        return;
    }

    /* Update statistics */
    pthread_mutex_lock(&ctx->stats_lock);

    /* Track latency */
    ctx->stats.total_fault_latency_ns += latency_ns;
    if (latency_ns > ctx->stats.max_fault_latency_ns) {
        ctx->stats.max_fault_latency_ns = latency_ns;
    }
    if (ctx->stats.min_fault_latency_ns == 0 ||
        latency_ns < ctx->stats.min_fault_latency_ns) {
        ctx->stats.min_fault_latency_ns = latency_ns;
    }

    if (was_queued) {
        ctx->stats.queued_requests++;
    }

    pthread_mutex_unlock(&ctx->stats_lock);

    /* Log to CSV file */
    pthread_mutex_lock(&g_perf_log.lock);
    if (g_perf_log.log_file) {
        uint64_t timestamp = perf_get_timestamp_ns();
        fprintf(g_perf_log.log_file,
                "%lu,PAGE_FAULT,%lu,%s,%lu,%d\n",
                timestamp, page_id,
                access_type == ACCESS_READ ? "READ" : "WRITE",
                latency_ns, was_queued ? 1 : 0);
        fflush(g_perf_log.log_file);
    }
    pthread_mutex_unlock(&g_perf_log.lock);
}

void perf_log_false_sharing(page_id_t page_id) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.false_sharing_events++;
    pthread_mutex_unlock(&ctx->stats_lock);

    pthread_mutex_lock(&g_perf_log.lock);
    if (g_perf_log.log_file) {
        uint64_t timestamp = perf_get_timestamp_ns();
        fprintf(g_perf_log.log_file,
                "%lu,FALSE_SHARING,%lu,NA,0,0\n",
                timestamp, page_id);
        fflush(g_perf_log.log_file);
    }
    pthread_mutex_unlock(&g_perf_log.lock);

    LOG_DEBUG("False sharing detected on page %lu", page_id);
}

void perf_log_network_retry(void) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.network_retries++;
    pthread_mutex_unlock(&ctx->stats_lock);
}

void perf_log_network_failure(void) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.network_failures++;
    pthread_mutex_unlock(&ctx->stats_lock);
}

void perf_log_timeout(void) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.timeouts++;
    pthread_mutex_unlock(&ctx->stats_lock);
}

int perf_log_export_stats(void) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx) {
        return DSM_ERROR_INIT;
    }

    char stats_file[512];
    snprintf(stats_file, sizeof(stats_file), "dsm_stats_node%u.csv", ctx->node_id);

    FILE *f = fopen(stats_file, "w");
    if (!f) {
        LOG_ERROR("Failed to open stats file: %s", stats_file);
        return DSM_ERROR_INIT;
    }

    pthread_mutex_lock(&ctx->stats_lock);

    fprintf(f, "metric,value\n");
    fprintf(f, "node_id,%u\n", ctx->node_id);
    fprintf(f, "page_faults,%lu\n", ctx->stats.page_faults);
    fprintf(f, "read_faults,%lu\n", ctx->stats.read_faults);
    fprintf(f, "write_faults,%lu\n", ctx->stats.write_faults);
    fprintf(f, "pages_fetched,%lu\n", ctx->stats.pages_fetched);
    fprintf(f, "pages_sent,%lu\n", ctx->stats.pages_sent);
    fprintf(f, "invalidations_sent,%lu\n", ctx->stats.invalidations_sent);
    fprintf(f, "invalidations_received,%lu\n", ctx->stats.invalidations_received);
    fprintf(f, "network_bytes_sent,%lu\n", ctx->stats.network_bytes_sent);
    fprintf(f, "network_bytes_received,%lu\n", ctx->stats.network_bytes_received);

    /* Performance metrics */
    uint64_t avg_latency = 0;
    if (ctx->stats.page_faults > 0) {
        avg_latency = ctx->stats.total_fault_latency_ns / ctx->stats.page_faults;
    }
    fprintf(f, "avg_fault_latency_ns,%lu\n", avg_latency);
    fprintf(f, "avg_fault_latency_us,%lu\n", avg_latency / 1000);
    fprintf(f, "max_fault_latency_ns,%lu\n", ctx->stats.max_fault_latency_ns);
    fprintf(f, "max_fault_latency_us,%lu\n", ctx->stats.max_fault_latency_ns / 1000);
    fprintf(f, "min_fault_latency_ns,%lu\n", ctx->stats.min_fault_latency_ns);
    fprintf(f, "min_fault_latency_us,%lu\n", ctx->stats.min_fault_latency_ns / 1000);
    fprintf(f, "queued_requests,%lu\n", ctx->stats.queued_requests);
    fprintf(f, "false_sharing_events,%lu\n", ctx->stats.false_sharing_events);
    fprintf(f, "network_retries,%lu\n", ctx->stats.network_retries);
    fprintf(f, "network_failures,%lu\n", ctx->stats.network_failures);
    fprintf(f, "timeouts,%lu\n", ctx->stats.timeouts);

    pthread_mutex_unlock(&ctx->stats_lock);

    fclose(f);
    LOG_INFO("Statistics exported to: %s", stats_file);
    return DSM_SUCCESS;
}

void perf_log_print_summary(void) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->stats_lock);

    printf("\n");
    printf("========================================\n");
    printf("  DSM Performance Summary (Node %u)\n", ctx->node_id);
    printf("========================================\n");
    printf("Page Faults:         %lu\n", ctx->stats.page_faults);
    printf("  Read Faults:       %lu\n", ctx->stats.read_faults);
    printf("  Write Faults:      %lu\n", ctx->stats.write_faults);
    printf("Pages Fetched:       %lu\n", ctx->stats.pages_fetched);
    printf("Pages Sent:          %lu\n", ctx->stats.pages_sent);
    printf("Invalidations Sent:  %lu\n", ctx->stats.invalidations_sent);
    printf("Invalidations Rcvd:  %lu\n", ctx->stats.invalidations_received);

    printf("\nPerformance Metrics:\n");
    if (ctx->stats.page_faults > 0) {
        uint64_t avg = ctx->stats.total_fault_latency_ns / ctx->stats.page_faults;
        printf("  Avg Fault Latency: %lu us\n", avg / 1000);
        printf("  Max Fault Latency: %lu us\n", ctx->stats.max_fault_latency_ns / 1000);
        printf("  Min Fault Latency: %lu us\n", ctx->stats.min_fault_latency_ns / 1000);
    }
    printf("  Queued Requests:   %lu\n", ctx->stats.queued_requests);
    printf("  False Sharing:     %lu\n", ctx->stats.false_sharing_events);
    printf("  Network Retries:   %lu\n", ctx->stats.network_retries);
    printf("  Network Failures:  %lu\n", ctx->stats.network_failures);
    printf("  Timeouts:          %lu\n", ctx->stats.timeouts);
    printf("========================================\n\n");

    pthread_mutex_unlock(&ctx->stats_lock);
}
