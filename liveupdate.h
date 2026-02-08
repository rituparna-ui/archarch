/*
 * Live kernel update via virtio-blk.
 *
 * Allows replacing the running kernel with a new image stored on
 * the virtio-blk disk, without losing state. A handoff region in
 * high RAM preserves generation counter and uptime across updates.
 *
 * Disk layout (512-byte sectors):
 *   Sector 0-3:   user data (blk demo)
 *   Sector 4-5:   update header (1024 bytes)
 *   Sector 6+:    new kernel.bin image
 *
 * Update flow:
 *   1. Host writes kernel.bin to sectors 6+ on disk.img
 *   2. Host writes update header to sector 4 with magic + size.
 *   3. Running kernel polls sector 4, detects the magic.
 *   4. Reads new image into staging area (high RAM).
 *   5. Disables IRQs, copies image to 0x40000000, jumps to _start.
 *   6. New kernel checks handoff region, prints warm-boot banner.
 *
 * Handoff region: fixed address at top of RAM (128MB - 4KB = 0x47FFF000).
 * Survives the update because we only overwrite low RAM (kernel image).
 */
#ifndef LIVEUPDATE_H
#define LIVEUPDATE_H

#include "types.h"
#include "virtio_blk.h"

/* Magic values */
#define UPDATE_HEADER_MAGIC   0x4C495645554E4557ULL  /* "LIVEUPDW" */
#define HANDOFF_MAGIC         0x48414E444F464621ULL  /* "HANDOFF!" */

/* Disk sectors */
#define UPDATE_HEADER_SECTOR  4
#define UPDATE_IMAGE_SECTOR   6

/* Handoff region: last 4KB of 128MB RAM */
#define HANDOFF_ADDR          0x47FFF000UL

/* Max kernel image size: 512KB (1024 sectors) */
#define MAX_IMAGE_SIZE        (512 * 1024)

/* Staging area: just below handoff region */
#define STAGING_ADDR          (HANDOFF_ADDR - MAX_IMAGE_SIZE)

/* Update header on disk (sector 4-5, 1024 bytes) */
struct update_header {
    uint64_t magic;         /* UPDATE_HEADER_MAGIC */
    uint32_t image_size;    /* size of kernel.bin in bytes */
    uint32_t image_sectors; /* number of 512-byte sectors */
    uint32_t generation;    /* update generation (incremented each update) */
    uint32_t checksum;      /* simple additive checksum of image */
};

/* Handoff region in RAM (survives update) */
struct handoff_data {
    uint64_t magic;         /* HANDOFF_MAGIC */
    uint32_t generation;    /* current generation */
    uint32_t prev_uptime_ms;/* uptime at moment of update */
    uint64_t total_boots;   /* total boot count */
    uint64_t total_ticks;   /* accumulated timer ticks */
    char     message[64];   /* short message from previous generation */
};

/*
 * Check if this is a warm boot (live update).
 * Returns pointer to handoff data if valid, NULL if cold boot.
 */
struct handoff_data *liveupdate_check_handoff(void);

/*
 * Initialize handoff region for first boot (cold boot).
 */
void liveupdate_cold_init(void);

/*
 * Poll the disk for an update header.
 * Returns 1 if update is available, 0 if not.
 */
int liveupdate_check_disk(struct virtio_blk *blk);

/*
 * Execute the live update: read new image, save state, jump.
 * This function does not return.
 */
void liveupdate_apply(struct virtio_blk *blk, uint64_t uptime_ms,
                      uint64_t ticks) __attribute__((noreturn));

/*
 * Write an update image to disk (for self-update testing).
 * Writes kernel.bin data to sectors 6+ and header to sector 4.
 */
int liveupdate_stage(struct virtio_blk *blk, const void *image,
                     uint32_t size, uint32_t generation);

#endif
