/*
 * System call interface.
 *
 * Syscalls are invoked from EL0 via SVC #0.
 * The syscall number is passed in x8, arguments in x0-x5.
 * Return value goes in x0.
 *
 * The Synchronous exception vector for Lower EL (AArch64)
 * in start.S saves state, calls syscall_handler(), and
 * restores state before eret back to EL0.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/* Syscall numbers */
#define SYS_WRITE    0   /* write(buf, len) -> bytes written */
#define SYS_GETTIME  1   /* gettime() -> ms since boot */
#define SYS_YIELD    2   /* yield() -> 0 */
#define SYS_EXIT     3   /* exit(code) -> does not return */
#define SYS_GETPID   4   /* getpid() -> task id */
#define SYS_SLEEP    5   /* sleep(ms) -> 0 */

#define NR_SYSCALLS  6

/*
 * Called from the assembly exception vector.
 * x0-x5 are the syscall arguments, x8 is the syscall number.
 * Returns the value to place in x0 on return to EL0.
 */
uint64_t syscall_handler(uint64_t x0, uint64_t x1, uint64_t x2,
                         uint64_t x3, uint64_t x4, uint64_t x5,
                         uint64_t x8);

#endif
