/**
 * @file fault_handler.h
 * @brief Page fault handler
 */

#ifndef FAULT_HANDLER_H
#define FAULT_HANDLER_H

/**
 * Install SIGSEGV signal handler
 */
int install_fault_handler(void);

/**
 * Uninstall fault handler
 */
void uninstall_fault_handler(void);

/**
 * Handle read fault
 */
int handle_read_fault(void *addr);

/**
 * Handle write fault
 */
int handle_write_fault(void *addr);

#endif /* FAULT_HANDLER_H */
