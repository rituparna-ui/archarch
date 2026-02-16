/*
 * File descriptor layer implementation.
 */
#include "fd.h"
#include "uart.h"
#include "kmalloc.h"
#include "fat16.h"

extern struct fat16_fs root_fs;

/* Shared console file object (singleton — all processes share it) */
static struct open_file console_file = {
    .type = FD_TYPE_CONSOLE,
    .flags = O_RDWR,
    .ref_count = 0,
};

static struct open_file *alloc_file(void) {
    struct open_file *f = kzalloc(sizeof(struct open_file));
    return f;
}

static void free_file(struct open_file *f) {
    if (f && f != &console_file)
        kfree(f);
}

static int find_free_fd(struct fd_table *fdt) {
    for (int i = 0; i < MAX_FDS_PER_TASK; i++) {
        if (fdt->fds[i] == NULL)
            return i;
    }
    return -1;
}

void fd_table_init(struct fd_table *fdt) {
    for (int i = 0; i < MAX_FDS_PER_TASK; i++)
        fdt->fds[i] = NULL;

    /* fd 0 = stdin, fd 1 = stdout, fd 2 = stderr — all console */
    fdt->fds[0] = &console_file;
    fdt->fds[1] = &console_file;
    fdt->fds[2] = &console_file;
    console_file.ref_count += 3;
}

void fd_table_destroy(struct fd_table *fdt) {
    for (int i = 0; i < MAX_FDS_PER_TASK; i++) {
        if (fdt->fds[i]) {
            fdt->fds[i]->ref_count--;
            if (fdt->fds[i]->ref_count <= 0 && fdt->fds[i] != &console_file)
                free_file(fdt->fds[i]);
            fdt->fds[i] = NULL;
        }
    }
}

int fd_open(struct fd_table *fdt, const char *path, int flags) {
    int fd = find_free_fd(fdt);
    if (fd < 0) return -1;

    /* Try to open as a FAT16 file */
    struct fat16_file fat_file;
    if (fat16_open(&root_fs, path, &fat_file) < 0)
        return -1;

    struct open_file *f = alloc_file();
    if (!f) return -1;

    f->type = FD_TYPE_FILE;
    f->flags = flags;
    f->ref_count = 1;
    f->first_cluster = fat_file.first_cluster;
    f->file_size = fat_file.file_size;
    f->position = 0;
    f->current_cluster = fat_file.first_cluster;

    fdt->fds[fd] = f;
    return fd;
}

int fd_close(struct fd_table *fdt, int fd) {
    if (fd < 0 || fd >= MAX_FDS_PER_TASK || !fdt->fds[fd])
        return -1;

    struct open_file *f = fdt->fds[fd];
    f->ref_count--;
    if (f->ref_count <= 0 && f != &console_file)
        free_file(f);
    fdt->fds[fd] = NULL;
    return 0;
}

int fd_read(struct fd_table *fdt, int fd, void *buf, uint32_t count) {
    if (fd < 0 || fd >= MAX_FDS_PER_TASK || !fdt->fds[fd])
        return -1;

    struct open_file *f = fdt->fds[fd];

    if (f->type == FD_TYPE_CONSOLE) {
        /* Read from UART with line editing */
        char *cbuf = (char *)buf;
        uint32_t i = 0;
        while (i < count - 1) {
            int c = uart_getc();
            if (c == '\r' || c == '\n') {
                uart_putc('\r');
                uart_putc('\n');
                cbuf[i++] = '\n';
                break;
            }
            if (c == 127 || c == 8) {
                if (i > 0) {
                    i--;
                    uart_putc('\b');
                    uart_putc(' ');
                    uart_putc('\b');
                }
                continue;
            }
            if (c >= 32 && c < 127) {
                cbuf[i++] = (char)c;
                uart_putc((char)c);
            }
        }
        return (int)i;
    }

    if (f->type == FD_TYPE_FILE) {
        /* Read from FAT16 file */
        if (f->position >= f->file_size)
            return 0;  /* EOF */

        uint32_t remaining = f->file_size - f->position;
        if (count > remaining) count = remaining;

        /* Build a temporary fat16_file to use the existing read function */
        struct fat16_file tmp;
        tmp.first_cluster = f->first_cluster;
        tmp.file_size = f->file_size;
        tmp.position = f->position;
        tmp.current_cluster = f->first_cluster;

        /* For simplicity, re-read from the beginning and skip to position.
         * A proper implementation would track the current cluster chain. */
        uint8_t *dst = (uint8_t *)buf;
        int bytes = fat16_read_file(&root_fs, &tmp, dst, f->position + count);
        if (bytes < 0) return -1;

        /* We read from 0 to position+count, but we only want position..position+count */
        uint32_t actual = 0;
        if ((uint32_t)bytes > f->position) {
            actual = (uint32_t)bytes - f->position;
            if (actual > count) actual = count;
            /* Shift data: move bytes from offset position to start of buf */
            /* Actually, fat16_read_file reads into buf from the start.
             * We need to read the whole thing into a temp buffer. */
            uint8_t *tmpbuf = kmalloc(f->position + count);
            if (!tmpbuf) return -1;
            bytes = fat16_read_file(&root_fs, &tmp, tmpbuf, f->position + count);
            if (bytes > (int)f->position) {
                actual = (uint32_t)bytes - f->position;
                if (actual > count) actual = count;
                for (uint32_t j = 0; j < actual; j++)
                    dst[j] = tmpbuf[f->position + j];
            } else {
                actual = 0;
            }
            kfree(tmpbuf);
        }

        f->position += actual;
        return (int)actual;
    }

    return -1;
}

int fd_write(struct fd_table *fdt, int fd, const void *buf, uint32_t count) {
    if (fd < 0 || fd >= MAX_FDS_PER_TASK || !fdt->fds[fd])
        return -1;

    struct open_file *f = fdt->fds[fd];

    if (f->type == FD_TYPE_CONSOLE) {
        const char *cbuf = (const char *)buf;
        for (uint32_t i = 0; i < count; i++) {
            if (cbuf[i] == '\n')
                uart_putc('\r');
            uart_putc(cbuf[i]);
        }
        return (int)count;
    }

    /* File write not supported yet */
    return -1;
}
