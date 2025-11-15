/**
 * @file test_simple_fault.c
 * @brief Simple test to debug page fault handling
 */

#include "dsm/dsm.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("=== Simple Page Fault Test ===\n");

    /* Initialize DSM */
    dsm_config_t config = {
        .node_id = 0,
        .port = 15000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = 4  /* DEBUG level */
    };

    printf("Initializing DSM...\n");
    if (dsm_init(&config) != DSM_SUCCESS) {
        printf("Failed to initialize DSM\n");
        return 1;
    }
    printf("DSM initialized\n");

    /* Allocate DSM memory */
    printf("Allocating DSM memory...\n");
    int *data = (int*)dsm_malloc(sizeof(int) * 10);
    if (!data) {
        printf("Failed to allocate DSM memory\n");
        dsm_finalize();
        return 1;
    }
    printf("DSM memory allocated at %p\n", (void*)data);

    /* Try to write to it (should trigger page fault) */
    printf("Writing to DSM memory (this should trigger a page fault)...\n");
    data[0] = 42;
    printf("Write successful! data[0] = %d\n", data[0]);

    /* Try to read from it */
    printf("Reading from DSM memory...\n");
    int value = data[0];
    printf("Read successful! value = %d\n", value);

    /* Cleanup */
    printf("Cleaning up...\n");
    dsm_free(data);
    dsm_finalize();

    printf("=== Test PASSED ===\n");
    return 0;
}

