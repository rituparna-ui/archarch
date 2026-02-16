/*
 * User process support.
 *
 * Creates a user-mode task that runs at EL0 with its own stack.
 * The kernel identity-maps everything, so user code can access
 * the same physical addresses — but AP bits restrict what EL0
 * can touch (only pages marked AP_RW_ALL or AP_RO_ALL).
 *
 * For this first implementation we keep it simple:
 *   - User code is copied into dynamically allocated pages
 *   - User gets its own stack (also allocated pages)
 *   - We use the existing kernel page tables but mark user
 *     code/stack pages as EL0-accessible
 *   - Drop to EL0 via eret
 */
#ifndef USER_H
#define USER_H

#include "types.h"

/* User stack size */
#define USER_STACK_PAGES  4   /* 16KB */

/*
 * Create and run a user-mode task.
 *
 * code: pointer to user program binary (position-independent)
 * code_size: size in bytes
 * name: task name for debug output
 *
 * This function:
 *   1. Allocates pages for user code + stack
 *   2. Copies the code into the allocated pages
 *   3. Drops to EL0 via eret
 *
 * Does not return (the user program must call SYS_EXIT).
 */
void user_exec(const void *code, uint32_t code_size, const char *name);

/*
 * Helper: drop to EL0 at the given entry point with the given
 * user stack pointer. Called from user_exec.
 */
void drop_to_el0(uintptr_t entry, uintptr_t user_sp);

#endif
