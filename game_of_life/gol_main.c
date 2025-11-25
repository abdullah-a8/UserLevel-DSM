/**
 * @file gol_main.c
 * @brief Conway's Game of Life - DSM Implementation
 * Following test_multinode.c patterns for DSM usage
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsm/dsm.h"
#include "gol_config.h"
#include "gol_state.h"
#include "gol_rules.h"

/**
 * Print usage information
 */
void print_usage(const char *prog) {
    printf("Conway's Game of Life - DSM Implementation\n\n");
    printf("Usage:\n");
    printf("  Manager: %s --manager --nodes <N> [options]\n", prog);
    printf("  Worker:  %s --worker --node-id <ID> --manager-host <HOST> [options]\n\n", prog);
    printf("Required Arguments:\n");
    printf("  --manager              Run as manager (Node 0)\n");
    printf("  --worker               Run as worker node\n");
    printf("  --node-id <ID>         Node ID (required for worker)\n");
    printf("  --nodes <N>            Total number of nodes (2-4)\n");
    printf("  --manager-host <HOST>  Manager hostname (required for worker)\n\n");
    printf("Optional Arguments:\n");
    printf("  --grid <WxH>           Grid size (default: 100x100)\n");
    printf("  --generations <N>      Number of generations (default: 100)\n");
    printf("  --pattern <TYPE>       Initial pattern: glider, random, rpentomino (default: random)\n");
    printf("  --density <F>          Random pattern density 0.0-1.0 (default: 0.3)\n");
    printf("  --display-interval <N> Display every N generations (default: 10, 0=none)\n");
    printf("  --port <P>             Base port number (default: 5000)\n");
    printf("  --log-level <L>        Log level 0-4 (default: 3=INFO)\n\n");
    printf("Examples:\n");
    printf("  # 2-node setup with 100x100 grid\n");
    printf("  Node 0: %s --manager --nodes 2\n", prog);
    printf("  Node 1: %s --worker --node-id 1 --manager-host 192.168.1.100 --nodes 2\n\n", prog);
}

/**
 * Parse command-line arguments
 * Following test_multinode.c argument parsing pattern
 */
int parse_arguments(int argc, char *argv[], gol_config_t *config) {
    gol_config_init(config);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            return -1;  /* Signal to print usage */
        } else if (strcmp(argv[i], "--manager") == 0) {
            config->is_manager = 1;
            config->node_id = 0;
        } else if (strcmp(argv[i], "--worker") == 0) {
            config->is_manager = 0;
        } else if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
            config->node_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--nodes") == 0 && i + 1 < argc) {
            config->num_nodes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--manager-host") == 0 && i + 1 < argc) {
            strncpy(config->manager_host, argv[++i], sizeof(config->manager_host) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            config->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--grid") == 0 && i + 1 < argc) {
            /* Parse WxH format */
            if (sscanf(argv[++i], "%dx%d", &config->grid_width, &config->grid_height) != 2) {
                fprintf(stderr, "Invalid grid format. Use WxH (e.g., 100x100)\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--generations") == 0 && i + 1 < argc) {
            config->num_generations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "glider") == 0) {
                config->pattern = PATTERN_GLIDER;
            } else if (strcmp(argv[i], "random") == 0) {
                config->pattern = PATTERN_RANDOM;
            } else if (strcmp(argv[i], "rpentomino") == 0) {
                config->pattern = PATTERN_RPENTOMINO;
            } else if (strcmp(argv[i], "blinker") == 0) {
                config->pattern = PATTERN_BLINKER;
            } else {
                fprintf(stderr, "Unknown pattern: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--density") == 0 && i + 1 < argc) {
            config->random_density = atof(argv[++i]);
        } else if (strcmp(argv[i], "--display-interval") == 0 && i + 1 < argc) {
            config->display_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            config->log_level = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    /* Validate required arguments */
    if (config->node_id < 0) {
        fprintf(stderr, "Error: Node ID not specified\n");
        return -1;
    }

    if (config->num_nodes < 2 || config->num_nodes > 4) {
        fprintf(stderr, "Error: num_nodes must be 2-4 (got %d)\n", config->num_nodes);
        return -1;
    }

    if (!config->is_manager && config->manager_host[0] == '\0') {
        fprintf(stderr, "Error: Worker nodes must specify --manager-host\n");
        return -1;
    }

    return 0;
}

/**
 * Initialize DSM subsystem
 * Following test_multinode.c DSM initialization pattern
 */
int initialize_dsm(const gol_config_t *config) {
    dsm_config_t dsm_cfg = {
        .node_id = config->node_id,
        .port = config->port + config->node_id,  /* Unique port per node */
        .num_nodes = config->num_nodes,
        .is_manager = config->is_manager,
        .log_level = config->log_level
    };

    /* Set manager connection info for workers */
    if (!config->is_manager) {
        strncpy(dsm_cfg.manager_host, config->manager_host, sizeof(dsm_cfg.manager_host) - 1);
        dsm_cfg.manager_port = config->port;  /* Manager uses base port */
    }

    printf("[Node %d] Initializing DSM (port=%u, is_manager=%d, num_nodes=%d)...\n",
           config->node_id, dsm_cfg.port, config->is_manager, config->num_nodes);

    int rc = dsm_init(&dsm_cfg);
    if (rc != DSM_SUCCESS) {
        fprintf(stderr, "[Node %d] Failed to initialize DSM (rc=%d)\n", config->node_id, rc);
        return rc;
    }

    printf("[Node %d] DSM initialized successfully\n", config->node_id);
    return DSM_SUCCESS;
}

/**
 * Main execution
 */
int main(int argc, char *argv[]) {
    gol_config_t config;
    gol_state_t state;
    int rc;

    /* Parse arguments */
    if (parse_arguments(argc, argv, &config) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    printf("\n=== Conway's Game of Life - DSM Demo (Node %d) ===\n\n", config.node_id);
    printf("Configuration:\n");
    printf("  Grid: %dx%d\n", config.grid_width, config.grid_height);
    printf("  Generations: %d\n", config.num_generations);
    printf("  Pattern: %d\n", config.pattern);
    printf("  Nodes: %d (this node: %d, %s)\n",
           config.num_nodes, config.node_id, config.is_manager ? "manager" : "worker");
    printf("\n");

    /* Initialize DSM - following test_multinode.c pattern */
    rc = initialize_dsm(&config);
    if (rc != DSM_SUCCESS) {
        fprintf(stderr, "[Node %d] DSM initialization failed\n", config.node_id);
        return 1;
    }

    /* Initialize state */
    rc = gol_state_init(&state, &config);
    if (rc != DSM_SUCCESS) {
        fprintf(stderr, "[Node %d] State initialization failed\n", config.node_id);
        dsm_finalize();
        return 1;
    }

    /* PHASE 2: Per-Node Partition Allocation */
    /* CRITICAL: Sequential allocation to ensure deterministic allocation indices
     * Node 0 allocates first (gets indices 0, 1)
     * Node 1 allocates second (gets indices 2, 3)
     * This ensures dsm_get_allocation(node*2) returns correct partition
     */
    for (int i = 0; i < config.num_nodes; i++) {
        if (config.node_id == i) {
            printf("[Node %d] Allocating my partition...\n", config.node_id);
            rc = gol_allocate_partition(&state);
            if (rc != DSM_SUCCESS) {
                fprintf(stderr, "[Node %d] Failed to allocate partition\n", config.node_id);
                dsm_finalize();
                return 1;
            }
        }

        /* Barrier after each node's allocation */
        printf("[Node %d] Waiting at BARRIER_ALLOC_%d...\n", config.node_id, i);
        dsm_barrier(BARRIER_ALLOC + i, config.num_nodes);
        printf("[Node %d] Passed BARRIER_ALLOC_%d\n", config.node_id, i);
    }

    /* ALL nodes retrieve other nodes' partitions via SVAS */
    printf("[Node %d] Retrieving other partitions...\n", config.node_id);
    rc = gol_retrieve_partitions(&state);
    if (rc != DSM_SUCCESS) {
        fprintf(stderr, "[Node %d] Failed to retrieve partitions\n", config.node_id);
        dsm_finalize();
        return 1;
    }

    /* Verify partition addresses (SVAS verification) */
    printf("[Node %d] Partition addresses:\n", config.node_id);
    for (int i = 0; i < config.num_nodes; i++) {
        printf("[Node %d]   Partition %d: current=%p, next=%p\n",
               config.node_id, i,
               (void*)state.partitions_current[i],
               (void*)state.partitions_next[i]);
    }

    /* CRITICAL: Initialization barrier - following test_multinode.c pattern */
    printf("[Node %d] Waiting at BARRIER_INIT...\n", config.node_id);
    dsm_barrier(BARRIER_INIT, config.num_nodes);
    printf("[Node %d] Passed BARRIER_INIT\n", config.node_id);

    /* PHASE 3: Pattern Initialization */
    /* Note: All nodes participate in random pattern, only Node 0 for structured patterns */
    initialize_pattern(&state);

    /* Barrier after pattern initialization - ensure all nodes see initialized grid */
    printf("[Node %d] Waiting at pattern init barrier...\n", config.node_id);
    dsm_barrier(BARRIER_COMPUTE_BASE - 1, config.num_nodes);
    printf("[Node %d] Pattern initialized, starting computation\n", config.node_id);

    /* Get initial DSM statistics */
    dsm_stats_t stats_start;
    dsm_get_stats(&stats_start);

    /* PHASE 4: Main Generation Loop */
    printf("[Node %d] Starting %d generations...\n", config.node_id, config.num_generations);

    for (int gen = 0; gen < config.num_generations; gen++) {
        /* Step 1: Compute next generation for this partition */
        /* CRITICAL: This will trigger page faults at boundary rows */
        uint64_t live_cells = compute_generation(&state);

        /* Step 2: Barrier - all nodes must finish computing before swap */
        dsm_barrier(BARRIER_COMPUTE_BASE + gen, config.num_nodes);

        /* Step 3: Status output (Node 0 only, periodic) */
        if (config.node_id == 0 && (gen % 10 == 0 || gen == config.num_generations - 1)) {
            printf("[Node %d] Generation %d: %lu live cells in partition\n",
                   config.node_id, gen, live_cells);
        }

        /* Step 4: Swap partition buffers (pointer swap, no memory copy) */
        for (int i = 0; i < config.num_nodes; i++) {
            cell_t *temp = state.partitions_current[i];
            state.partitions_current[i] = state.partitions_next[i];
            state.partitions_next[i] = temp;
        }

        /* Step 5: Barrier after swap - ensure all nodes swapped before next iteration */
        dsm_barrier(BARRIER_SWAP_BASE + gen, config.num_nodes);
    }

    /* Get final DSM statistics */
    dsm_stats_t stats_end;
    dsm_get_stats(&stats_end);

    /* Print statistics for this node */
    printf("\n[Node %d] === Computation Complete ===\n", config.node_id);
    printf("[Node %d] Generations: %d\n", config.node_id, config.num_generations);
    printf("[Node %d] Partition: rows [%d, %d)\n",
           config.node_id, state.start_row, state.end_row);
    printf("[Node %d] Final live cells: %lu\n",
           config.node_id, count_live_cells(NULL, &state));
    printf("[Node %d] Page faults: %lu (R: %lu, W: %lu)\n",
           config.node_id,
           stats_end.page_faults - stats_start.page_faults,
           stats_end.read_faults - stats_start.read_faults,
           stats_end.write_faults - stats_start.write_faults);
    printf("[Node %d] Network: %.2f KB sent, %.2f KB received\n",
           config.node_id,
           (stats_end.network_bytes_sent - stats_start.network_bytes_sent) / 1024.0,
           (stats_end.network_bytes_received - stats_start.network_bytes_received) / 1024.0);
    printf("\n");

    /* CRITICAL: Final barrier before cleanup - following test_multinode.c pattern (line 100, 172, 246) */
    printf("[Node %d] Waiting at BARRIER_FINAL...\n", config.node_id);
    dsm_barrier(BARRIER_FINAL, config.num_nodes);
    printf("[Node %d] Passed BARRIER_FINAL\n", config.node_id);

    /* Cleanup */
    gol_state_cleanup(&state);

    /* CRITICAL: Cleanup barrier - following test_multinode.c pattern */
    printf("[Node %d] Waiting at BARRIER_CLEANUP...\n", config.node_id);
    dsm_barrier(BARRIER_CLEANUP, config.num_nodes);
    printf("[Node %d] Passed BARRIER_CLEANUP\n", config.node_id);

    /* Finalize DSM */
    printf("[Node %d] Finalizing DSM...\n", config.node_id);
    dsm_finalize();

    printf("\n[Node %d] Game of Life completed successfully\n", config.node_id);
    return 0;
}
