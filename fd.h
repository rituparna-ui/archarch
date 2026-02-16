/*
 * File descriptor layer.
 *
 * Each process has a table of open file descriptors.
 * fd 0 = stdin, fd 1 = stdout, fd 2 = stderr (all UART by default).
 *
 * Each fd points to a struct file which has a type and type-specific
 * read/write operations.
 */
#ifndef FD_H
#define FD_H

#include "types.h"

#define MAX_FDS_PER_TASK 16

/* File types */
#define FD_TYPE_NONE    0
#define FD_TYPE_CONSOLE 1   /* UART stdin/stdout/stderr */
#define FD_TYPE_FILE    2   /* FAT16 file (read-only for now) */

/* Open flags */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2

struct open_file {
    int      type;
    int      flags;       /* O_RDONLY, O_WRONLY, O_RDWR */
    int      ref_count;   /* Number of fd's pointing to this file */

    /* For FD_TYPE_FILE: FAT16 file state */
    uint16_t first_cluster;
    uint32_t file_size;
    uint32_t position;     /* Current read offset */
    uint16_t current_cluster;
};

/* Per-process fd table */
struct fd_table {
    struct open_file *fds[MAX_FDS_PER_TASK];
};

/*
 * Initialize a new fd table for a process.
 * Opens fd 0/1/2 as console (UART).
 */
void fd_table_init(struct fd_table *fdt);

/*
 * Destroy a fd table, closing all open files.
 */
void fd_table_destroy(struct fd_table *fdt);

/*
 * Open a file by path. Returns fd number, or -1 on error.
 */
int fd_open(struct fd_table *fdt, const char *path, int flags);

/*
 * Close a file descriptor. Returns 0 on success, -1 on error.
 */
int fd_close(struct fd_table *fdt, int fd);

/*
 * Read from a file descriptor. Returns bytes read, or -1 on error.
 * For console (stdin): reads one line with echo.
 * For files: reads up to count bytes from current position.
 */
int fd_read(struct fd_table *fdt, int fd, void *buf, uint32_t count);

/*
 * Write to a file descriptor. Returns bytes written, or -1 on error.
 * For console (stdout/stderr): writes to UART.
 * For files: not supported yet (returns -1).
 */
int fd_write(struct fd_table *fdt, int fd, const void *buf, uint32_t count);

#endif
