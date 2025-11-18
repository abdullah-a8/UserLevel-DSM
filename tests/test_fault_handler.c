/**
 * @file test_fault_handler.c
 * @brief Page fault handler tests
 */

#include "dsm/dsm.h"
#include "../src/core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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

int test_write_fault(void) {
    dsm_config_t config = {
        .node_id = 0,
        .port = 5000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);

    /* Allocate memory */
    int *data = dsm_malloc(PAGE_SIZE);
    if (!data) {
        dsm_finalize();
        return 0;
    }

    /* Write should trigger fault and upgrade to READ_WRITE */
    data[0] = 42;

    /* Read should work */
    if (data[0] != 42) {
        dsm_free(data);
        dsm_finalize();
        return 0;
    }

    /* Check stats */
    dsm_stats_t stats;
    dsm_get_stats(&stats);
    if (stats.page_faults == 0) {
        dsm_free(data);
        dsm_finalize();
        return 0;
    }

    dsm_free(data);
    dsm_finalize();
    return 1;
}

int test_multiple_pages(void) {
    dsm_config_t config = {
        .node_id = 0,
        .port = 5000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);
    dsm_reset_stats();

    /* Allocate multiple pages */
    size_t num_pages = 4;
    int *data = dsm_malloc(num_pages * PAGE_SIZE);
    if (!data) {
        dsm_finalize();
        return 0;
    }

    /* Access each page */
    for (size_t i = 0; i < num_pages; i++) {
        size_t offset = (i * PAGE_SIZE) / sizeof(int);
        data[offset] = (int)i;
    }

    /* Verify */
    for (size_t i = 0; i < num_pages; i++) {
        size_t offset = (i * PAGE_SIZE) / sizeof(int);
        if (data[offset] != (int)i) {
            dsm_free(data);
            dsm_finalize();
            return 0;
        }
    }

    /* Should have faulted on each page */
    dsm_stats_t stats;
    dsm_get_stats(&stats);
    if (stats.page_faults < num_pages) {
        dsm_free(data);
        dsm_finalize();
        return 0;
    }

    dsm_free(data);
    dsm_finalize();
    return 1;
}

typedef struct {
    int *data;
    int thread_id;
} thread_arg_t;

void* thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t*)arg;

    /* Each thread writes to different part of same page */
    targ->data[targ->thread_id] = targ->thread_id + 100;

    return NULL;
}

int test_multithreaded_faults(void) {
    dsm_config_t config = {
        .node_id = 0,
        .port = 5000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);

    int *data = dsm_malloc(PAGE_SIZE);
    if (!data) {
        dsm_finalize();
        return 0;
    }

    /* Create threads */
    pthread_t threads[4];
    thread_arg_t thread_args[4];
    for (int i = 0; i < 4; i++) {
        thread_args[i].data = data;
        thread_args[i].thread_id = i;
        if (pthread_create(&threads[i], NULL, thread_func, &thread_args[i]) != 0) {
            dsm_free(data);
            dsm_finalize();
            return 0;
        }
    }

    /* Wait for threads */
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    dsm_free(data);
    dsm_finalize();
    return 1;
}

int test_sequential_access(void) {
    dsm_config_t config = {
        .node_id = 0,
        .port = 5000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);

    size_t array_size = 1024;
    int *array = dsm_malloc(array_size * sizeof(int));
    if (!array) {
        dsm_finalize();
        return 0;
    }

    /* Sequential write */
    for (size_t i = 0; i < array_size; i++) {
        array[i] = (int)i;
    }

    /* Sequential read */
    for (size_t i = 0; i < array_size; i++) {
        if (array[i] != (int)i) {
            dsm_free(array);
            dsm_finalize();
            return 0;
        }
    }

    dsm_free(array);
    dsm_finalize();
    return 1;
}

int main(void) {
    printf("=== Page Fault Handler Tests ===\n\n");

    RUN_TEST(test_write_fault);
    RUN_TEST(test_multiple_pages);
    RUN_TEST(test_multithreaded_faults);
    RUN_TEST(test_sequential_access);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
