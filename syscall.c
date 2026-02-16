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
#include "virtio_blk.h"

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

    case SYS_READ: {
        /* read(buf, maxlen) — read line from UART with echo */
        char *buf = (char *)arg0;
        uint64_t maxlen = arg1;
        uint64_t i = 0;
        while (i < maxlen - 1) {
            int c = uart_getc();
            if (c == '\r' || c == '\n') {
                uart_putc('\r');
                uart_putc('\n');
                break;
            }
            if (c == 127 || c == 8) {  /* backspace/delete */
                if (i > 0) {
                    i--;
                    uart_putc('\b');
                    uart_putc(' ');
                    uart_putc('\b');
                }
                continue;
            }
            if (c >= 32 && c < 127) {
                buf[i++] = (char)c;
                uart_putc((char)c);  /* echo */
            }
        }
        buf[i] = '\0';
        regs[0] = i;
        break;
    }

    case SYS_WAIT: {
        /* wait(tid) — block until child finishes */
        int tid = (int)arg0;
        int ret = sched_wait(tid);
        regs[0] = (uint64_t)ret;
        break;
    }

    case SYS_LISTDIR: {
        /* listdir(buf, buflen) — write "name1\nname2\n..." into buf */
        char *buf = (char *)arg0;
        uint32_t buflen = (uint32_t)arg1;
        uint32_t pos = 0;

        /* Iterate root directory manually */
        struct fat16_file dummy;
        /* Use a simple approach: list via callback that writes to buf */
        /* We'll just iterate the root dir entries directly */
        struct fat16_bpb *bpb = &root_fs.bpb;
        static uint8_t dir_sector[512] __attribute__((aligned(512)));

        for (uint32_t s = 0; s < bpb->root_dir_sectors && pos < buflen - 1; s++) {
            if (virtio_blk_read(root_fs.blk, bpb->root_dir_sector + s, 1, dir_sector) < 0)
                break;
            struct fat16_dirent *entries = (struct fat16_dirent *)dir_sector;
            for (int j = 0; j < 16 && pos < buflen - 1; j++) {
                struct fat16_dirent *de = &entries[j];
                if (de->name[0] == 0x00) goto listdir_done;
                if ((uint8_t)de->name[0] == 0xE5) continue;
                if (de->attr & 0x1E) continue;  /* skip LFN/vol/dir/system */

                /* Copy name (trim spaces) */
                for (int k = 0; k < 8 && pos < buflen - 1; k++) {
                    if (de->name[k] != ' ') buf[pos++] = de->name[k];
                }
                if (de->ext[0] != ' ' && pos < buflen - 1) {
                    buf[pos++] = '.';
                    for (int k = 0; k < 3 && pos < buflen - 1; k++) {
                        if (de->ext[k] != ' ') buf[pos++] = de->ext[k];
                    }
                }
                if (pos < buflen - 1) buf[pos++] = '\n';
            }
        }
listdir_done:
        buf[pos] = '\0';
        regs[0] = pos;
        (void)dummy;
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
