/**
 * @file test_sync.c
 * @brief Tests for distributed locks and barriers (Day 9)
 */

#include "dsm/dsm.h"
#include "../src/core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

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

/* Global shared counter for lock testing */
static volatile int shared_counter = 0;

/**
 * Thread worker for lock testing
 */
typedef struct {
    dsm_lock_t *lock;
    int iterations;
} lock_thread_args_t;

void* lock_worker_thread(void *arg) {
    lock_thread_args_t *args = (lock_thread_args_t*)arg;

    for (int i = 0; i < args->iterations; i++) {
        /* Acquire lock */
        int rc = dsm_lock_acquire(args->lock);
        if (rc != DSM_SUCCESS) {
            fprintf(stderr, "Failed to acquire lock: %d\n", rc);
            return NULL;
        }

        /* Critical section: increment counter */
        int old_val = shared_counter;
        usleep(100);  /* Simulate some work */
        shared_counter = old_val + 1;

        /* Release lock */
        rc = dsm_lock_release(args->lock);
        if (rc != DSM_SUCCESS) {
            fprintf(stderr, "Failed to release lock: %d\n", rc);
            return NULL;
        }
    }

    return NULL;
}

/**
 * Test 1: Basic lock create/destroy
 */
int test_lock_create_destroy() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15200,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    dsm_lock_t *lock = dsm_lock_create(100);
    if (!lock) {
        dsm_finalize();
        return 0;
    }

    int rc = dsm_lock_destroy(lock);
    dsm_finalize();

    return rc == DSM_SUCCESS ? 1 : 0;
}

/**
 * Test 2: Single-threaded lock acquire/release
 */
int test_lock_single_thread() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15201,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    dsm_lock_t *lock = dsm_lock_create(101);
    if (!lock) {
        dsm_finalize();
        return 0;
    }

    /* Acquire and release multiple times */
    for (int i = 0; i < 5; i++) {
        if (dsm_lock_acquire(lock) != DSM_SUCCESS) {
            dsm_lock_destroy(lock);
            dsm_finalize();
            return 0;
        }

        if (dsm_lock_release(lock) != DSM_SUCCESS) {
            dsm_lock_destroy(lock);
            dsm_finalize();
            return 0;
        }
    }

    dsm_lock_destroy(lock);
    dsm_finalize();
    return 1;
}

/**
 * Test 3: Multi-threaded lock mutual exclusion
 */
int test_lock_mutual_exclusion() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15202,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    dsm_lock_t *lock = dsm_lock_create(102);
    if (!lock) {
        dsm_finalize();
        return 0;
    }

    /* Reset counter */
    shared_counter = 0;

    /* Create multiple threads */
    const int NUM_THREADS = 4;
    const int ITERATIONS = 10;
    pthread_t threads[NUM_THREADS];
    lock_thread_args_t args = {
        .lock = lock,
        .iterations = ITERATIONS
    };

    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, lock_worker_thread, &args) != 0) {
            dsm_lock_destroy(lock);
            dsm_finalize();
            return 0;
        }
    }

    /* Wait for all threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Verify counter */
    int expected = NUM_THREADS * ITERATIONS;
    int success = (shared_counter == expected);

    if (!success) {
        fprintf(stderr, "Counter mismatch: got %d, expected %d\n", shared_counter, expected);
    }

    dsm_lock_destroy(lock);
    dsm_finalize();
    return success ? 1 : 0;
}

/**
 * Test 4: Lock statistics
 */
int test_lock_statistics() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15203,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    dsm_reset_stats();

    dsm_lock_t *lock = dsm_lock_create(103);
    if (!lock) {
        dsm_finalize();
        return 0;
    }

    /* Acquire and release 10 times */
    for (int i = 0; i < 10; i++) {
        dsm_lock_acquire(lock);
        dsm_lock_release(lock);
    }

    dsm_stats_t stats;
    dsm_get_stats(&stats);

    int success = (stats.lock_acquires == 10);
    if (!success) {
        fprintf(stderr, "Lock stat mismatch: got %lu, expected 10\n", stats.lock_acquires);
    }

    dsm_lock_destroy(lock);
    dsm_finalize();
    return success ? 1 : 0;
}

/**
 * Thread worker for barrier testing
 */
typedef struct {
    barrier_id_t barrier_id;
    int num_participants;
    int *counter;
    pthread_mutex_t *counter_lock;
} barrier_thread_args_t;

void* barrier_worker_thread(void *arg) {
    barrier_thread_args_t *args = (barrier_thread_args_t*)arg;

    for (int i = 0; i < 3; i++) {
        /* Do some work */
        pthread_mutex_lock(args->counter_lock);
        (*args->counter)++;
        pthread_mutex_unlock(args->counter_lock);

        /* Wait at barrier */
        int rc = dsm_barrier(args->barrier_id, args->num_participants);
        if (rc != DSM_SUCCESS) {
            fprintf(stderr, "Barrier wait failed: %d\n", rc);
            return NULL;
        }
    }

    return NULL;
}

/**
 * Test 5: Basic barrier synchronization
 */
int test_barrier_basic() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15204,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    const int NUM_THREADS = 4;
    pthread_t threads[NUM_THREADS];
    int counter = 0;
    pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

    barrier_thread_args_t args = {
        .barrier_id = 200,
        .num_participants = NUM_THREADS,
        .counter = &counter,
        .counter_lock = &counter_lock
    };

    /* Create threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, barrier_worker_thread, &args) != 0) {
            dsm_finalize();
            return 0;
        }
    }

    /* Wait for all threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Each thread increments counter 3 times */
    int expected = NUM_THREADS * 3;
    int success = (counter == expected);

    if (!success) {
        fprintf(stderr, "Barrier counter mismatch: got %d, expected %d\n", counter, expected);
    }

    pthread_mutex_destroy(&counter_lock);
    dsm_finalize();
    return success ? 1 : 0;
}

/**
 * Test 6: Barrier statistics
 */
int test_barrier_statistics() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15205,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    dsm_reset_stats();

    /* Wait at barrier 5 times */
    for (int i = 0; i < 5; i++) {
        dsm_barrier(201, 1);
    }

    dsm_stats_t stats;
    dsm_get_stats(&stats);

    int success = (stats.barrier_waits == 5);
    if (!success) {
        fprintf(stderr, "Barrier stat mismatch: got %lu, expected 5\n", stats.barrier_waits);
    }

    dsm_finalize();
    return success ? 1 : 0;
}

/**
 * Test 7: Barrier with different thread counts
 */
int test_barrier_scaling() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15206,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    /* Test with 2, 4, 8 threads */
    int thread_counts[] = {2, 4, 8};
    int success = 1;

    for (size_t test = 0; test < sizeof(thread_counts)/sizeof(thread_counts[0]); test++) {
        int num_threads = thread_counts[test];
        pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
        int counter = 0;
        pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

        barrier_thread_args_t args = {
            .barrier_id = 202 + test,
            .num_participants = num_threads,
            .counter = &counter,
            .counter_lock = &counter_lock
        };

        for (int i = 0; i < num_threads; i++) {
            pthread_create(&threads[i], NULL, barrier_worker_thread, &args);
        }

        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }

        if (counter != num_threads * 3) {
            success = 0;
            fprintf(stderr, "Barrier scaling test failed for %d threads: got %d, expected %d\n",
                    num_threads, counter, num_threads * 3);
        }

        pthread_mutex_destroy(&counter_lock);
        free(threads);
    }

    dsm_finalize();
    return success ? 1 : 0;
}

/**
 * Test 8: Lock and barrier integration
 */
int test_lock_barrier_integration() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15207,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    dsm_lock_t *lock = dsm_lock_create(300);
    if (!lock) {
        dsm_finalize();
        return 0;
    }

    shared_counter = 0;
    const int NUM_THREADS = 4;
    pthread_t threads[NUM_THREADS];

    typedef struct {
        dsm_lock_t *lock;
        barrier_id_t barrier_id;
        int num_participants;
    } integration_args_t;

    integration_args_t args = {
        .lock = lock,
        .barrier_id = 301,
        .num_participants = NUM_THREADS
    };

    /* Thread function combining lock and barrier */
    void* integration_worker(void *arg) {
        integration_args_t *a = (integration_args_t*)arg;

        /* Acquire lock, increment counter, release */
        dsm_lock_acquire(a->lock);
        shared_counter++;
        dsm_lock_release(a->lock);

        /* Wait at barrier */
        dsm_barrier(a->barrier_id, a->num_participants);

        /* Verify counter after barrier */
        assert(shared_counter == NUM_THREADS);

        return NULL;
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, integration_worker, &args);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    int success = (shared_counter == NUM_THREADS);

    dsm_lock_destroy(lock);
    dsm_finalize();
    return success ? 1 : 0;
}

int main() {
    printf("\n=== Distributed Synchronization Tests (Day 9) ===\n\n");

    printf("--- Lock Tests ---\n");
    RUN_TEST(test_lock_create_destroy);
    RUN_TEST(test_lock_single_thread);
    RUN_TEST(test_lock_mutual_exclusion);
    RUN_TEST(test_lock_statistics);

    printf("\n--- Barrier Tests ---\n");
    RUN_TEST(test_barrier_basic);
    RUN_TEST(test_barrier_statistics);
    RUN_TEST(test_barrier_scaling);

    printf("\n--- Integration Tests ---\n");
    RUN_TEST(test_lock_barrier_integration);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
