/**
 * @file dsm.c
 * @brief DSM API implementation
 */

#include "dsm/dsm.h"
#include "dsm_context.h"
#include "log.h"
#include "perf_log.h"
#include "../memory/fault_handler.h"
#include "../consistency/page_migration.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

int dsm_init(const dsm_config_t *config) {
    if (!config) {
        LOG_ERROR("NULL config provided");
        return DSM_ERROR_INVALID;
    }

    log_init(config->log_level);
    LOG_INFO("Initializing DSM (node_id=%u, port=%u)",
             config->node_id, config->port);

    int rc = dsm_context_init(config);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to initialize DSM context");
        return rc;
    }

    rc = install_fault_handler();
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to install fault handler");
        dsm_context_cleanup();
        return rc;
    }

    /* Note: consistency module will be initialized when dsm_malloc() creates page table */

    LOG_INFO("DSM initialized successfully");
    return DSM_SUCCESS;
}

int dsm_finalize(void) {
    LOG_INFO("Finalizing DSM");
    consistency_cleanup();
    uninstall_fault_handler();
    perf_log_cleanup();  /* Clean up performance logging resources */
    dsm_context_cleanup();
    LOG_INFO("DSM finalized");
    return DSM_SUCCESS;
}

node_id_t dsm_get_node_id(void) {
    dsm_context_t *ctx = dsm_get_context();
    return ctx->node_id;
}

int dsm_get_num_nodes(void) {
    dsm_context_t *ctx = dsm_get_context();
    return ctx->config.num_nodes;
}

int dsm_get_stats(dsm_stats_t *stats) {
    if (!stats) {
        return DSM_ERROR_INVALID;
    }

    dsm_context_t *ctx = dsm_get_context();
    pthread_mutex_lock(&ctx->stats_lock);
    *stats = ctx->stats;
    pthread_mutex_unlock(&ctx->stats_lock);

    return DSM_SUCCESS;
}

int dsm_reset_stats(void) {
    dsm_context_t *ctx = dsm_get_context();
    pthread_mutex_lock(&ctx->stats_lock);
    memset(&ctx->stats, 0, sizeof(dsm_stats_t));
    pthread_mutex_unlock(&ctx->stats_lock);
    return DSM_SUCCESS;
}

void dsm_print_stats(void) {
    dsm_stats_t stats;
    dsm_get_stats(&stats);

    printf("\n=== DSM Statistics ===\n");
    printf("Page Faults:       %lu (read: %lu, write: %lu)\n",
           stats.page_faults, stats.read_faults, stats.write_faults);
    printf("Pages Fetched:     %lu\n", stats.pages_fetched);
    printf("Pages Sent:        %lu\n", stats.pages_sent);
    printf("Invalidations:     %lu sent, %lu received\n",
           stats.invalidations_sent, stats.invalidations_received);
    printf("Network:           %lu bytes sent, %lu bytes received\n",
           stats.network_bytes_sent, stats.network_bytes_received);
    printf("Locks Acquired:    %lu\n", stats.lock_acquires);
    printf("Barrier Waits:     %lu\n", stats.barrier_waits);
    printf("======================\n\n");
}

void dsm_set_log_level(int level) {
    log_set_level((log_level_t)level);
}

/* Performance logging (Task 8.6) */
int dsm_perf_log_init(const char *log_file) {
    return perf_log_init(log_file);
}

int dsm_perf_export_stats(void) {
    return perf_log_export_stats();
}

void dsm_perf_print_summary(void) {
    perf_log_print_summary();
}
