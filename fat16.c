/*
 * Minimal read-only FAT16 filesystem driver.
 */
#include "fat16.h"
#include "kmalloc.h"
#include "uart.h"

/* Read a 16-bit LE value from a byte buffer */
static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Sector buffer */
static uint8_t sector_buf[512] __attribute__((aligned(512)));

int fat16_mount(struct fat16_fs *fs, struct virtio_blk *blk) {
    fs->blk = blk;

    uart_puts("[FAT16] Reading boot sector...\n");

    if (virtio_blk_read(blk, 0, 1, sector_buf) < 0) {
        uart_puts("[FAT16] Cannot read boot sector\n");
        return -1;
    }

    /* Check boot signature */
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) {
        uart_puts("[FAT16] Invalid boot signature\n");
        return -1;
    }

    /* Parse BPB */
    struct fat16_bpb *bpb = &fs->bpb;
    bpb->bytes_per_sector    = read16(&sector_buf[11]);
    bpb->sectors_per_cluster = sector_buf[13];
    bpb->reserved_sectors    = read16(&sector_buf[14]);
    bpb->num_fats            = sector_buf[16];
    bpb->root_entry_count    = read16(&sector_buf[17]);
    bpb->total_sectors_16    = read16(&sector_buf[19]);
    bpb->total_sectors_32    = read32(&sector_buf[32]);
    bpb->fat_size_16         = read16(&sector_buf[22]);

    if (bpb->bytes_per_sector != 512) {
        uart_puts("[FAT16] Unsupported sector size\n");
        return -1;
    }

    /* Compute derived values */
    bpb->root_dir_sector = bpb->reserved_sectors
                         + bpb->num_fats * bpb->fat_size_16;
    bpb->root_dir_sectors = ((bpb->root_entry_count * 32)
                           + (bpb->bytes_per_sector - 1))
                          / bpb->bytes_per_sector;
    bpb->data_start_sector = bpb->root_dir_sector + bpb->root_dir_sectors;

    uart_puts("[FAT16] Mounted: ");
    uart_putdec(bpb->sectors_per_cluster);
    uart_puts(" sec/cluster, ");
    uart_putdec(bpb->fat_size_16);
    uart_puts(" FAT sectors, ");
    uart_putdec(bpb->root_entry_count);
    uart_puts(" root entries\n");
    uart_puts("[FAT16] Root dir at sector ");
    uart_putdec(bpb->root_dir_sector);
    uart_puts(", data at sector ");
    uart_putdec(bpb->data_start_sector);
    uart_puts("\n");

    /* Cache the FAT table */
    uint32_t fat_bytes = bpb->fat_size_16 * 512;
    fs->fat_table = (uint16_t *)kmalloc(fat_bytes);
    if (!fs->fat_table) {
        uart_puts("[FAT16] Cannot allocate FAT cache\n");
        return -1;
    }

    uart_puts("[FAT16] Reading FAT (");
    uart_putdec(bpb->fat_size_16);
    uart_puts(" sectors)...\n");

    for (uint32_t i = 0; i < bpb->fat_size_16; i++) {
        if (virtio_blk_read(blk, bpb->reserved_sectors + i, 1,
                           (uint8_t *)fs->fat_table + i * 512) < 0) {
            uart_puts("[FAT16] Cannot read FAT\n");
            return -1;
        }
    }

    uart_puts("[FAT16] Filesystem mounted OK\n");
    return 0;
}

/*
 * Convert a "filename.ext" string to FAT 8.3 format.
 * Output: 11 bytes, space-padded, uppercase.
 */
static void to_fat83(const char *input, char *out) {
    /* Fill with spaces */
    for (int i = 0; i < 11; i++) out[i] = ' ';

    int pos = 0;
    int i = 0;

    /* Copy name part (up to 8 chars, before dot) */
    while (input[i] && input[i] != '.' && pos < 8) {
        char c = input[i];
        if (c >= 'a' && c <= 'z') c -= 32; /* uppercase */
        out[pos++] = c;
        i++;
    }

    /* Skip to extension */
    while (input[i] && input[i] != '.') i++;
    if (input[i] == '.') i++;

    /* Copy extension (up to 3 chars) */
    pos = 8;
    while (input[i] && pos < 11) {
        char c = input[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[pos++] = c;
        i++;
    }
}

/*
 * Compare two 11-byte FAT 8.3 names.
 */
static int fat83_cmp(const char *a, const char *b) {
    for (int i = 0; i < 11; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/*
 * Format a FAT 8.3 name into a human-readable "name.ext" string.
 */
static void fat83_to_str(const struct fat16_dirent *de, char *out) {
    int pos = 0;

    /* Copy name, trimming trailing spaces */
    for (int i = 0; i < 8; i++) {
        if (de->name[i] != ' ')
            out[pos++] = de->name[i];
    }

    /* Add dot and extension if present */
    if (de->ext[0] != ' ') {
        out[pos++] = '.';
        for (int i = 0; i < 3; i++) {
            if (de->ext[i] != ' ')
                out[pos++] = de->ext[i];
        }
    }

    out[pos] = '\0';
}

int fat16_list_root(struct fat16_fs *fs,
                    void (*callback)(const char *name, uint32_t size)) {
    struct fat16_bpb *bpb = &fs->bpb;
    int count = 0;

    for (uint32_t s = 0; s < bpb->root_dir_sectors; s++) {
        if (virtio_blk_read(fs->blk, bpb->root_dir_sector + s, 1,
                           sector_buf) < 0)
            break;

        struct fat16_dirent *entries = (struct fat16_dirent *)sector_buf;
        int entries_per_sector = 512 / sizeof(struct fat16_dirent);

        for (int i = 0; i < entries_per_sector; i++) {
            struct fat16_dirent *de = &entries[i];

            /* End of directory */
            if (de->name[0] == 0x00) return count;

            /* Deleted entry */
            if ((uint8_t)de->name[0] == 0xE5) continue;

            /* Skip LFN, volume label, directories */
            if (de->attr & (FAT_ATTR_LFN | FAT_ATTR_VOLUME_ID | FAT_ATTR_DIRECTORY))
                continue;

            char name_buf[13];
            fat83_to_str(de, name_buf);

            if (callback)
                callback(name_buf, de->file_size);
            count++;
        }
    }

    return count;
}

int fat16_open(struct fat16_fs *fs, const char *filename,
               struct fat16_file *file) {
    struct fat16_bpb *bpb = &fs->bpb;
    char fat_name[11];
    to_fat83(filename, fat_name);

    for (uint32_t s = 0; s < bpb->root_dir_sectors; s++) {
        if (virtio_blk_read(fs->blk, bpb->root_dir_sector + s, 1,
                           sector_buf) < 0)
            return -1;

        struct fat16_dirent *entries = (struct fat16_dirent *)sector_buf;
        int entries_per_sector = 512 / sizeof(struct fat16_dirent);

        for (int i = 0; i < entries_per_sector; i++) {
            struct fat16_dirent *de = &entries[i];

            if (de->name[0] == 0x00) return -1;
            if ((uint8_t)de->name[0] == 0xE5) continue;
            if (de->attr & (FAT_ATTR_LFN | FAT_ATTR_VOLUME_ID)) continue;

            char entry_name[11];
            for (int j = 0; j < 8; j++) entry_name[j] = de->name[j];
            for (int j = 0; j < 3; j++) entry_name[8+j] = de->ext[j];

            if (fat83_cmp(fat_name, entry_name)) {
                file->first_cluster   = de->first_cluster;
                file->file_size       = de->file_size;
                file->position        = 0;
                file->current_cluster = de->first_cluster;
                return 0;
            }
        }
    }

    return -1;
}

int fat16_read_file(struct fat16_fs *fs, struct fat16_file *file,
                    void *buf, uint32_t buf_size) {
    struct fat16_bpb *bpb = &fs->bpb;
    uint32_t bytes_per_cluster = bpb->sectors_per_cluster * 512;
    uint32_t to_read = file->file_size;
    if (to_read > buf_size) to_read = buf_size;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t bytes_done = 0;
    uint16_t cluster = file->first_cluster;

    while (bytes_done < to_read && cluster >= 2 && cluster < 0xFFF8) {
        /* Compute first sector of this cluster */
        uint32_t first_sector = bpb->data_start_sector
                              + (uint32_t)(cluster - 2) * bpb->sectors_per_cluster;

        /* Read each sector in the cluster */
        for (uint32_t s = 0; s < bpb->sectors_per_cluster && bytes_done < to_read; s++) {
            if (virtio_blk_read(fs->blk, first_sector + s, 1, sector_buf) < 0)
                return -1;

            uint32_t chunk = 512;
            if (bytes_done + chunk > to_read)
                chunk = to_read - bytes_done;

            for (uint32_t j = 0; j < chunk; j++)
                dst[bytes_done + j] = sector_buf[j];

            bytes_done += chunk;
        }

        /* Follow FAT chain */
        cluster = fs->fat_table[cluster];
    }

    return (int)bytes_done;
}
