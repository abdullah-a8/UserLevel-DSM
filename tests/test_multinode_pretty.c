/**
 * @file test_multinode_pretty.c
 * @brief Pretty multi-node integration tests for presentation
 *
 * Beautiful terminal output version of test_multinode.c for class presentations.
 * Uses ANSI styling for visually appealing test output.
 *
 * USAGE:
 *   Node 0 (manager): ./test_multinode_pretty --manager --nodes 2
 *   Node 1 (worker):  ./test_multinode_pretty --worker --manager-host <ip> --node-id 1
 */

#include "dsm/dsm.h"
#include "../src/core/log.h"
#include "test_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Test results tracking */
static test_result_t g_test_results[10];
static int g_num_tests = 0;

/* ════════════════════════════════════════════════════════════════════════════
 *                              TEST A: PING-PONG
 * ════════════════════════════════════════════════════════════════════════════ */

/**
 * Test A: Ping-Pong Pattern
 * Node 0 writes, Node 1 reads, Node 1 writes, Node 0 reads
 */
int test_ping_pong_pretty(int node_id, int num_nodes) {
    int passed = 1;
    
    display_test_start("PING-PONG", "Cross-node read/write memory coherence test");
    printf("\n");
    
    int *shared_value = NULL;

    /* Phase 1: Allocation */
    if (node_id == 0) {
        display_action(node_id, ICON_MEM, "Allocating shared memory...");
        shared_value = (int*)dsm_malloc(sizeof(int));
        if (shared_value) {
            display_action(node_id, ICON_CHECK, "Allocated DSM region");
        }
    }
    
    dsm_barrier(10, num_nodes);
    display_barrier(10, num_nodes);
    
    if (node_id != 0) {
        display_action(node_id, ICON_MEM, "Retrieving shared memory mapping...");
        shared_value = (int*)dsm_get_allocation(0);
        if (shared_value) {
            display_action(node_id, ICON_CHECK, "Retrieved DSM region from manager");
        }
    }

    if (!shared_value) {
        display_action(node_id, ICON_CROSS, "Failed to allocate/retrieve DSM memory");
        return 0;
    }

    display_divider();

    /* Phase 2: Ping-Pong Exchange */
    if (node_id == 0) {
        /* Node 0: Write initial value */
        display_action(node_id, ICON_BOLT, "Writing initial value to shared memory...");
        *shared_value = 42;
        display_memory_op(node_id, "WRITE", shared_value, 42);

        dsm_barrier(1000, num_nodes);
        display_barrier(1000, num_nodes);
        
        display_divider();
        display_action(node_id, ICON_CLOCK, "Waiting for Node 1 to modify value...");
        
        dsm_barrier(1001, num_nodes);
        display_barrier(1001, num_nodes);

        /* Read value written by Node 1 */
        display_action(node_id, ICON_BOLT, "Reading value modified by Node 1...");
        display_page_transfer(1, 0, "write invalidation");
        int value = *shared_value;
        display_memory_op(node_id, "READ", shared_value, value);
        
        passed = (value == 43);
        display_verify(node_id, 43, value, passed);

    } else if (node_id == 1) {
        dsm_barrier(1000, num_nodes);
        display_barrier(1000, num_nodes);

        /* Read value from Node 0 */
        display_action(node_id, ICON_BOLT, "Reading value written by Node 0...");
        display_page_transfer(0, 1, "read fault");
        int value = *shared_value;
        display_memory_op(node_id, "READ", shared_value, value);
        
        if (value != 42) {
            display_verify(node_id, 42, value, 0);
            dsm_barrier(1001, num_nodes);
            dsm_barrier(1002, num_nodes);
            dsm_free(shared_value);
            return 0;
        }
        display_verify(node_id, 42, value, 1);

        display_divider();

        /* Write new value */
        display_action(node_id, ICON_BOLT, "Modifying shared value...");
        *shared_value = 43;
        display_memory_op(node_id, "WRITE", shared_value, 43);

        dsm_barrier(1001, num_nodes);
        display_barrier(1001, num_nodes);
        
    } else {
        dsm_barrier(1000, num_nodes);
        dsm_barrier(1001, num_nodes);
    }

    dsm_barrier(1002, num_nodes);
    dsm_free(shared_value);
    
    display_test_result("Ping-Pong", passed);
    return passed;
}

/* ════════════════════════════════════════════════════════════════════════════
 *                         TEST B: PRODUCER-CONSUMER
 * ════════════════════════════════════════════════════════════════════════════ */

/**
 * Test B: Producer-Consumer with Locks
 */
int test_producer_consumer_pretty(int node_id, int num_nodes) {
    int passed = 1;
    
    display_test_start("PRODUCER-CONSUMER", "Lock-protected buffer with cross-node coordination");
    printf("\n");

    const int BUFFER_SIZE = 5;  /* Reduced for cleaner output */
    int *buffer = NULL;
    int *count = NULL;

    /* Phase 1: Allocation */
    if (node_id == 0) {
        display_action(node_id, ICON_MEM, "Allocating shared buffer...");
        buffer = (int*)dsm_malloc(BUFFER_SIZE * sizeof(int));
        count = (int*)dsm_malloc(sizeof(int));
        display_action(node_id, ICON_CHECK, "Buffer and counter allocated");
    }
    
    dsm_barrier(20, num_nodes);
    display_barrier(20, num_nodes);
    
    if (node_id != 0) {
        display_action(node_id, ICON_MEM, "Mapping shared buffer...");
        buffer = (int*)dsm_get_allocation(0);
        count = (int*)dsm_get_allocation(1);
        display_action(node_id, ICON_CHECK, "Shared buffer mapped");
    }

    if (!buffer || !count) {
        display_action(node_id, ICON_CROSS, "Failed to allocate DSM memory");
        return 0;
    }

    display_divider();

    /* Phase 2: Create Lock */
    display_action(node_id, ICON_LOCK, "Creating distributed lock...");
    dsm_lock_t *lock = dsm_lock_create(3000);
    if (!lock) {
        display_action(node_id, ICON_CROSS, "Failed to create lock");
        dsm_free(buffer);
        dsm_free(count);
        return 0;
    }
    display_action(node_id, ICON_CHECK, "Distributed lock created (ID: 3000)");
    
    display_divider();

    /* Phase 3: Producer-Consumer */
    if (node_id == 0) {
        /* Producer */
        display_section("PRODUCER (Node 0)");
        *count = 0;
        
        for (int i = 0; i < BUFFER_SIZE; i++) {
            dsm_lock_acquire(lock);
            display_lock_op(node_id, "ACQUIRE", 3000);
            
            buffer[i] = i * 10;
            (*count)++;
            
            char msg[64];
            snprintf(msg, sizeof(msg), "Produced item[%d] = %d (count=%d)", i, buffer[i], *count);
            display_action(node_id, ICON_SQUARE, msg);
            
            dsm_lock_release(lock);
            display_lock_op(node_id, "RELEASE", 3000);
            
            usleep(100000);
        }
        
        sleep(2);
        display_action(node_id, ICON_CHECK, "Production complete");
        
    } else if (node_id == 1) {
        /* Consumer */
        sleep(1);
        display_section("CONSUMER (Node 1)");
        
        int consumed = 0;
        while (consumed < BUFFER_SIZE) {
            dsm_lock_acquire(lock);
            display_lock_op(node_id, "ACQUIRE", 3000);
            
            if (*count > consumed) {
                int value = buffer[consumed];
                char msg[64];
                snprintf(msg, sizeof(msg), "Consumed item[%d] = %d", consumed, value);
                display_action(node_id, ICON_CIRCLE, msg);
                consumed++;
            }
            
            dsm_lock_release(lock);
            display_lock_op(node_id, "RELEASE", 3000);
            
            usleep(150000);
        }
        
        display_action(node_id, ICON_CHECK, "Consumption complete");
    }

    dsm_barrier(21, num_nodes);
    display_barrier(21, num_nodes);

    dsm_lock_destroy(lock);
    dsm_free(buffer);
    dsm_free(count);
    
    display_test_result("Producer-Consumer", passed);
    return passed;
}

/* ════════════════════════════════════════════════════════════════════════════
 *                           TEST C: READ SHARING
 * ════════════════════════════════════════════════════════════════════════════ */

/**
 * Test C: Read-Sharing (Multiple nodes read same data)
 */
int test_read_sharing_pretty(int node_id, int num_nodes) {
    int passed = 1;
    
    display_test_start("READ-SHARING", "Multiple nodes sharing read-only page access");
    printf("\n");

    int *shared_data = NULL;
    
    if (node_id == 0) {
        display_action(node_id, ICON_MEM, "Allocating shared data array...");
        shared_data = (int*)dsm_malloc(PAGE_SIZE);
        display_action(node_id, ICON_CHECK, "Allocated 4KB shared region");
    }
    
    dsm_barrier(30, num_nodes);
    display_barrier(30, num_nodes);
    
    if (node_id != 0) {
        display_action(node_id, ICON_MEM, "Mapping shared data array...");
        shared_data = (int*)dsm_get_allocation(0);
        display_action(node_id, ICON_CHECK, "Shared data mapped");
    }

    if (!shared_data) {
        display_action(node_id, ICON_CROSS, "Failed to allocate DSM memory");
        return 0;
    }

    const int EXPECTED_SUM = (100 * 99) / 2;  /* Sum of 0 to 99 */
    
    display_divider();

    if (node_id == 0) {
        /* Initialize data */
        display_action(node_id, ICON_BOLT, "Initializing shared array [0..99]...");
        for (int i = 0; i < PAGE_SIZE / (int)sizeof(int); i++) {
            shared_data[i] = i;
        }
        display_action(node_id, ICON_CHECK, "Array initialized with values 0-99");

        dsm_barrier(2000, num_nodes);
        display_barrier(2000, num_nodes);
        
        display_divider();

        /* Read data */
        display_action(node_id, ICON_BOLT, "Computing sum of first 100 elements...");
        int sum = 0;
        for (int i = 0; i < 100; i++) {
            sum += shared_data[i];
        }
        
        char msg[64];
        snprintf(msg, sizeof(msg), "Computed sum = %d", sum);
        display_action(node_id, ICON_INFO, msg);
        
        passed = (sum == EXPECTED_SUM);
        display_verify(node_id, EXPECTED_SUM, sum, passed);

    } else if (node_id == 1) {
        dsm_barrier(2000, num_nodes);
        display_barrier(2000, num_nodes);
        
        display_divider();

        /* Read same data - should share READ_ONLY copy */
        display_action(node_id, ICON_BOLT, "Reading shared array (should trigger page fetch)...");
        display_page_transfer(0, 1, "read sharing");
        
        int sum = 0;
        for (int i = 0; i < 100; i++) {
            sum += shared_data[i];
        }
        
        char msg[64];
        snprintf(msg, sizeof(msg), "Computed sum = %d", sum);
        display_action(node_id, ICON_INFO, msg);
        
        passed = (sum == EXPECTED_SUM);
        display_verify(node_id, EXPECTED_SUM, sum, passed);
        
        display_action(node_id, ICON_INFO, "Both nodes can read simultaneously (READ_ONLY state)");

    } else {
        dsm_barrier(2000, num_nodes);
    }

    dsm_barrier(2001, num_nodes);
    dsm_free(shared_data);
    
    display_test_result("Read-Sharing", passed);
    return passed;
}

/* ════════════════════════════════════════════════════════════════════════════
 *                           TEST D: PARALLEL SUM
 * ════════════════════════════════════════════════════════════════════════════ */

/**
 * Test D: Parallel Sum (4+ nodes)
 */
int test_parallel_sum_pretty(int node_id, int num_nodes) {
    int passed = 1;
    
    display_test_start("PARALLEL SUM", "Distributed array summation across all nodes");
    printf("\n");

    const int ARRAY_SIZE = 1000;
    int *array = NULL;
    int *partial_sums = NULL;

    if (node_id == 0) {
        display_action(node_id, ICON_MEM, "Allocating shared array and partial sums...");
        array = (int*)dsm_malloc(ARRAY_SIZE * sizeof(int));
        partial_sums = (int*)dsm_malloc(num_nodes * sizeof(int));
        display_action(node_id, ICON_CHECK, "Allocated shared regions");
    }
    
    dsm_barrier(40, num_nodes);
    display_barrier(40, num_nodes);
    
    if (node_id != 0) {
        display_action(node_id, ICON_MEM, "Mapping shared regions...");
        array = (int*)dsm_get_allocation(0);
        partial_sums = (int*)dsm_get_allocation(1);
        display_action(node_id, ICON_CHECK, "Shared regions mapped");
    }

    if (!array || !partial_sums) {
        display_action(node_id, ICON_CROSS, "Failed to allocate DSM memory");
        return 0;
    }

    display_divider();

    if (node_id == 0) {
        display_action(node_id, ICON_BOLT, "Initializing array [1..1000]...");
        for (int i = 0; i < ARRAY_SIZE; i++) {
            array[i] = i + 1;
        }
        display_action(node_id, ICON_CHECK, "Array initialized");
    }

    dsm_barrier(4000, num_nodes);
    display_barrier(4000, num_nodes);
    
    display_divider();

    /* Each node sums its portion */
    int per_node = ARRAY_SIZE / num_nodes;
    int start = node_id * per_node;
    int end = (node_id == num_nodes - 1) ? ARRAY_SIZE : (node_id + 1) * per_node;

    char range_msg[64];
    snprintf(range_msg, sizeof(range_msg), "Computing partial sum for range [%d..%d]...", start, end - 1);
    display_action(node_id, ICON_BOLT, range_msg);
    
    if (node_id != 0) {
        display_page_transfer(0, node_id, "array read");
    }

    int local_sum = 0;
    for (int i = start; i < end; i++) {
        local_sum += array[i];
    }

    partial_sums[node_id] = local_sum;
    
    char sum_msg[64];
    snprintf(sum_msg, sizeof(sum_msg), "Partial sum = %d", local_sum);
    display_action(node_id, ICON_INFO, sum_msg);

    dsm_barrier(4001, num_nodes);
    display_barrier(4001, num_nodes);

    if (node_id == 0) {
        display_divider();
        display_action(node_id, ICON_BOLT, "Aggregating partial sums from all nodes...");
        
        int total = 0;
        for (int i = 0; i < num_nodes; i++) {
            total += partial_sums[i];
        }

        int expected = (ARRAY_SIZE * (ARRAY_SIZE + 1)) / 2;
        
        char total_msg[64];
        snprintf(total_msg, sizeof(total_msg), "Total sum = %d", total);
        display_action(node_id, ICON_INFO, total_msg);
        
        passed = (total == expected);
        display_verify(node_id, expected, total, passed);
    }

    dsm_free(array);
    dsm_free(partial_sums);
    
    display_test_result("Parallel Sum", passed);
    return passed;
}

/* ════════════════════════════════════════════════════════════════════════════
 *                         TEST E: SHARED COUNTER
 * ════════════════════════════════════════════════════════════════════════════ */

/**
 * Test E: Shared Counter with Locks (4+ nodes)
 */
int test_shared_counter_pretty(int node_id, int num_nodes) {
    int passed = 1;
    
    display_test_start("SHARED COUNTER", "Lock-protected counter incremented by all nodes");
    printf("\n");

    int *counter = NULL;
    
    if (node_id == 0) {
        display_action(node_id, ICON_MEM, "Allocating shared counter...");
        counter = (int*)dsm_malloc(sizeof(int));
        display_action(node_id, ICON_CHECK, "Counter allocated");
    }
    
    dsm_barrier(50, num_nodes);
    display_barrier(50, num_nodes);
    
    if (node_id != 0) {
        display_action(node_id, ICON_MEM, "Mapping shared counter...");
        counter = (int*)dsm_get_allocation(0);
        display_action(node_id, ICON_CHECK, "Counter mapped");
    }

    if (!counter) {
        display_action(node_id, ICON_CROSS, "Failed to allocate DSM memory");
        return 0;
    }

    display_divider();

    display_action(node_id, ICON_LOCK, "Creating distributed lock...");
    dsm_lock_t *lock = dsm_lock_create(5000);
    if (!lock) {
        display_action(node_id, ICON_CROSS, "Failed to create lock");
        dsm_free(counter);
        return 0;
    }
    display_action(node_id, ICON_CHECK, "Distributed lock created (ID: 5000)");

    if (node_id == 0) {
        *counter = 0;
        display_action(node_id, ICON_INFO, "Counter initialized to 0");
    }

    dsm_barrier(5001, num_nodes);
    display_barrier(5001, num_nodes);
    
    display_divider();

    /* Each node increments counter N times */
    const int INCREMENTS = 25;  /* Reduced for cleaner output */
    
    char inc_msg[64];
    snprintf(inc_msg, sizeof(inc_msg), "Incrementing counter %d times...", INCREMENTS);
    display_action(node_id, ICON_BOLT, inc_msg);
    
    for (int i = 0; i < INCREMENTS; i++) {
        dsm_lock_acquire(lock);
        (*counter)++;
        dsm_lock_release(lock);
        
        /* Show progress every 5 increments */
        if ((i + 1) % 5 == 0) {
            display_progress("Incrementing", i + 1, INCREMENTS);
        }
    }
    
    char done_msg[64];
    snprintf(done_msg, sizeof(done_msg), "Completed %d increments", INCREMENTS);
    display_action(node_id, ICON_CHECK, done_msg);

    dsm_barrier(5002, num_nodes);
    display_barrier(5002, num_nodes);

    if (node_id == 0) {
        display_divider();
        int expected = num_nodes * INCREMENTS;
        
        char result_msg[64];
        snprintf(result_msg, sizeof(result_msg), "Final counter value = %d", *counter);
        display_action(node_id, ICON_INFO, result_msg);
        
        passed = (*counter == expected);
        display_verify(node_id, expected, *counter, passed);
    }

    dsm_lock_destroy(lock);
    dsm_free(counter);
    
    display_test_result("Shared Counter", passed);
    return passed;
}

/* ════════════════════════════════════════════════════════════════════════════
 *                                   MAIN
 * ════════════════════════════════════════════════════════════════════════════ */

void print_usage(const char *prog) {
    printf("\n");
    printf(BCYN "Usage:\n" RST);
    printf("  Manager: " BWHT "%s --manager --nodes <N> [--port <P>]\n" RST, prog);
    printf("  Worker:  " BWHT "%s --worker --node-id <ID> --manager-host <HOST> [--manager-port <P>]\n" RST, prog);
    printf("\n");
}

int main(int argc, char *argv[]) {
    int is_manager = 0;
    int node_id = -1;
    int num_nodes = 2;
    char manager_host[256] = "localhost";
    int port = 5000;

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
        }
    }

    if (node_id < 0) {
        print_usage(argv[0]);
        return 1;
    }

    /* Initialize DSM with minimal log level for clean presentation output */
    dsm_config_t config = {
        .node_id = node_id,
        .port = port + node_id,
        .num_nodes = num_nodes,
        .is_manager = is_manager,
        .log_level = LOG_LEVEL_ERROR  /* Minimal verbosity - only show critical errors */
    };

    if (!is_manager) {
        strncpy(config.manager_host, manager_host, sizeof(config.manager_host) - 1);
        config.manager_port = port;
    }

    /* Display banner */
    display_banner("USER-LEVEL DISTRIBUTED SHARED MEMORY", "Multi-Node Integration Tests");
    
    /* Display node info */
    display_node_info(node_id, is_manager, num_nodes, is_manager ? NULL : manager_host);

    /* Initialize DSM */
    display_action(node_id, ICON_GEAR, "Initializing DSM subsystem...");
    
    if (dsm_init(&config) != DSM_SUCCESS) {
        display_action(node_id, ICON_CROSS, "Failed to initialize DSM");
        return 1;
    }
    display_action(node_id, ICON_CHECK, "DSM initialized successfully");
    
    /* Initialize performance logging */
    dsm_perf_log_init(NULL);
    
    printf("\n");
    display_divider();
    printf("\n");

    /* Run tests based on number of nodes */
    if (num_nodes == 2) {
        display_section("TWO-NODE TEST SUITE");
        
        g_test_results[g_num_tests].name = "Ping-Pong Pattern";
        g_test_results[g_num_tests].passed = test_ping_pong_pretty(node_id, num_nodes);
        g_num_tests++;
        
        dsm_barrier(9000, num_nodes);
        
        g_test_results[g_num_tests].name = "Producer-Consumer";
        g_test_results[g_num_tests].passed = test_producer_consumer_pretty(node_id, num_nodes);
        g_num_tests++;
        
        dsm_barrier(9001, num_nodes);
        
        g_test_results[g_num_tests].name = "Read-Sharing";
        g_test_results[g_num_tests].passed = test_read_sharing_pretty(node_id, num_nodes);
        g_num_tests++;
        
        dsm_barrier(9002, num_nodes);
        
    } else if (num_nodes >= 4) {
        display_section("FOUR-NODE TEST SUITE");
        
        g_test_results[g_num_tests].name = "Parallel Sum";
        g_test_results[g_num_tests].passed = test_parallel_sum_pretty(node_id, num_nodes);
        g_num_tests++;
        
        dsm_barrier(9003, num_nodes);
        
        g_test_results[g_num_tests].name = "Shared Counter";
        g_test_results[g_num_tests].passed = test_shared_counter_pretty(node_id, num_nodes);
        g_num_tests++;
        
        dsm_barrier(9004, num_nodes);
    }

    /* Display test summary */
    display_test_summary(g_test_results, g_num_tests, node_id);

    /* Display DSM statistics */
    dsm_stats_t stats;
    if (dsm_get_stats(&stats) == DSM_SUCCESS) {
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
    }

    /* Cleanup */
    dsm_finalize();

    display_action(node_id, ICON_CHECK, "All tests completed successfully");
    printf("\n");
    
    return 0;
}
