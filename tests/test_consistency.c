/**
 * @file test_consistency.c
 * @brief Test page directory and migration functionality
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "dsm/types.h"
#include "../src/consistency/directory.h"
#include "../src/consistency/page_migration.h"

#define NUM_TEST_PAGES 10

void test_directory_create(void) {
    printf("Testing directory_create()...\n");

    page_directory_t *dir = directory_create(NUM_TEST_PAGES);
    assert(dir != NULL);
    assert(dir->num_entries == NUM_TEST_PAGES);

    directory_destroy(dir);
    printf("  ✓ directory_create passed\n");
}

void test_directory_lookup(void) {
    printf("Testing directory_lookup()...\n");

    page_directory_t *dir = directory_create(NUM_TEST_PAGES);
    assert(dir != NULL);

    node_id_t owner;
    int rc = directory_lookup(dir, 0, &owner);
    assert(rc == DSM_SUCCESS);
    /* Initial owner is (node_id_t)-1 until pages are allocated */
    assert(owner == (node_id_t)-1);

    directory_destroy(dir);
    printf("  ✓ directory_lookup passed\n");
}

void test_directory_add_reader(void) {
    printf("Testing directory_add_reader()...\n");

    page_directory_t *dir = directory_create(NUM_TEST_PAGES);
    assert(dir != NULL);

    /* Add node 1 as reader for page 0 */
    int rc = directory_add_reader(dir, 0, 1);
    assert(rc == DSM_SUCCESS);

    /* Verify sharer was added */
    node_id_t sharers[MAX_SHARERS];
    int count;
    rc = directory_get_sharers(dir, 0, sharers, &count);
    assert(rc == DSM_SUCCESS);
    assert(count == 1);
    assert(sharers[0] == 1);

    /* Add node 2 as reader */
    rc = directory_add_reader(dir, 0, 2);
    assert(rc == DSM_SUCCESS);

    rc = directory_get_sharers(dir, 0, sharers, &count);
    assert(rc == DSM_SUCCESS);
    assert(count == 2);

    directory_destroy(dir);
    printf("  ✓ directory_add_reader passed\n");
}

void test_directory_set_writer(void) {
    printf("Testing directory_set_writer()...\n");

    page_directory_t *dir = directory_create(NUM_TEST_PAGES);
    assert(dir != NULL);

    /* Add some readers */
    directory_add_reader(dir, 0, 1);
    directory_add_reader(dir, 0, 2);
    directory_add_reader(dir, 0, 3);

    /* Set node 4 as writer */
    node_id_t invalidate_list[MAX_SHARERS];
    int num_invalidate;
    int rc = directory_set_writer(dir, 0, 4, invalidate_list, &num_invalidate);
    assert(rc == DSM_SUCCESS);

    /* Should have invalidated all previous readers and owner */
    assert(num_invalidate >= 3);  /* At least nodes 1, 2, 3 */

    /* Verify owner changed */
    node_id_t owner;
    rc = directory_lookup(dir, 0, &owner);
    assert(rc == DSM_SUCCESS);
    assert(owner == 4);

    /* Verify sharers list cleared */
    node_id_t sharers[MAX_SHARERS];
    int count;
    rc = directory_get_sharers(dir, 0, sharers, &count);
    assert(rc == DSM_SUCCESS);
    assert(count == 0);

    directory_destroy(dir);
    printf("  ✓ directory_set_writer passed\n");
}

void test_directory_remove_sharer(void) {
    printf("Testing directory_remove_sharer()...\n");

    page_directory_t *dir = directory_create(NUM_TEST_PAGES);
    assert(dir != NULL);

    /* Add some readers */
    directory_add_reader(dir, 0, 1);
    directory_add_reader(dir, 0, 2);
    directory_add_reader(dir, 0, 3);

    /* Remove node 2 */
    int rc = directory_remove_sharer(dir, 0, 2);
    assert(rc == DSM_SUCCESS);

    /* Verify node 2 was removed */
    node_id_t sharers[MAX_SHARERS];
    int count;
    rc = directory_get_sharers(dir, 0, sharers, &count);
    assert(rc == DSM_SUCCESS);
    assert(count == 2);

    /* Check that node 2 is not in list */
    int found_node_2 = 0;
    for (int i = 0; i < count; i++) {
        if (sharers[i] == 2) {
            found_node_2 = 1;
            break;
        }
    }
    assert(found_node_2 == 0);

    directory_destroy(dir);
    printf("  ✓ directory_remove_sharer passed\n");
}

void test_consistency_init(void) {
    printf("Testing consistency_init()...\n");

    int rc = consistency_init(NUM_TEST_PAGES);
    assert(rc == DSM_SUCCESS);

    /* Verify directory was created */
    page_directory_t *dir = get_page_directory();
    assert(dir != NULL);

    consistency_cleanup();
    printf("  ✓ consistency_init passed\n");
}

int main(void) {
    printf("=================================\n");
    printf("  DSM Consistency Module Tests\n");
    printf("=================================\n\n");

    test_directory_create();
    test_directory_lookup();
    test_directory_add_reader();
    test_directory_set_writer();
    test_directory_remove_sharer();
    test_consistency_init();

    printf("\n=================================\n");
    printf("  All tests passed! ✓\n");
    printf("=================================\n");

    return 0;
}
