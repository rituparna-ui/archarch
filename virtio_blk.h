#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "pci.h"
#include "virtio_pci.h"
#include "virtqueue.h"

/*
 * Virtio block device (spec 5.2)
 *
 * Device-specific config layout at device_cfg MMIO region:
 *   offset 0x00: uint64_t capacity    (in 512-byte sectors)
 *   offset 0x08: uint32_t size_max    (max segment size)
 *   offset 0x0C: uint32_t seg_max     (max segments per request)
 *   offset 0x10: struct virtio_blk_geometry
 *   offset 0x18: uint32_t blk_size    (if VIRTIO_BLK_F_BLK_SIZE)
 *
 * Request format (3-descriptor chain):
 *   desc[0]: virtio_blk_req header  (device-readable)
 *   desc[1]: data buffer            (readable for write, writable for read)
 *   desc[2]: uint8_t status          (device-writable)
 */

/* Feature bits (spec 5.2.3) */
#define VIRTIO_BLK_F_SIZE_MAX   (1 << 1)
#define VIRTIO_BLK_F_SEG_MAX    (1 << 2)
#define VIRTIO_BLK_F_GEOMETRY   (1 << 4)
#define VIRTIO_BLK_F_RO         (1 << 5)
#define VIRTIO_BLK_F_BLK_SIZE   (1 << 6)
#define VIRTIO_BLK_F_FLUSH      (1 << 9)

/* Request types */
#define VIRTIO_BLK_T_IN         0   /* read */
#define VIRTIO_BLK_T_OUT        1   /* write */
#define VIRTIO_BLK_T_FLUSH      4
#define VIRTIO_BLK_T_GET_ID     8

/* Status values */
#define VIRTIO_BLK_S_OK         0
#define VIRTIO_BLK_S_IOERR      1
#define VIRTIO_BLK_S_UNSUPP     2

/* Request header (spec 5.2.6) */
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct virtio_blk {
    struct pci_device     pci;
    struct virtio_pci_dev vpci;
    struct virtqueue      vq;       /* requestq (queue 0) */
    uint64_t              capacity; /* in 512-byte sectors */
    uint32_t              blk_size; /* bytes per block (usually 512) */
};

int virtio_blk_init(struct virtio_blk *dev);

/*
 * Read sectors from the device.
 *   sector: starting sector (512 bytes each)
 *   count:  number of sectors to read
 *   buf:    output buffer (must be count * 512 bytes)
 * Returns 0 on success, -1 on error.
 */
int virtio_blk_read(struct virtio_blk *dev, uint64_t sector,
                    uint32_t count, void *buf);

/*
 * Write sectors to the device.
 */
int virtio_blk_write(struct virtio_blk *dev, uint64_t sector,
                     uint32_t count, const void *buf);

#endif
