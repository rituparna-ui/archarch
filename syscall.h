/*
 * System call interface — Unix-style with file descriptors.
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
#define SYS_READ    0   /* read(fd, buf, count) → bytes read */
#define SYS_WRITE   1   /* write(fd, buf, count) → bytes written */
#define SYS_OPEN    2   /* open(path, flags) → fd */
#define SYS_CLOSE   3   /* close(fd) → 0 */
#define SYS_GETPID  4   /* getpid() → pid */
#define SYS_EXIT    5   /* exit(code) → does not return */
#define SYS_YIELD   6   /* yield() → 0 */
#define SYS_EXEC    7   /* exec(filename, len) → child tid */
#define SYS_WAIT    8   /* wait(tid) → 0 */
#define SYS_SBRK    9   /* sbrk(increment) → addr */
#define SYS_LISTDIR 10  /* listdir(buf, buflen) → bytes written */

void syscall_handler(uint64_t *regs);

#endif
