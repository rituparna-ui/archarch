/*
 * System call dispatch — Unix-style with file descriptors.
 */
#include "syscall.h"
#include "uart.h"
#include "sched.h"
#include "pmm.h"
#include "fat16.h"
#include "kmalloc.h"
#include "virtio_blk.h"
#include "fd.h"

extern struct fat16_fs root_fs;

static struct fd_table *current_fdt(void) {
    struct task *t = sched_get_task(sched_current_id());
    return &t->fdt;
}

void syscall_handler(uint64_t *regs) {
    uint64_t num  = regs[8];
    uint64_t arg0 = regs[0];
    uint64_t arg1 = regs[1];
    uint64_t arg2 = regs[2];

    switch (num) {

    case SYS_READ: {
        int fd = (int)arg0;
        void *buf = (void *)arg1;
        uint32_t count = (uint32_t)arg2;
        regs[0] = (uint64_t)fd_read(current_fdt(), fd, buf, count);
        break;
    }

    case SYS_WRITE: {
        int fd = (int)arg0;
        const void *buf = (const void *)arg1;
        uint32_t count = (uint32_t)arg2;
        regs[0] = (uint64_t)fd_write(current_fdt(), fd, buf, count);
        break;
    }

    case SYS_OPEN: {
        const char *path = (const char *)arg0;
        int flags = (int)arg1;
        regs[0] = (uint64_t)fd_open(current_fdt(), path, flags);
        break;
    }

    case SYS_CLOSE: {
        regs[0] = (uint64_t)fd_close(current_fdt(), (int)arg0);
        break;
    }

    case SYS_GETPID:
        regs[0] = (uint64_t)sched_current_id();
        break;

    case SYS_EXIT: {
        uart_puts("[SYSCALL] Task ");
        uart_putdec((uint64_t)sched_current_id());
        uart_puts(" exit(");
        uart_putdec(arg0);
        uart_puts(")\n");
        sched_exit();
        __builtin_unreachable();
    }

    case SYS_YIELD:
        sched_yield();
        regs[0] = 0;
        break;

    case SYS_EXEC: {
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
        if (!code) { regs[0] = (uint64_t)-1; break; }
        int bytes = fat16_read_file(&root_fs, &file, code, file.file_size);
        if (bytes < 0) { kfree(code); regs[0] = (uint64_t)-1; break; }
        uart_puts("[EXEC] Loaded ");
        uart_putdec((uint64_t)bytes);
        uart_puts(" bytes\n");
        int tid = sched_create_user(fname, code, (uint32_t)bytes);
        kfree(code);
        regs[0] = (tid < 0) ? (uint64_t)-1 : (uint64_t)tid;
        break;
    }

    case SYS_WAIT:
        regs[0] = (uint64_t)sched_wait((int)arg0);
        break;

    case SYS_SBRK: {
        uint64_t pages = (arg0 + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages == 0) pages = 1;
        uintptr_t addr = pmm_alloc_pages((uint32_t)pages);
        regs[0] = addr ? addr : (uint64_t)-1;
        break;
    }

    case SYS_LISTDIR: {
        char *buf = (char *)arg0;
        uint32_t buflen = (uint32_t)arg1;
        uint32_t pos = 0;
        struct fat16_bpb *bpb = &root_fs.bpb;
        static uint8_t dir_sector[512] __attribute__((aligned(512)));
        for (uint32_t s = 0; s < bpb->root_dir_sectors && pos < buflen - 1; s++) {
            if (virtio_blk_read(root_fs.blk, bpb->root_dir_sector + s, 1, dir_sector) < 0)
                break;
            struct fat16_dirent *entries = (struct fat16_dirent *)dir_sector;
            for (int j = 0; j < 16 && pos < buflen - 1; j++) {
                struct fat16_dirent *de = &entries[j];
                if (de->name[0] == 0x00) goto done;
                if ((uint8_t)de->name[0] == 0xE5) continue;
                if (de->attr & 0x1E) continue;
                for (int k = 0; k < 8 && pos < buflen - 1; k++)
                    if (de->name[k] != ' ') buf[pos++] = de->name[k];
                if (de->ext[0] != ' ' && pos < buflen - 1) {
                    buf[pos++] = '.';
                    for (int k = 0; k < 3 && pos < buflen - 1; k++)
                        if (de->ext[k] != ' ') buf[pos++] = de->ext[k];
                }
                if (pos < buflen - 1) buf[pos++] = '\n';
            }
        }
done:   buf[pos] = '\0';
        regs[0] = pos;
        break;
    }

    case SYS_FORK: {
        int child_tid = sched_fork(regs);
        regs[0] = (uint64_t)child_tid;
        break;
    }

    case SYS_PIPE: {
        int *user_fds = (int *)arg0;
        int fds[2];
        int ret = fd_pipe(current_fdt(), fds);
        if (ret == 0) {
            user_fds[0] = fds[0];
            user_fds[1] = fds[1];
        }
        regs[0] = (uint64_t)ret;
        break;
    }

    case SYS_DUP2: {
        regs[0] = (uint64_t)fd_dup2(current_fdt(), (int)arg0, (int)arg1);
        break;
    }

    default:
        uart_puts("[SYSCALL] Unknown #");
        uart_putdec(num);
        uart_puts("\n");
        regs[0] = (uint64_t)-1;
        break;
    }
}
