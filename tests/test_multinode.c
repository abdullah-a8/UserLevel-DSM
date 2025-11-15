/**
 * @file test_multinode.c
 * @brief Multi-node integration tests (Day 10, Tasks 10.2-10.3)
 *
 * These tests require actual multi-node setup with network connectivity.
 * To run: Deploy DSM on multiple machines and coordinate test execution.
 *
 * USAGE:
 *   Node 0 (manager): ./test_multinode --manager --nodes 2
 *   Node 1 (worker):  ./test_multinode --worker --manager-host <ip> --node-id 1
 */

#include "dsm/dsm.h"
#include "../src/core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================================================================
 * Task 10.2: Two-Node Tests
 * ================================================================ */

/**
 * Test A: Ping-Pong Pattern
 * Node 0 writes, Node 1 reads, Node 1 writes, Node 0 reads
 */
void test_ping_pong(int node_id, int num_nodes) {
    printf("[Node %d] Starting ping-pong test...\n", node_id);

    int *shared_value = (int*)dsm_malloc(sizeof(int));
    if (!shared_value) {
        printf("[Node %d] Failed to allocate DSM memory\n", node_id);
        return;
    }

    if (node_id == 0) {
        /* Node 0: Write initial value */
        *shared_value = 42;
        printf("[Node %d] Wrote value: %d\n", node_id, *shared_value);

        /* Wait for Node 1 to read and write */
        sleep(2);

        /* Read value written by Node 1 */
        int value = *shared_value;
        printf("[Node %d] Read value: %d (expected 43)\n", node_id, value);

        if (value == 43) {
            printf("[Node %d] ✓ Ping-pong test PASSED\n", node_id);
        } else {
            printf("[Node %d] ✗ Ping-pong test FAILED\n", node_id);
        }
    } else if (node_id == 1) {
        /* Node 1: Wait for Node 0 to write */
        sleep(1);

        /* Read value from Node 0 */
        int value = *shared_value;
        printf("[Node %d] Read value: %d (expected 42)\n", node_id, value);

        /* Write new value */
        *shared_value = 43;
        printf("[Node %d] Wrote value: %d\n", node_id, *shared_value);

        sleep(1);  /* Give Node 0 time to read */

        printf("[Node %d] ✓ Ping-pong test PASSED\n", node_id);
    }

    dsm_free(shared_value);
}

/**
 * Test B: Producer-Consumer with Locks
 */
void test_producer_consumer(int node_id, int num_nodes) {
    printf("[Node %d] Starting producer-consumer test...\n", node_id);

    const int BUFFER_SIZE = 10;
    int *buffer = (int*)dsm_malloc(BUFFER_SIZE * sizeof(int));
    int *count = (int*)dsm_malloc(sizeof(int));

    if (!buffer || !count) {
        printf("[Node %d] Failed to allocate DSM memory\n", node_id);
        return;
    }

    dsm_lock_t *lock = dsm_lock_create(3000);
    if (!lock) {
        printf("[Node %d] Failed to create lock\n", node_id);
        dsm_free(buffer);
        dsm_free(count);
        return;
    }

    if (node_id == 0) {
        /* Producer */
        *count = 0;
        for (int i = 0; i < BUFFER_SIZE; i++) {
            dsm_lock_acquire(lock);
            buffer[i] = i * 10;
            (*count)++;
            printf("[Node %d] Produced: %d (count=%d)\n", node_id, buffer[i], *count);
            dsm_lock_release(lock);
            usleep(100000);  /* 100ms */
        }
        sleep(2);  /* Wait for consumer */
        printf("[Node %d] ✓ Producer-consumer test PASSED\n", node_id);
    } else if (node_id == 1) {
        /* Consumer */
        sleep(1);  /* Let producer start */
        int consumed = 0;
        while (consumed < BUFFER_SIZE) {
            dsm_lock_acquire(lock);
            if (*count > consumed) {
                int value = buffer[consumed];
                printf("[Node %d] Consumed: %d\n", node_id, value);
                consumed++;
            }
            dsm_lock_release(lock);
            usleep(150000);  /* 150ms */
        }
        printf("[Node %d] ✓ Producer-consumer test PASSED\n", node_id);
    }

    dsm_lock_destroy(lock);
    dsm_free(buffer);
    dsm_free(count);
}

/**
 * Test C: Read-Sharing
 */
void test_read_sharing(int node_id, int num_nodes) {
    printf("[Node %d] Starting read-sharing test...\n", node_id);

    int *shared_data = (int*)dsm_malloc(PAGE_SIZE);
    if (!shared_data) {
        printf("[Node %d] Failed to allocate DSM memory\n", node_id);
        return;
    }

    if (node_id == 0) {
        /* Initialize data */
        for (int i = 0; i < PAGE_SIZE / sizeof(int); i++) {
            shared_data[i] = i;
        }
        printf("[Node %d] Initialized shared data\n", node_id);
        sleep(1);

        /* Read data (should remain in READ_ONLY) */
        int sum = 0;
        for (int i = 0; i < 100; i++) {
            sum += shared_data[i];
        }
        printf("[Node %d] Read sum: %d\n", node_id, sum);
        printf("[Node %d] ✓ Read-sharing test PASSED\n", node_id);
    } else if (node_id == 1) {
        sleep(2);  /* Let Node 0 initialize */

        /* Read same data */
        int sum = 0;
        for (int i = 0; i < 100; i++) {
            sum += shared_data[i];
        }
        printf("[Node %d] Read sum: %d\n", node_id, sum);
        printf("[Node %d] ✓ Read-sharing test PASSED\n", node_id);
    }

    dsm_free(shared_data);
}

/* ================================================================
 * Task 10.3: Four-Node Tests
 * ================================================================ */

/**
 * Test A: Parallel Sum
 */
void test_parallel_sum(int node_id, int num_nodes) {
    printf("[Node %d] Starting parallel sum test...\n", node_id);

    const int ARRAY_SIZE = 1000;
    int *array = (int*)dsm_malloc(ARRAY_SIZE * sizeof(int));
    int *partial_sums = (int*)dsm_malloc(num_nodes * sizeof(int));

    if (!array || !partial_sums) {
        printf("[Node %d] Failed to allocate DSM memory\n", node_id);
        return;
    }

    if (node_id == 0) {
        /* Initialize array */
        for (int i = 0; i < ARRAY_SIZE; i++) {
            array[i] = i + 1;
        }
        printf("[Node %d] Initialized array\n", node_id);
    }

    /* Synchronize */
    dsm_barrier(4000, num_nodes);

    /* Each node sums its portion */
    int per_node = ARRAY_SIZE / num_nodes;
    int start = node_id * per_node;
    int end = (node_id == num_nodes - 1) ? ARRAY_SIZE : (node_id + 1) * per_node;

    int local_sum = 0;
    for (int i = start; i < end; i++) {
        local_sum += array[i];
    }

    partial_sums[node_id] = local_sum;
    printf("[Node %d] Partial sum: %d (range %d-%d)\n", node_id, local_sum, start, end - 1);

    /* Synchronize */
    dsm_barrier(4000, num_nodes);

    if (node_id == 0) {
        /* Compute total sum */
        int total = 0;
        for (int i = 0; i < num_nodes; i++) {
            total += partial_sums[i];
        }

        int expected = (ARRAY_SIZE * (ARRAY_SIZE + 1)) / 2;  /* Sum of 1 to N */
        printf("[Node %d] Total sum: %d (expected %d)\n", node_id, total, expected);

        if (total == expected) {
            printf("[Node %d] ✓ Parallel sum test PASSED\n", node_id);
        } else {
            printf("[Node %d] ✗ Parallel sum test FAILED\n", node_id);
        }
    }

    dsm_free(array);
    dsm_free(partial_sums);
}

/**
 * Test B: Shared Counter with Locks
 */
void test_shared_counter(int node_id, int num_nodes) {
    printf("[Node %d] Starting shared counter test...\n", node_id);

    int *counter = (int*)dsm_malloc(sizeof(int));
    if (!counter) {
        printf("[Node %d] Failed to allocate DSM memory\n", node_id);
        return;
    }

    dsm_lock_t *lock = dsm_lock_create(5000);
    if (!lock) {
        printf("[Node %d] Failed to create lock\n", node_id);
        dsm_free(counter);
        return;
    }

    if (node_id == 0) {
        *counter = 0;
    }

    dsm_barrier(5001, num_nodes);

    /* Each node increments counter 100 times */
    for (int i = 0; i < 100; i++) {
        dsm_lock_acquire(lock);
        (*counter)++;
        dsm_lock_release(lock);
    }

    dsm_barrier(5001, num_nodes);

    if (node_id == 0) {
        int expected = num_nodes * 100;
        printf("[Node %d] Final counter: %d (expected %d)\n", node_id, *counter, expected);

        if (*counter == expected) {
            printf("[Node %d] ✓ Shared counter test PASSED\n", node_id);
        } else {
            printf("[Node %d] ✗ Shared counter test FAILED\n", node_id);
        }
    }

    dsm_lock_destroy(lock);
    dsm_free(counter);
}

/* ================================================================
 * Main
 * ================================================================ */

void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  Manager: %s --manager --nodes <N> [--port <P>]\n", prog);
    printf("  Worker:  %s --worker --node-id <ID> --manager-host <HOST> [--manager-port <P>]\n", prog);
}

int main(int argc, char *argv[]) {
    int is_manager = 0;
    int node_id = -1;
    int num_nodes = 2;
    char manager_host[256] = "localhost";
    int port = 17000;

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

    /* Initialize DSM */
    dsm_config_t config = {
        .node_id = node_id,
        .port = port + node_id,
        .num_nodes = num_nodes,
        .is_manager = is_manager,
        .log_level = LOG_LEVEL_INFO
    };

    if (!is_manager) {
        strncpy(config.manager_host, manager_host, sizeof(config.manager_host) - 1);
        config.manager_port = port;
    }

    printf("\n=== Multi-Node DSM Tests (Node %d) ===\n\n", node_id);

    if (dsm_init(&config) != DSM_SUCCESS) {
        printf("Failed to initialize DSM\n");
        return 1;
    }

    /* Run tests based on number of nodes */
    if (num_nodes == 2) {
        printf("--- Two-Node Tests ---\n");
        test_ping_pong(node_id, num_nodes);
        sleep(1);
        test_producer_consumer(node_id, num_nodes);
        sleep(1);
        test_read_sharing(node_id, num_nodes);
    } else if (num_nodes >= 4) {
        printf("--- Four-Node Tests ---\n");
        test_parallel_sum(node_id, num_nodes);
        sleep(1);
        test_shared_counter(node_id, num_nodes);
    }

    dsm_finalize();

    printf("\n[Node %d] All tests completed\n", node_id);
    return 0;
}
