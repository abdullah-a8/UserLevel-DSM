/**
 * @file test_integration.c
 * @brief Integration tests for DSM system (Day 10)
 *
 * Tests the complete DSM system with realistic workloads including:
 * - Multi-threaded access patterns
 * - False sharing scenarios
 * - Consistency validation
 * - Performance measurements
 */

#include "dsm/dsm.h"
#include "../src/core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(test) do { \
    printf("Running %s... ", #test); \
    fflush(stdout); \
    if (test()) { \
        printf("PASS\n"); \
        tests_passed++; \
    } else { \
        printf("FAIL\n"); \
        tests_failed++; \
    } \
} while(0)

/* Timing utilities */
static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

/* ================================================================
 * Task 10.1: Single-Node Multi-Threaded Tests
 * ================================================================ */

typedef struct {
    int *shared_array;
    int start_idx;
    int end_idx;
    int thread_id;
    dsm_lock_t *lock;
    barrier_id_t barrier_id;
    int num_threads;
} thread_args_t;

/**
 * Test 10.1.1: Multiple threads reading and writing DSM memory
 */
void* multithread_worker(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;

    /* Each thread writes to its assigned region */
    for (int i = args->start_idx; i < args->end_idx; i++) {
        args->shared_array[i] = args->thread_id * 1000 + (i - args->start_idx);
    }

    /* Synchronize at barrier */
    dsm_barrier(args->barrier_id, args->num_threads);

    /* Verify other threads' writes */
    int errors = 0;
    int total_elements = (args->end_idx - args->start_idx) * args->num_threads;
    int elements_per_thread = args->end_idx - args->start_idx;
    
    for (int i = 0; i < total_elements; i++) {
        int expected_thread = i / elements_per_thread;
        int offset_in_thread = i % elements_per_thread;
        int expected_val = expected_thread * 1000 + offset_in_thread;
        if (args->shared_array[i] != expected_val) {
            errors++;
        }
    }

    return (void*)(long)errors;
}

int test_multithread_dsm_access() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 16000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    const int NUM_THREADS = 4;
    const int ELEMENTS_PER_THREAD = 256;  /* 1KB per thread = 4KB total (1 page) */

    int *shared_array = (int*)dsm_malloc(NUM_THREADS * ELEMENTS_PER_THREAD * sizeof(int));
    if (!shared_array) {
        dsm_finalize();
        return 0;
    }

    pthread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];

    /* Create threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].shared_array = shared_array;
        args[i].start_idx = i * ELEMENTS_PER_THREAD;
        args[i].end_idx = (i + 1) * ELEMENTS_PER_THREAD;
        args[i].thread_id = i;
        args[i].barrier_id = 1000;
        args[i].num_threads = NUM_THREADS;

        pthread_create(&threads[i], NULL, multithread_worker, &args[i]);
    }

    /* Wait for completion */
    int total_errors = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        void *result;
        pthread_join(threads[i], &result);
        int thread_errors = (int)(long)result;
        if (thread_errors > 0) {
            printf("\n  Thread %d reported %d errors\n", i, thread_errors);
        }
        total_errors += thread_errors;
    }

    if (total_errors > 0) {
        printf("\n  Total validation errors: %d\n", total_errors);
        /* Print first few elements for debugging */
        printf("  First 20 elements:\n");
        for (int i = 0; i < 20 && i < NUM_THREADS * ELEMENTS_PER_THREAD; i++) {
            printf("    [%d] = %d\n", i, shared_array[i]);
        }
    }

    dsm_free(shared_array);
    dsm_finalize();

    return total_errors == 0 ? 1 : 0;
}

/**
 * Test 10.1.2: Thread-safety with concurrent page faults
 */
void* concurrent_fault_worker(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;

    /* Trigger page faults by accessing different pages */
    for (int i = args->start_idx; i < args->end_idx; i += 1024) {  /* Every 4KB */
        args->shared_array[i] = args->thread_id;
        /* Small delay to increase chance of concurrent faults */
        usleep(100);
    }

    return NULL;
}

int test_concurrent_page_faults() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 16001,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    const int NUM_THREADS = 4;
    const int PAGES = 8;
    const int INTS_PER_PAGE = PAGE_SIZE / sizeof(int);

    int *shared_array = (int*)dsm_malloc(PAGES * PAGE_SIZE);
    if (!shared_array) {
        dsm_finalize();
        return 0;
    }

    pthread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];

    /* Each thread accesses multiple pages */
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].shared_array = shared_array;
        args[i].start_idx = i * INTS_PER_PAGE;
        args[i].end_idx = PAGES * INTS_PER_PAGE;
        args[i].thread_id = i;

        pthread_create(&threads[i], NULL, concurrent_fault_worker, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    dsm_free(shared_array);
    dsm_finalize();

    return 1;  /* Success if no crash */
}

/* ================================================================
 * Task 10.4: False Sharing Test
 * ================================================================ */

typedef struct {
    int value;
    char padding[60];  /* Pad to 64 bytes to avoid false sharing */
} padded_int_t;

void* false_sharing_worker(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    int *array = args->shared_array;

    /* Repeatedly write to assigned index */
    for (int i = 0; i < 1000; i++) {
        array[args->thread_id]++;
    }

    return NULL;
}

void* padded_worker(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    padded_int_t *array = (padded_int_t*)args->shared_array;

    /* Repeatedly write to assigned index */
    for (int i = 0; i < 1000; i++) {
        array[args->thread_id].value++;
    }

    return NULL;
}

int test_false_sharing() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 16002,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    const int NUM_THREADS = 4;
    pthread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];

    printf("\n  Testing FALSE SHARING scenario (adjacent elements)...\n");

    /* Test 1: False sharing - threads write to adjacent ints (same page) */
    int *false_shared = (int*)dsm_malloc(NUM_THREADS * sizeof(int));
    if (!false_shared) {
        dsm_finalize();
        return 0;
    }

    /* Initialize to zero by triggering page faults */
    for (int i = 0; i < NUM_THREADS; i++) {
        false_shared[i] = 0;
    }

    dsm_reset_stats();
    double start = get_time_ms();

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].shared_array = false_shared;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, false_sharing_worker, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    double false_sharing_time = get_time_ms() - start;

    dsm_stats_t stats_false;
    dsm_get_stats(&stats_false);

    printf("    False sharing: %.2f ms, %lu page faults\n",
           false_sharing_time, stats_false.page_faults);

    /* Verify correctness */
    int correct = 1;
    for (int i = 0; i < NUM_THREADS; i++) {
        if (false_shared[i] != 1000) {
            correct = 0;
            break;
        }
    }

    dsm_free(false_shared);

    /* Test 2: Padded version - avoid false sharing */
    printf("  Testing PADDED scenario (no false sharing)...\n");

    padded_int_t *padded = (padded_int_t*)dsm_malloc(NUM_THREADS * sizeof(padded_int_t));
    if (!padded) {
        dsm_finalize();
        return 0;
    }

    /* Initialize to zero by triggering page faults */
    for (int i = 0; i < NUM_THREADS; i++) {
        padded[i].value = 0;
    }

    dsm_reset_stats();
    start = get_time_ms();

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].shared_array = (int*)padded;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, padded_worker, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    double padded_time = get_time_ms() - start;

    dsm_stats_t stats_padded;
    dsm_get_stats(&stats_padded);

    printf("    Padded:        %.2f ms, %lu page faults\n",
           padded_time, stats_padded.page_faults);

    /* Verify correctness */
    for (int i = 0; i < NUM_THREADS; i++) {
        if (padded[i].value != 1000) {
            correct = 0;
            break;
        }
    }

    printf("    Performance impact: %.1fx slower with false sharing\n",
           false_sharing_time / padded_time);

    dsm_free(padded);
    dsm_finalize();

    return correct ? 1 : 0;
}

/* ================================================================
 * Task 10.5: Consistency Validation
 * ================================================================ */

typedef struct {
    int *shared_counter;
    int *sequence_array;
    int num_operations;
    dsm_lock_t *lock;
    int thread_id;
} consistency_args_t;

void* consistency_worker(void *arg) {
    consistency_args_t *args = (consistency_args_t*)arg;

    for (int i = 0; i < args->num_operations; i++) {
        /* Acquire lock */
        dsm_lock_acquire(args->lock);

        /* Read current counter */
        int current = *args->shared_counter;

        /* Record sequence */
        args->sequence_array[current] = args->thread_id;

        /* Increment counter */
        *args->shared_counter = current + 1;

        /* Release lock */
        dsm_lock_release(args->lock);
    }

    return NULL;
}

int test_consistency_validation() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 16003,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    const int NUM_THREADS = 4;
    const int OPS_PER_THREAD = 100;

    /* Allocate shared memory for counter and sequence in one allocation */
    size_t total_size = sizeof(int) + (NUM_THREADS * OPS_PER_THREAD * sizeof(int));
    int *shared_mem = (int*)dsm_malloc(total_size);
    if (!shared_mem) {
        dsm_finalize();
        return 0;
    }

    int *shared_counter = &shared_mem[0];
    int *sequence = &shared_mem[1];

    *shared_counter = 0;
    /* Initialize sequence array by triggering page faults */
    for (int i = 0; i < NUM_THREADS * OPS_PER_THREAD; i++) {
        sequence[i] = -1;
    }

    dsm_lock_t *lock = dsm_lock_create(2000);
    if (!lock) {
        dsm_free(shared_mem);
        dsm_finalize();
        return 0;
    }

    pthread_t threads[NUM_THREADS];
    consistency_args_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].shared_counter = shared_counter;
        args[i].sequence_array = sequence;
        args[i].num_operations = OPS_PER_THREAD;
        args[i].lock = lock;
        args[i].thread_id = i;

        pthread_create(&threads[i], NULL, consistency_worker, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Validate consistency */
    int valid = 1;
    int expected_total = NUM_THREADS * OPS_PER_THREAD;

    /* Check final counter value */
    if (*shared_counter != expected_total) {
        printf("\n    Counter mismatch: got %d, expected %d\n",
               *shared_counter, expected_total);
        valid = 0;
    }

    /* Check sequence has no gaps */
    for (int i = 0; i < expected_total; i++) {
        if (sequence[i] < 0 || sequence[i] >= NUM_THREADS) {
            printf("\n    Invalid sequence at index %d: %d\n", i, sequence[i]);
            valid = 0;
            break;
        }
    }

    dsm_lock_destroy(lock);
    dsm_free(shared_mem);
    dsm_finalize();

    return valid ? 1 : 0;
}

/* ================================================================
 * Task 10.6: Performance Profiling
 * ================================================================ */

int test_performance_profiling() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 16004,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    printf("\n  === Performance Profiling ===\n");

    const int NUM_PAGES = 10;
    int *memory = (int*)dsm_malloc(NUM_PAGES * PAGE_SIZE);
    if (!memory) {
        dsm_finalize();
        return 0;
    }

    int ints_per_page = PAGE_SIZE / sizeof(int);

    dsm_reset_stats();

    /* Measure sequential access (page fault latency) */
    double start = get_time_ms();
    for (int i = 0; i < NUM_PAGES; i++) {
        memory[i * ints_per_page] = i;  /* Trigger fault for each page */
    }
    double seq_time = get_time_ms() - start;

    dsm_stats_t stats;
    dsm_get_stats(&stats);

    double avg_fault_latency = seq_time / stats.page_faults;

    printf("    Pages accessed:        %d\n", NUM_PAGES);
    printf("    Total page faults:     %lu\n", stats.page_faults);
    printf("    Total time:            %.2f ms\n", seq_time);
    printf("    Avg fault latency:     %.2f ms\n", avg_fault_latency);

    /* Measure random access */
    dsm_reset_stats();
    start = get_time_ms();

    for (int i = 0; i < 100; i++) {
        int page = rand() % NUM_PAGES;
        int offset = rand() % ints_per_page;
        memory[page * ints_per_page + offset] = i;
    }

    double random_time = get_time_ms() - start;
    dsm_get_stats(&stats);

    printf("    Random accesses:       100\n");
    printf("    Page faults:           %lu\n", stats.page_faults);
    printf("    Time:                  %.2f ms\n", random_time);

    dsm_free(memory);
    dsm_finalize();

    return 1;
}

/* ================================================================
 * Main Test Runner
 * ================================================================ */

int main() {
    printf("\n=== DSM Integration Tests (Day 10) ===\n\n");

    printf("--- Task 10.1: Single-Node Multi-Threaded Tests ---\n");
    RUN_TEST(test_multithread_dsm_access);
    RUN_TEST(test_concurrent_page_faults);

    printf("\n--- Task 10.4: False Sharing Test ---\n");
    RUN_TEST(test_false_sharing);

    printf("\n--- Task 10.5: Consistency Validation ---\n");
    RUN_TEST(test_consistency_validation);

    printf("\n--- Task 10.6: Performance Profiling ---\n");
    RUN_TEST(test_performance_profiling);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    printf("\nNote: Tasks 10.2 and 10.3 (multi-node tests) require actual\n");
    printf("      network setup with multiple physical/virtual machines.\n");

    return tests_failed > 0 ? 1 : 0;
}
