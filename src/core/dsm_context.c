/**
 * @file dsm_context.c
 * @brief Global DSM context implementation
 */

#include "dsm_context.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Global singleton instance */
static dsm_context_t g_dsm_context = {
    .initialized = false
};

dsm_context_t* dsm_get_context(void) {
    return &g_dsm_context;
}

int dsm_context_init(const dsm_config_t *config) {
    if (!config) {
        LOG_ERROR("Invalid config");
        return DSM_ERROR_INVALID;
    }

    dsm_context_t *ctx = &g_dsm_context;

    if (ctx->initialized) {
        LOG_WARN("DSM context already initialized");
        return DSM_SUCCESS;
    }

    /* Copy config */
    memcpy(&ctx->config, config, sizeof(dsm_config_t));
    ctx->node_id = config->node_id;

    /* Initialize mutexes */
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_mutex_init(&ctx->stats_lock, NULL);

    /* Initialize network state */
    ctx->network.server_sockfd = -1;
    ctx->network.server_port = config->port;
    ctx->network.num_nodes = 0;
    ctx->network.running = false;
    ctx->network.dispatcher_thread = 0;
    ctx->network.send_queue = msg_queue_create();

    for (int i = 0; i < MAX_NODES; i++) {
        ctx->network.nodes[i].connected = false;
        ctx->network.nodes[i].sockfd = -1;
    }

    /* Initialize lock manager */
    pthread_mutex_init(&ctx->lock_mgr.lock, NULL);
    ctx->lock_mgr.num_locks = 0;
    memset(ctx->lock_mgr.locks, 0, sizeof(ctx->lock_mgr.locks));

    /* Initialize barrier manager */
    pthread_mutex_init(&ctx->barrier_mgr.lock, NULL);
    ctx->barrier_mgr.max_barriers = 256;
    ctx->barrier_mgr.barriers = calloc(ctx->barrier_mgr.max_barriers,
                                       sizeof(dsm_barrier_t));

    /* Initialize statistics */
    memset(&ctx->stats, 0, sizeof(dsm_stats_t));

    /* Page table will be initialized when first DSM memory is allocated */
    ctx->page_table = NULL;

    ctx->initialized = true;

    LOG_INFO("DSM context initialized (node_id=%u, port=%u)",
             ctx->node_id, config->port);
    return DSM_SUCCESS;
}

void dsm_context_cleanup(void) {
    dsm_context_t *ctx = &g_dsm_context;

    if (!ctx->initialized) {
        return;
    }

    LOG_INFO("Cleaning up DSM context");

    /* Stop network */
    ctx->network.running = false;

    /* Cleanup network */
    if (ctx->network.send_queue) {
        msg_queue_destroy(ctx->network.send_queue);
    }

    /* Close all connections */
    for (int i = 0; i < MAX_NODES; i++) {
        if (ctx->network.nodes[i].sockfd >= 0) {
            close(ctx->network.nodes[i].sockfd);
        }
    }

    if (ctx->network.server_sockfd >= 0) {
        close(ctx->network.server_sockfd);
    }

    /* Cleanup locks */
    pthread_mutex_lock(&ctx->lock_mgr.lock);
    for (int i = 0; i < ctx->lock_mgr.num_locks; i++) {
        if (ctx->lock_mgr.locks[i]) {
            free(ctx->lock_mgr.locks[i]);
        }
    }
    pthread_mutex_unlock(&ctx->lock_mgr.lock);
    pthread_mutex_destroy(&ctx->lock_mgr.lock);

    /* Cleanup barriers */
    if (ctx->barrier_mgr.barriers) {
        free(ctx->barrier_mgr.barriers);
    }
    pthread_mutex_destroy(&ctx->barrier_mgr.lock);

    /* Cleanup page table */
    if (ctx->page_table) {
        page_table_destroy(ctx->page_table);
    }

    pthread_mutex_destroy(&ctx->lock);
    pthread_mutex_destroy(&ctx->stats_lock);

    ctx->initialized = false;
    LOG_INFO("DSM context cleanup complete");
}
