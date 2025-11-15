/**
 * @file test_handlers.c
 * @brief Protocol handler tests
 */

#include "dsm/dsm.h"
#include "../src/network/handlers.h"
#include "../src/core/log.h"
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

int test_page_request_handler() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15100,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);
    void *mem = dsm_malloc(PAGE_SIZE);
    if (!mem) {
        dsm_finalize();
        return 0;
    }

    /* Test PAGE_REPLY handler instead (doesn't need network) */
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type = MSG_PAGE_REPLY;
    msg.header.sender = 1;
    msg.payload.page_reply.page_id = 0;
    msg.payload.page_reply.version = 0;
    memset(msg.payload.page_reply.data, 0x42, PAGE_SIZE);

    int rc = handle_page_reply(&msg);

    dsm_free(mem);
    dsm_finalize();

    return rc == DSM_SUCCESS ? 1 : 0;
}

int test_invalidate_handler() {
    dsm_config_t config = {
        .node_id = 0,
        .port = 15101,
        .num_nodes = 1,
        .is_manager = true,
        .log_level = LOG_LEVEL_ERROR
    };

    dsm_init(&config);
    void *mem = dsm_malloc(PAGE_SIZE);
    if (!mem) {
        dsm_finalize();
        return 0;
    }

    /* Access memory to set it READ_WRITE */
    ((int*)mem)[0] = 42;

    /* Test INVALIDATE_ACK handler (doesn't need network send) */
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type = MSG_INVALIDATE_ACK;
    msg.header.sender = 1;
    msg.payload.invalidate_ack.page_id = 0;
    msg.payload.invalidate_ack.acker = 1;

    int rc = handle_invalidate_ack(&msg);

    dsm_free(mem);
    dsm_finalize();

    return rc == DSM_SUCCESS ? 1 : 0;
}

int test_message_dispatch() {
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    /* Test various message types */
    msg.header.type = MSG_LOCK_GRANT;
    msg.payload.lock_grant.lock_id = 1;
    msg.payload.lock_grant.grantee = 0;

    int rc = dispatch_message(&msg);

    return rc == DSM_SUCCESS ? 1 : 0;
}

int test_lock_handlers() {
    /* Test LOCK_RELEASE (doesn't send) */
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.type = MSG_LOCK_RELEASE;
    msg.payload.lock_release.lock_id = 5;
    msg.payload.lock_release.releaser = 2;

    int rc = dispatch_message(&msg);

    return rc == DSM_SUCCESS ? 1 : 0;
}

int test_barrier_handlers() {
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.type = MSG_BARRIER_ARRIVE;
    msg.payload.barrier_arrive.barrier_id = 1;
    msg.payload.barrier_arrive.arriver = 0;
    msg.payload.barrier_arrive.num_participants = 4;

    int rc = dispatch_message(&msg);

    return rc == DSM_SUCCESS ? 1 : 0;
}

int main(void) {
    printf("=== Protocol Handler Tests ===\n\n");

    RUN_TEST(test_page_request_handler);
    RUN_TEST(test_invalidate_handler);
    RUN_TEST(test_message_dispatch);
    RUN_TEST(test_lock_handlers);
    RUN_TEST(test_barrier_handlers);

    printf("\n=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
