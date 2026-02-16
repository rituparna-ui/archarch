/*
 * System call interface.
 *
 * User programs at EL0 invoke `svc #0` with:
 *   x8 = syscall number
 *   x0-x5 = arguments
 *
 * Return value in x0.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/* Syscall numbers */
#define SYS_WRITE   0   /* write(buf, len) → bytes written */
#define SYS_GETPID  1   /* getpid() → task id */
#define SYS_EXIT    2   /* exit(code) → does not return */
#define SYS_YIELD   3   /* yield() → 0 */
#define SYS_SBRK    4   /* sbrk(increment) → old break, or -1 */
#define SYS_EXEC    5   /* exec(filename, len) → child tid, or -1 */
#define SYS_READ    6   /* read(buf, maxlen) → bytes read */
#define SYS_WAIT    7   /* wait(tid) → exit code of child, blocks until done */
#define SYS_LISTDIR 8   /* listdir(buf, buflen) → bytes written to buf */

/*
 * Called from the synchronous exception vector when ESR_EL1
 * indicates an SVC from AArch64 EL0.
 *
 * regs points to the saved register frame on the kernel stack:
 *   regs[0] = x0, regs[1] = x1, ..., regs[30] = x30
 *   regs[31] = saved SP_EL0
 *   regs[32] = saved ELR_EL1
 *   regs[33] = saved SPSR_EL1
 *
 * The return value is placed in regs[0] (x0).
 */
void syscall_handler(uint64_t *regs);

#endif
