/**
 * @file test_memory.c
 * @brief Memory allocation and permission tests
 */

#include "dsm/dsm.h"
#include "../src/core/log.h"
#include "../src/memory/permission.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int test_dsm_init(void) {
    dsm_config_t config = {
        .node_id = 1,
        .port = 5000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    int rc = dsm_init(&config);
    if (rc != DSM_SUCCESS) {
        return 0;
    }

    if (dsm_get_node_id() != 1) {
        dsm_finalize();
        return 0;
    }

    dsm_finalize();
    return 1;
}

int test_dsm_malloc_free(void) {
    dsm_config_t config = {
        .node_id = 1,
        .port = 5000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);

    /* Allocate memory */
    size_t size = 16 * PAGE_SIZE;
    void *ptr = dsm_malloc(size);
    if (!ptr) {
        dsm_finalize();
        return 0;
    }

    /* Free memory */
    int rc = dsm_free(ptr);
    if (rc != DSM_SUCCESS) {
        dsm_finalize();
        return 0;
    }

    dsm_finalize();
    return 1;
}

int test_page_permissions(void) {
    dsm_config_t config = {
        .node_id = 1,
        .port = 5000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);

    void *ptr = dsm_malloc(4 * PAGE_SIZE);
    if (!ptr) {
        dsm_finalize();
        return 0;
    }

    /* Test setting to READ_ONLY */
    int rc = set_page_permission(ptr, PAGE_PERM_READ);
    if (rc != DSM_SUCCESS) {
        dsm_free(ptr);
        dsm_finalize();
        return 0;
    }

    /* Test setting to READ_WRITE */
    rc = set_page_permission(ptr, PAGE_PERM_READ_WRITE);
    if (rc != DSM_SUCCESS) {
        dsm_free(ptr);
        dsm_finalize();
        return 0;
    }

    /* Test setting to NONE */
    rc = set_page_permission(ptr, PAGE_PERM_NONE);
    if (rc != DSM_SUCCESS) {
        dsm_free(ptr);
        dsm_finalize();
        return 0;
    }

    dsm_free(ptr);
    dsm_finalize();
    return 1;
}

int test_stats(void) {
    dsm_config_t config = {
        .node_id = 1,
        .port = 5000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);

    dsm_stats_t stats;
    int rc = dsm_get_stats(&stats);
    if (rc != DSM_SUCCESS) {
        dsm_finalize();
        return 0;
    }

    /* Initial stats should be zero */
    if (stats.page_faults != 0) {
        dsm_finalize();
        return 0;
    }

    /* Reset stats */
    rc = dsm_reset_stats();
    if (rc != DSM_SUCCESS) {
        dsm_finalize();
        return 0;
    }

    dsm_finalize();
    return 1;
}

int main(void) {
    printf("=== Memory Management Tests ===\n\n");

    RUN_TEST(test_dsm_init);
    RUN_TEST(test_dsm_malloc_free);
    RUN_TEST(test_page_permissions);
    RUN_TEST(test_stats);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
