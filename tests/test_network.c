/**
 * @file test_network.c
 * @brief Network layer tests
 */

#include "dsm/dsm.h"
#include "../src/network/network.h"
#include "../src/core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int test_server_init(void) {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15000,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    if (network_server_init(15000) != DSM_SUCCESS) {
        dsm_finalize();
        return 0;
    }

    network_shutdown();
    dsm_finalize();
    return 1;
}

int test_serialization(void) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_PAGE_REQUEST;
    msg.header.sender = 1;
    msg.header.seq_num = 42;
    msg.payload.page_request.page_id = 5;
    msg.payload.page_request.access = ACCESS_WRITE;
    msg.payload.page_request.requester = 1;

    uint8_t buffer[8192];
    size_t len;

    if (serialize_message(&msg, buffer, &len) != DSM_SUCCESS) {
        return 0;
    }

    message_t msg2;
    if (deserialize_message(buffer, len, &msg2) != DSM_SUCCESS) {
        return 0;
    }

    if (msg2.header.type != MSG_PAGE_REQUEST) {
        return 0;
    }

    if (msg2.payload.page_request.page_id != 5) {
        return 0;
    }

    return 1;
}

int test_connect_localhost(void) {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15001,
        .num_nodes = 2,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    if (dsm_init(&config) != DSM_SUCCESS) {
        return 0;
    }

    if (network_server_init(15001) != DSM_SUCCESS) {
        dsm_finalize();
        return 0;
    }

    /* Give server time to start */
    usleep(100000);

    /* Connect to self (localhost) */
    int rc = network_connect_to_node(1, "127.0.0.1", 15001);

    network_shutdown();
    dsm_finalize();

    return rc == DSM_SUCCESS ? 1 : 0;
}

int test_message_roundtrip(void) {
    /* This is a simplified test - full roundtrip requires two processes */
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_INVALIDATE;
    msg.header.sender = 2;
    msg.payload.invalidate.page_id = 10;
    msg.payload.invalidate.new_owner = 3;

    uint8_t buffer[8192];
    size_t len;

    if (serialize_message(&msg, buffer, &len) != DSM_SUCCESS) {
        return 0;
    }

    message_t received;
    if (deserialize_message(buffer, len, &received) != DSM_SUCCESS) {
        return 0;
    }

    if (received.header.type != MSG_INVALIDATE) {
        return 0;
    }

    if (received.payload.invalidate.page_id != 10) {
        return 0;
    }

    return 1;
}

int main(void) {
    printf("=== Network Layer Tests ===\n\n");

    RUN_TEST(test_server_init);
    RUN_TEST(test_serialization);
    RUN_TEST(test_connect_localhost);
    RUN_TEST(test_message_roundtrip);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
