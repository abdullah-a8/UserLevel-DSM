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
#include "../consistency/directory.h"
#include "../network/network.h"
#include "../network/handlers.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

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

    /* Initialize network layer */
    if (config->num_nodes > 1) {
        if (config->is_manager) {
            /* Manager: Start server and wait for workers */
            LOG_INFO("Starting network server on port %u", config->port);
            rc = network_server_init(config->port);
            if (rc != DSM_SUCCESS) {
                LOG_ERROR("Failed to initialize network server");
                uninstall_fault_handler();
                dsm_context_cleanup();
                return rc;
            }

            /* Start message dispatcher */
            rc = network_start_dispatcher();
            if (rc != DSM_SUCCESS) {
                LOG_ERROR("Failed to start network dispatcher");
                network_shutdown();
                uninstall_fault_handler();
                dsm_context_cleanup();
                return rc;
            }

            /* CRITICAL FIX #2: Start heartbeat thread for failure detection */
            start_heartbeat_thread();

            /* Wait for all workers to connect */
            LOG_INFO("Waiting for %d workers to connect...", config->num_nodes - 1);
            dsm_context_t *ctx = dsm_get_context();
            int expected_workers = config->num_nodes - 1;
            int timeout_seconds = 60;

            for (int i = 0; i < timeout_seconds * 10; i++) {
                pthread_mutex_lock(&ctx->lock);
                int connected = ctx->network.num_nodes;
                pthread_mutex_unlock(&ctx->lock);

                if (connected >= expected_workers) {
                    LOG_INFO("All %d workers connected", expected_workers);
                    break;
                }

                usleep(100000);  /* 100ms */

                if (i == timeout_seconds * 10 - 1) {
                    LOG_ERROR("Timeout waiting for workers (got %d, expected %d)",
                             connected, expected_workers);
                    network_shutdown();
                    uninstall_fault_handler();
                    dsm_context_cleanup();
                    return DSM_ERROR_TIMEOUT;
                }
            }

            /* Give workers a moment to complete their setup */
            sleep(1);

        } else {
            /* Worker: Connect to manager */
            LOG_INFO("Connecting to manager at %s:%u",
                    config->manager_host, config->manager_port);

            rc = network_connect_to_node(0, config->manager_host, config->manager_port);
            if (rc != DSM_SUCCESS) {
                LOG_ERROR("Failed to connect to manager");
                uninstall_fault_handler();
                dsm_context_cleanup();
                return rc;
            }

            /* Start message dispatcher */
            rc = network_start_dispatcher();
            if (rc != DSM_SUCCESS) {
                LOG_ERROR("Failed to start network dispatcher");
                network_shutdown();
                uninstall_fault_handler();
                dsm_context_cleanup();
                return rc;
            }

            /* CRITICAL FIX #2: Start heartbeat thread for failure detection */
            start_heartbeat_thread();

            /* Send NODE_JOIN to identify ourselves to the manager */
            LOG_INFO("Sending NODE_JOIN to manager (node_id=%u)", config->node_id);
            extern int send_node_join(node_id_t node_id, const char *hostname, uint16_t port);
            rc = send_node_join(config->node_id, "worker", config->port);
            if (rc != DSM_SUCCESS) {
                LOG_WARN("Failed to send NODE_JOIN (rc=%d), but continuing", rc);
            }

            /* Give manager time to process NODE_JOIN */
            usleep(100000);  /* 100ms */

            LOG_INFO("Connected to manager successfully");
        }
    }

    /* PHASE 9: Initialize backup state for Node 1 (primary backup) */
    if (config->node_id == 1) {
        LOG_INFO("Initializing Node 1 as primary backup for hot failover");

        dsm_context_t *ctx = dsm_get_context();
        pthread_mutex_lock(&ctx->lock);

        /* Set backup flags */
        ctx->network.backup_state.is_backup = true;
        ctx->network.backup_state.is_primary_backup = true;
        ctx->network.backup_state.is_promoted = false;
        ctx->network.backup_state.current_manager = 0;  /* Node 0 is initial manager */
        ctx->network.backup_state.last_sync_seq = 0;

        /* Create shadow directory structure for replication (100K buckets as per plan) */
        ctx->network.backup_state.backup_directory = directory_create(100000);
        if (!ctx->network.backup_state.backup_directory) {
            LOG_ERROR("Failed to create backup directory");
            pthread_mutex_unlock(&ctx->lock);
            network_shutdown();
            uninstall_fault_handler();
            dsm_context_cleanup();
            return DSM_ERROR_MEMORY;
        }

        /* Initialize shadow lock structures (256 locks max) */
        for (int i = 0; i < 256; i++) {
            ctx->network.backup_state.backup_locks[i] = NULL;  /* Lazy allocation */
        }

        /* Initialize shadow barrier structures (256 barriers max) */
        for (int i = 0; i < 256; i++) {
            ctx->network.backup_state.backup_barriers[i] = NULL;  /* Lazy allocation */
        }

        /* Initialize promotion lock to prevent split-brain */
        pthread_mutex_init(&ctx->network.backup_state.promotion_lock, NULL);

        pthread_mutex_unlock(&ctx->lock);

        /* Prepare backup server socket (bind but don't listen yet) */
        /* IMPORTANT: Bind to manager's port, not this node's port! */
        uint16_t manager_port = config->manager_port;  /* Port manager listens on */
        rc = network_prepare_backup_server(manager_port);
        if (rc != DSM_SUCCESS) {
            LOG_WARN("Failed to prepare backup server socket on port %u (rc=%d), continuing anyway", manager_port, rc);
            /* Not fatal - can still function as backup without pre-bound socket */
        }

        LOG_INFO("Node 1 initialized as primary backup (shadow directory created, promotion lock initialized)");
    }

    /* Note: consistency module will be initialized when dsm_malloc() creates page table */

    LOG_INFO("DSM initialized successfully");
    return DSM_SUCCESS;
}

int dsm_finalize(void) {
    LOG_INFO("Finalizing DSM");

    dsm_context_t *ctx = dsm_get_context();

    /* PHASE 9: Cleanup backup state if this is Node 1 */
    if (ctx->config.node_id == 1 && ctx->network.backup_state.is_backup) {
        LOG_INFO("Cleaning up backup state for Node 1");

        /* Destroy shadow directory */
        if (ctx->network.backup_state.backup_directory) {
            directory_destroy((page_directory_t*)ctx->network.backup_state.backup_directory);
            ctx->network.backup_state.backup_directory = NULL;
        }

        /* Destroy shadow locks (if any were allocated) */
        for (int i = 0; i < 256; i++) {
            if (ctx->network.backup_state.backup_locks[i]) {
                free(ctx->network.backup_state.backup_locks[i]);
                ctx->network.backup_state.backup_locks[i] = NULL;
            }
        }

        /* Destroy shadow barriers (if any were allocated) */
        for (int i = 0; i < 256; i++) {
            if (ctx->network.backup_state.backup_barriers[i]) {
                free(ctx->network.backup_state.backup_barriers[i]);
                ctx->network.backup_state.backup_barriers[i] = NULL;
            }
        }

        /* Destroy promotion lock */
        pthread_mutex_destroy(&ctx->network.backup_state.promotion_lock);

        LOG_INFO("Backup state cleanup complete");
    }

    /* Shutdown network first */
    if (ctx->config.num_nodes > 1) {
        network_shutdown();
    }

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
