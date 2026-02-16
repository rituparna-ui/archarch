/*
 * System call dispatch.
 *
 * Handles SVC exceptions from EL0 user programs.
 */
#include "syscall.h"
#include "uart.h"
#include "sched.h"
#include "pmm.h"
#include "fat16.h"
#include "kmalloc.h"

/* Global filesystem — set by kernel_main after mounting */
extern struct fat16_fs root_fs;

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
        /* exit(code) — terminate current task and switch to next */
        uart_puts("[SYSCALL] Task ");
        uart_putdec((uint64_t)sched_current_id());
        uart_puts(" exit(");
        uart_putdec(arg0);
        uart_puts(")\n");
        sched_exit();
        /* sched_exit switches away and never returns here */
        __builtin_unreachable();
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

    case SYS_EXEC: {
        /* exec(filename_ptr, filename_len) — load and run from disk */
        const char *name = (const char *)arg0;
        uint32_t name_len = (uint32_t)arg1;

        char fname[32];
        if (name_len > 30) name_len = 30;
        for (uint32_t i = 0; i < name_len; i++) fname[i] = name[i];
        fname[name_len] = '\0';

        uart_puts("[EXEC] Loading \"");
        uart_puts(fname);
        uart_puts("\"...\n");

        struct fat16_file file;
        if (fat16_open(&root_fs, fname, &file) < 0) {
            uart_puts("[EXEC] File not found\n");
            regs[0] = (uint64_t)-1;
            break;
        }

        void *code = kmalloc(file.file_size);
        if (!code) {
            regs[0] = (uint64_t)-1;
            break;
        }

        int bytes = fat16_read_file(&root_fs, &file, code, file.file_size);
        if (bytes < 0) {
            kfree(code);
            regs[0] = (uint64_t)-1;
            break;
        }

        uart_puts("[EXEC] Loaded ");
        uart_putdec((uint64_t)bytes);
        uart_puts(" bytes\n");

        int tid = sched_create_user(fname, code, (uint32_t)bytes);
        kfree(code);
        regs[0] = (tid < 0) ? (uint64_t)-1 : (uint64_t)tid;
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
