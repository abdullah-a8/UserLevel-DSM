/**
 * @file test_page_table.c
 * @brief Unit tests for page table
 */

#include "../src/memory/page_table.h"
#include "../src/core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>

#define TEST_SIZE (16 * PAGE_SIZE)

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

int test_create_destroy(void) {
    void *base = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return 0;
    }

    page_table_t *table = page_table_create(base, TEST_SIZE);
    if (!table) {
        munmap(base, TEST_SIZE);
        return 0;
    }

    if (table->num_pages != 16) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    page_table_destroy(table);
    munmap(base, TEST_SIZE);
    return 1;
}

int test_lookup_by_addr(void) {
    void *base = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return 0;
    }

    page_table_t *table = page_table_create(base, TEST_SIZE);
    if (!table) {
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Test first page */
    page_entry_t *entry = page_table_lookup_by_addr(table, base);
    if (!entry || entry->id != 0) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Test middle page */
    void *middle = (char*)base + (8 * PAGE_SIZE);
    entry = page_table_lookup_by_addr(table, middle);
    if (!entry || entry->id != 8) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Test out of range */
    void *out = (char*)base + (20 * PAGE_SIZE);
    entry = page_table_lookup_by_addr(table, out);
    if (entry != NULL) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    page_table_destroy(table);
    munmap(base, TEST_SIZE);
    return 1;
}

int test_lookup_by_id(void) {
    void *base = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return 0;
    }

    page_table_t *table = page_table_create(base, TEST_SIZE);
    if (!table) {
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Test valid IDs */
    for (page_id_t i = 0; i < 16; i++) {
        page_entry_t *entry = page_table_lookup_by_id(table, i);
        if (!entry || entry->id != i) {
            page_table_destroy(table);
            munmap(base, TEST_SIZE);
            return 0;
        }
    }

    /* Test invalid ID */
    page_entry_t *entry = page_table_lookup_by_id(table, 100);
    if (entry != NULL) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    page_table_destroy(table);
    munmap(base, TEST_SIZE);
    return 1;
}

int test_set_owner(void) {
    void *base = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return 0;
    }

    page_table_t *table = page_table_create(base, TEST_SIZE);
    if (!table) {
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Set owner */
    int rc = page_table_set_owner(table, 5, 42);
    if (rc != DSM_SUCCESS) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Verify */
    page_entry_t *entry = page_table_lookup_by_id(table, 5);
    if (!entry || entry->owner != 42) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    page_table_destroy(table);
    munmap(base, TEST_SIZE);
    return 1;
}

int test_set_state(void) {
    void *base = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return 0;
    }

    page_table_t *table = page_table_create(base, TEST_SIZE);
    if (!table) {
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Initial state should be INVALID */
    page_entry_t *entry = page_table_lookup_by_id(table, 3);
    if (!entry || entry->state != PAGE_STATE_INVALID) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Set to READ_ONLY */
    int rc = page_table_set_state(table, 3, PAGE_STATE_READ_ONLY);
    if (rc != DSM_SUCCESS) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    /* Verify */
    entry = page_table_lookup_by_id(table, 3);
    if (!entry || entry->state != PAGE_STATE_READ_ONLY) {
        page_table_destroy(table);
        munmap(base, TEST_SIZE);
        return 0;
    }

    page_table_destroy(table);
    munmap(base, TEST_SIZE);
    return 1;
}

int test_page_addr_helpers(void) {
    void *base = (void*)0x100000;

    /* Test page_get_base_addr */
    void *addr1 = (void*)0x100500;
    void *base1 = page_get_base_addr(addr1);
    if (base1 != base) {
        return 0;
    }

    void *addr2 = (void*)0x100FFF;
    void *base2 = page_get_base_addr(addr2);
    if (base2 != base) {
        return 0;
    }

    return 1;
}

int main(void) {
    log_init(LOG_LEVEL_ERROR);

    printf("=== Page Table Unit Tests ===\n\n");

    RUN_TEST(test_create_destroy);
    RUN_TEST(test_lookup_by_addr);
    RUN_TEST(test_lookup_by_id);
    RUN_TEST(test_set_owner);
    RUN_TEST(test_set_state);
    RUN_TEST(test_page_addr_helpers);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
