/*
 * Minimal read-only FAT16 filesystem driver.
 *
 * Supports:
 *   - Reading the root directory
 *   - Finding files by 8.3 name
 *   - Reading file contents
 *
 * Limitations:
 *   - Root directory only (no subdirectories)
 *   - Read-only
 *   - No long filename support
 *   - Single partition (no MBR parsing)
 */
#ifndef FAT16_H
#define FAT16_H

#include "types.h"
#include "virtio_blk.h"

/* FAT16 Boot Sector / BPB fields we care about */
struct fat16_bpb {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint32_t total_sectors_32;
    uint16_t fat_size_16;       /* sectors per FAT */
    uint32_t root_dir_sector;   /* computed: first sector of root dir */
    uint32_t root_dir_sectors;  /* computed: number of sectors for root dir */
    uint32_t data_start_sector; /* computed: first sector of data area */
};

/* FAT16 directory entry (32 bytes) */
struct fat16_dirent {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved[10];
    uint16_t time;
    uint16_t date;
    uint16_t first_cluster;
    uint32_t file_size;
} __attribute__((packed));

/* Directory entry attributes */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

/* File handle for reading */
struct fat16_file {
    uint16_t first_cluster;
    uint32_t file_size;
    uint32_t position;      /* current read position */
    uint16_t current_cluster;
};

/* Filesystem state */
struct fat16_fs {
    struct virtio_blk *blk;
    struct fat16_bpb   bpb;
    uint16_t          *fat_table;  /* cached FAT */
};

/*
 * Mount a FAT16 filesystem from a virtio-blk device.
 * Reads the BPB and caches the FAT table.
 * Returns 0 on success, -1 on error.
 */
int fat16_mount(struct fat16_fs *fs, struct virtio_blk *blk);

/*
 * List files in the root directory.
 * Calls callback for each file found.
 * Returns number of files found.
 */
int fat16_list_root(struct fat16_fs *fs,
                    void (*callback)(const char *name, uint32_t size));

/*
 * Open a file by 8.3 name (e.g. "HELLO   BIN" or "hello.bin").
 * Returns 0 on success, -1 if not found.
 */
int fat16_open(struct fat16_fs *fs, const char *filename,
               struct fat16_file *file);

/*
 * Read the entire file into a buffer.
 * buf must be at least file->file_size bytes.
 * Returns bytes read, or -1 on error.
 */
int fat16_read_file(struct fat16_fs *fs, struct fat16_file *file,
                    void *buf, uint32_t buf_size);

#endif
