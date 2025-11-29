/**
 * @file matrix_main.c
 * @brief Simple driver for distributed matrix multiplication
 */

#include "matrix_mult.h"
#include "../src/core/log.h"
#include "../tests/test_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

void print_usage(const char *prog) {
    printf("\nUsage:\n");
    printf("  Manager: %s --manager --nodes <N> --size <M> [--port <P>]\n", prog);
    printf("  Worker:  %s --worker --node-id <ID> --manager-host <HOST> --size <M>\n", prog);
    printf("\nOptions:\n");
    printf("  --size <M>  : Matrix dimension (default: 100)\n");
    printf("  --verify    : Enable verification\n");
    printf("  --sample    : Print sample results\n\n");
}

int main(int argc, char *argv[]) {
    /* Disable output buffering for real-time logging */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    int is_manager = 0;
    int node_id = -1;
    int num_nodes = 2;
    char manager_host[256] = "localhost";
    int port = 5000;
    int N = 100;
    int enable_verify = 0;
    int enable_sample = 0;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--manager") == 0) {
            is_manager = 1;
            node_id = 0;
        } else if (strcmp(argv[i], "--worker") == 0) {
            is_manager = 0;
        } else if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
            node_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--nodes") == 0 && i + 1 < argc) {
            num_nodes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--manager-host") == 0 && i + 1 < argc) {
            strncpy(manager_host, argv[++i], sizeof(manager_host) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--manager-port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            N = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verify") == 0) {
            enable_verify = 1;
        } else if (strcmp(argv[i], "--sample") == 0) {
            enable_sample = 1;
        }
    }
    
    if (node_id < 0) {
        print_usage(argv[0]);
        return 1;
    }
    
    if (N <= 0 || N > MAX_MATRIX_SIZE) {
        printf("Error: Matrix size must be between 1 and %d\n", MAX_MATRIX_SIZE);
        return 1;
    }
    
    display_banner("DISTRIBUTED MATRIX MULTIPLICATION", "DSM-Based Parallel Computation");
    display_node_info(node_id, is_manager, num_nodes, is_manager ? NULL : manager_host);
    
    /* Initialize DSM */
    dsm_config_t config = {
        .node_id = node_id,
        .port = port + node_id,
        .num_nodes = num_nodes,
        .is_manager = is_manager,
        .log_level = LOG_LEVEL_ERROR
    };
    
    if (!is_manager) {
        strncpy(config.manager_host, manager_host, sizeof(config.manager_host) - 1);
        config.manager_port = port;
    }
    
    display_action(node_id, ICON_GEAR, "Initializing DSM subsystem...");
    if (dsm_init(&config) != DSM_SUCCESS) {
        display_action(node_id, ICON_CROSS, "Failed to initialize DSM");
        return 1;
    }
    display_action(node_id, ICON_CHECK, "DSM initialized successfully");
    dsm_perf_log_init(NULL);
    
    printf("\n");
    display_divider();
    printf("\n");
    
    /* Initialize matrix state */
    matrix_state_t state;
    if (matrix_state_init(&state, N, node_id, num_nodes) != 0) {
        display_action(node_id, ICON_CROSS, "Failed to initialize matrix state");
        dsm_finalize();
        return 1;
    }
    
    matrix_calculate_partitions(&state);
    
    char info_msg[128];
    snprintf(info_msg, sizeof(info_msg), "Matrix size: %dx%d, Nodes: %d", N, N, num_nodes);
    display_action(node_id, ICON_INFO, info_msg);
    
    snprintf(info_msg, sizeof(info_msg), "This node computes rows %d-%d (%d rows)",
             state.start_row[node_id], state.end_row[node_id] - 1,
             state.rows_per_node[node_id]);
    display_action(node_id, ICON_INFO, info_msg);
    
    display_divider();
    
    /* PHASE 1: Allocation */
    display_section("PHASE 1: Memory Allocation");

    /* Each node allocates its own A and C partitions (in parallel) */
    display_action(node_id, ICON_MEM, "Allocating my partitions (A and C)...");
    if (matrix_allocate_my_partitions(&state) != 0) {
        display_action(node_id, ICON_CROSS, "Allocation failed");
        dsm_finalize();
        return 1;
    }
    double kb = (state.rows_per_node[node_id] * N * sizeof(double)) / 1024.0;
    snprintf(info_msg, sizeof(info_msg), "Allocated %.1f KB per matrix", kb);
    display_action(node_id, ICON_CHECK, info_msg);

    /* Wait for all nodes to complete their allocations */
    dsm_barrier(BARRIER_ALLOC_A_BASE, num_nodes);
    display_barrier(BARRIER_ALLOC_A_BASE, num_nodes);

    display_divider();
    
    if (node_id == 0) {
        display_action(node_id, ICON_MEM, "Allocating matrix B (shared by all)...");
        if (matrix_allocate_B(&state) != 0) {
            display_action(node_id, ICON_CROSS, "B allocation failed");
            matrix_state_cleanup(&state);
            dsm_finalize();
            return 1;
        }
        double kb = (N * N * sizeof(double)) / 1024.0;
        snprintf(info_msg, sizeof(info_msg), "Allocated %.1f KB for matrix B", kb);
        display_action(node_id, ICON_CHECK, info_msg);
    }
    
    dsm_barrier(BARRIER_ALLOC_B, num_nodes);
    display_barrier(BARRIER_ALLOC_B, num_nodes);
    
    display_divider();
    
    display_action(node_id, ICON_MEM, "Retrieving SVAS pointers to all partitions...");
    if (matrix_retrieve_all_partitions(&state) != 0) {
        display_action(node_id, ICON_CROSS, "Failed to retrieve partitions");
        matrix_state_cleanup(&state);
        dsm_finalize();
        return 1;
    }
    display_action(node_id, ICON_CHECK, "All partition pointers retrieved");
    
    dsm_barrier(BARRIER_INIT, num_nodes);
    display_barrier(BARRIER_INIT, num_nodes);
    
    printf("\n");
    display_divider();
    printf("\n");
    
    /* PHASE 2: Initialization */
    display_section("PHASE 2: Matrix Initialization");
    
    display_action(node_id, ICON_BOLT, "Initializing matrices with test pattern...");
    matrix_initialize(&state);
    display_action(node_id, ICON_CHECK, "Matrices initialized");
    
    dsm_barrier(BARRIER_INIT + 1, num_nodes);
    display_barrier(BARRIER_INIT + 1, num_nodes);
    
    printf("\n");
    display_divider();
    printf("\n");
    
    /* PHASE 3: Computation */
    display_section("PHASE 3: Matrix Multiplication");
    
    snprintf(info_msg, sizeof(info_msg), "Computing C = A x B (rows %d-%d)...",
             state.start_row[node_id], state.end_row[node_id] - 1);
    display_action(node_id, ICON_BOLT, info_msg);
    
    if (node_id != 0) {
        display_page_transfer(0, node_id, "fetching matrix B (read-only)");
    }
    
    dsm_reset_stats();
    double start_time = get_time_ms();
    
    matrix_compute(&state);
    
    double compute_time = get_time_ms() - start_time;
    snprintf(info_msg, sizeof(info_msg), "Computation complete in %.2f ms", compute_time);
    display_action(node_id, ICON_CHECK, info_msg);
    
    dsm_stats_t stats;
    dsm_get_stats(&stats);
    snprintf(info_msg, sizeof(info_msg), "Page faults: %lu (read: %lu, write: %lu)",
             stats.page_faults, stats.read_faults, stats.write_faults);
    display_action(node_id, ICON_INFO, info_msg);
    
    dsm_barrier(BARRIER_COMPUTE, num_nodes);
    display_barrier(BARRIER_COMPUTE, num_nodes);
    
    printf("\n");
    display_divider();
    printf("\n");
    
    /* PHASE 4: Verification (optional) */
    if (enable_verify) {
        display_section("PHASE 4: Result Verification");
        
        display_action(node_id, ICON_BOLT, "Verifying computed results...");
        int verify_result = matrix_verify(&state);
        if (verify_result) {
            display_action(node_id, ICON_CHECK, "All sampled results verified correct");
        } else {
            display_action(node_id, ICON_CROSS, "Verification failed - mismatch detected");
        }
        dsm_barrier(BARRIER_VERIFY, num_nodes);
        display_barrier(BARRIER_VERIFY, num_nodes);
        
        printf("\n");
        display_divider();
        printf("\n");
    }
    
    /* Sample output */
    if (enable_sample && node_id == 0) {
        display_section("Sample Output");
        matrix_print_sample(&state, 5);
        display_divider();
        printf("\n");
    }
    
    /* Final statistics */
    dsm_get_stats(&stats);
    
    uint64_t avg_latency = 0;
    if (stats.page_faults > 0) {
        avg_latency = stats.total_fault_latency_ns / stats.page_faults / 1000;
    }
    
    display_full_stats(node_id,
                      stats.page_faults,
                      stats.read_faults,
                      stats.write_faults,
                      stats.pages_fetched,
                      stats.pages_sent,
                      stats.invalidations_sent,
                      stats.invalidations_received,
                      stats.network_bytes_sent,
                      stats.network_bytes_received,
                      avg_latency,
                      stats.max_fault_latency_ns / 1000,
                      stats.min_fault_latency_ns / 1000);
    
    /* Cleanup */
    dsm_barrier(BARRIER_FINAL, num_nodes);
    
    display_action(node_id, ICON_MEM, "Cleaning up resources...");
    matrix_state_cleanup(&state);
    display_action(node_id, ICON_CHECK, "Resources freed");
    
    dsm_finalize();
    
    printf("\n");
    display_action(node_id, ICON_CHECK, "Matrix multiplication completed successfully!");
    printf("\n");
    return 0;
}
