/*
 * System call handler (EL1 side).
 *
 * Called from the synchronous exception vector when EL0 executes SVC #0.
 * Dispatches based on syscall number in x8.
 */
#include "syscall.h"
#include "uart.h"
#include "timer.h"
#include "sched.h"

/* SYS_WRITE: write a string to UART from userspace buffer */
static uint64_t sys_write(uint64_t buf, uint64_t len) {
    const char *s = (const char *)buf;
    for (uint64_t i = 0; i < len; i++)
        uart_putc(s[i]);
    return len;
}

/* SYS_GETTIME: return milliseconds since boot */
static uint64_t sys_gettime(void) {
    return timer_ms();
}

/* SYS_YIELD: voluntary yield */
static uint64_t sys_yield(void) {
    sched_yield();
    return 0;
}

/* SYS_EXIT: terminate current task */
static uint64_t sys_exit(uint64_t code) {
    (void)code;
    sched_exit();
    /* never reached */
    return 0;
}

/* SYS_GETPID: return current task ID */
static uint64_t sys_getpid(void) {
    return (uint64_t)sched_current_id();
}

/* SYS_SLEEP: busy-wait for approximately ms milliseconds */
static uint64_t sys_sleep(uint64_t ms) {
    uint64_t start = timer_ms();
    while (timer_ms() - start < ms)
        sched_yield();
    return 0;
}

uint64_t syscall_handler(uint64_t x0, uint64_t x1, uint64_t x2,
                         uint64_t x3, uint64_t x4, uint64_t x5,
                         uint64_t x8)
{
    (void)x2; (void)x3; (void)x4; (void)x5;

    switch (x8) {
    case SYS_WRITE:   return sys_write(x0, x1);
    case SYS_GETTIME: return sys_gettime();
    case SYS_YIELD:   return sys_yield();
    case SYS_EXIT:    return sys_exit(x0);
    case SYS_GETPID:  return sys_getpid();
    case SYS_SLEEP:   return sys_sleep(x0);
    default:
        uart_puts("[SYSCALL] Unknown syscall ");
        uart_putdec(x8);
        uart_puts("\n");
        return (uint64_t)-1;
    }
}
