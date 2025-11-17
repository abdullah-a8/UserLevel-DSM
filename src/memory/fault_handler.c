/**
 * @file fault_handler.c
 * @brief Page fault handler implementation
 */

#define _GNU_SOURCE
#include "fault_handler.h"
#include "page_table.h"
#include "permission.h"
#include "../core/log.h"
#include "../core/dsm_context.h"
#include "../consistency/page_migration.h"
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <ucontext.h>

static struct sigaction old_sa;

static void dsm_fault_handler(int sig, siginfo_t *info, void *context);

int install_fault_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = dsm_fault_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGSEGV, &sa, &old_sa) != 0) {
        LOG_ERROR("Failed to install SIGSEGV handler");
        return DSM_ERROR_INIT;
    }

    LOG_INFO("SIGSEGV handler installed");
    return DSM_SUCCESS;
}

void uninstall_fault_handler(void) {
    sigaction(SIGSEGV, &old_sa, NULL);
    LOG_INFO("SIGSEGV handler uninstalled");
}

static void dsm_fault_handler(int sig, siginfo_t *info, void *context) {
    (void)sig;

    void *fault_addr = info->si_addr;
    long tid = syscall(SYS_gettid);

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized || ctx->num_allocations == 0) {
        LOG_ERROR("[%ld] Fault at %p - DSM not initialized or no allocations", tid, fault_addr);
        /* Re-raise signal to get default behavior */
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
        return;
    }

    /* Check if fault is in any DSM region */
    page_entry_t *entry = NULL;

    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_addr(ctx->page_tables[i], fault_addr);
            if (entry) {
                break;
            }
        }
    }

    if (!entry) {
        LOG_ERROR("[%ld] Fault at %p - not in any DSM region", tid, fault_addr);
        /* Not our fault, re-raise */
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
        return;
    }

    /* Update stats */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.page_faults++;
    pthread_mutex_unlock(&ctx->stats_lock);

    /* Detect if this was a read or write fault using architecture-specific error code */
    bool is_write_fault = false;

#if defined(__x86_64__) || defined(__i386__)
    /* On x86/x86_64, the error code is in ucontext */
    if (context) {
        ucontext_t *uctx = (ucontext_t *)context;
        /* Bit 1 (0x2) indicates write fault, bit 0 (0x1) indicates protection fault */
        unsigned long err = uctx->uc_mcontext.gregs[REG_ERR];
        is_write_fault = (err & 0x2) != 0;
        LOG_DEBUG("[%ld] Page fault at %p (page_id=%lu, state=%d, err=0x%lx, %s)",
                  tid, fault_addr, entry->id, entry->state, err,
                  is_write_fault ? "WRITE" : "READ");
    } else {
        /* No context, conservatively assume write fault */
        is_write_fault = true;
        LOG_DEBUG("[%ld] Page fault at %p (page_id=%lu, state=%d, assuming WRITE)",
                  tid, fault_addr, entry->id, entry->state);
    }
#elif defined(__aarch64__)
    /* On ARM64, would use different mechanism - ESR_EL1 register */
    /* For now, conservatively assume write fault */
    is_write_fault = true;
    LOG_DEBUG("[%ld] Page fault at %p (page_id=%lu, state=%d, ARM64 assuming WRITE)",
              tid, fault_addr, entry->id, entry->state);
#else
    /* Unknown architecture, conservatively assume write fault */
    is_write_fault = true;
    LOG_DEBUG("[%ld] Page fault at %p (page_id=%lu, state=%d, unknown arch assuming WRITE)",
              tid, fault_addr, entry->id, entry->state);
#endif

    /* Handle fault based on type */
    int rc;
    if (is_write_fault) {
        rc = handle_write_fault(fault_addr);
    } else {
        rc = handle_read_fault(fault_addr);
    }

    if (rc != DSM_SUCCESS) {
        LOG_ERROR("[%ld] Failed to handle %s fault at %p",
                  tid, is_write_fault ? "write" : "read", fault_addr);
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
    }
}

int handle_read_fault(void *addr) {
    dsm_context_t *ctx = dsm_get_context();

    /* Find entry in any page table */
    page_entry_t *entry = NULL;
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_addr(ctx->page_tables[i], addr);
            if (entry) break;
        }
    }

    if (!entry) {
        return DSM_ERROR_NOT_FOUND;
    }

    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.read_faults++;
    pthread_mutex_unlock(&ctx->stats_lock);

    /* State transition: INVALID -> READ_ONLY */
    if (entry->state == PAGE_STATE_INVALID) {
        /* Fetch page from remote owner */
        int rc = fetch_page_read(entry->id);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to fetch page %lu for read", entry->id);
            return rc;
        }
        LOG_DEBUG("Read fault: page %lu fetched and set to READ_ONLY", entry->id);
    } else if (entry->state == PAGE_STATE_READ_ONLY) {
        /* Already have read access, this shouldn't happen */
        LOG_WARN("Read fault on page %lu which is already READ_ONLY", entry->id);
    }

    return DSM_SUCCESS;
}

int handle_write_fault(void *addr) {
    dsm_context_t *ctx = dsm_get_context();

    /* Find entry in any page table */
    page_entry_t *entry = NULL;
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_addr(ctx->page_tables[i], addr);
            if (entry) break;
        }
    }

    if (!entry) {
        return DSM_ERROR_NOT_FOUND;
    }

    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.write_faults++;
    pthread_mutex_unlock(&ctx->stats_lock);

    /* State transitions:
     * INVALID -> READ_WRITE
     * READ_ONLY -> READ_WRITE
     */
    if (entry->state == PAGE_STATE_INVALID || entry->state == PAGE_STATE_READ_ONLY) {
        /* Fetch page with write access (will invalidate remote copies) */
        int rc = fetch_page_write(entry->id);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to fetch page %lu for write", entry->id);
            return rc;
        }
        LOG_DEBUG("Write fault: page %lu fetched and set to READ_WRITE", entry->id);
    } else if (entry->state == PAGE_STATE_READ_WRITE) {
        /* Already have write access, this shouldn't happen */
        LOG_WARN("Write fault on page %lu which is already READ_WRITE", entry->id);
    }

    return DSM_SUCCESS;
}
