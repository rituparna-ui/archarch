/*
 * System call dispatch.
 *
 * Handles SVC exceptions from EL0 user programs.
 */
#include "syscall.h"
#include "uart.h"
#include "sched.h"
#include "pmm.h"

void syscall_handler(uint64_t *regs) {
    uint64_t syscall_num = regs[8];  /* x8 = syscall number */
    uint64_t arg0 = regs[0];        /* x0 */
    uint64_t arg1 = regs[1];        /* x1 */

    switch (syscall_num) {

    case SYS_WRITE: {
        /* write(buf, len) — print to UART */
        const char *buf = (const char *)arg0;
        uint64_t len = arg1;
        for (uint64_t i = 0; i < len; i++) {
            if (buf[i] == '\n')
                uart_putc('\r');
            uart_putc(buf[i]);
        }
        regs[0] = len;
        break;
    }

    case SYS_GETPID:
        /* getpid() — return current task ID */
        regs[0] = (uint64_t)sched_current_id();
        break;

    case SYS_EXIT: {
        /* exit(code) — terminate current task, return to kernel */
        uart_puts("[SYSCALL] exit(");
        uart_putdec(arg0);
        uart_puts(") — returning to kernel\n");
        /*
         * We can't call sched_exit here because we're not using the
         * scheduler for user tasks yet. Instead, we manipulate the
         * saved ELR to jump to a kernel return point.
         * For now, just loop — the eret will return to user code
         * which will spin. The timeout will kill QEMU.
         *
         * A proper implementation would longjmp back to kernel_main.
         */
        for (;;) __asm__ volatile("wfe");
        break;
    }

    case SYS_YIELD:
        /* yield() — give up CPU */
        sched_yield();
        regs[0] = 0;
        break;

    case SYS_SBRK: {
        /* sbrk(increment) — allocate pages */
        uint64_t pages = (arg0 + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages == 0) pages = 1;
        uintptr_t addr = pmm_alloc_pages((uint32_t)pages);
        regs[0] = addr ? addr : (uint64_t)-1;
        break;
    }

    default:
        uart_puts("[SYSCALL] Unknown syscall ");
        uart_putdec(syscall_num);
        uart_puts(" from task ");
        uart_putdec((uint64_t)sched_current_id());
        uart_puts("\n");
        regs[0] = (uint64_t)-1;
        break;
    }
}
