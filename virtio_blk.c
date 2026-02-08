/*
 * Virtio block device driver over PCI (spec 5.2).
 *
 * Uses 3-descriptor chains per request:
 *   [0] virtio_blk_req header  (device-readable)
 *   [1] data buffer            (read: device-writable, write: device-readable)
 *   [2] status byte            (device-writable)
 */
#include "virtio_blk.h"
#include "uart.h"

/* Device config offsets (spec 5.2.4) */
#define BLK_CFG_CAPACITY    0x00  /* u64 */
#define BLK_CFG_SIZE_MAX    0x08  /* u32 */
#define BLK_CFG_SEG_MAX     0x0C  /* u32 */
#define BLK_CFG_BLK_SIZE    0x18  /* u32 */

/* Static request buffers — only one request in flight at a time */
static struct virtio_blk_req blk_req_hdr __attribute__((aligned(16)));
static uint8_t               blk_status  __attribute__((aligned(4)));

int virtio_blk_init(struct virtio_blk *dev) {
    uart_puts("\n[VIRTIO-BLK] === Init ===\n");

    if (!pci_find_virtio_blk(&dev->pci)) {
        uart_puts("[VIRTIO-BLK] Device not found\n");
        return -1;
    }

    if (virtio_pci_parse_caps(&dev->pci, &dev->vpci) < 0)
        return -1;

    /*
     * Accept BLK_SIZE feature so we can read the block size.
     * We don't need SEG_MAX/SIZE_MAX for our simple single-segment requests.
     */
    uint32_t want_features = VIRTIO_BLK_F_BLK_SIZE;

    if (virtio_pci_init_device(&dev->pci, &dev->vpci, want_features, 0) < 0)
        return -1;

    /* Read device config */
    uintptr_t dcfg = dev->vpci.device_cfg;
    if (dcfg == 0) {
        uart_puts("[VIRTIO-BLK] No device config region!\n");
        return -1;
    }

    /* Capacity is 64-bit LE at offset 0 */
    uint32_t cap_lo = mmio_read32(dcfg + BLK_CFG_CAPACITY);
    uint32_t cap_hi = mmio_read32(dcfg + BLK_CFG_CAPACITY + 4);
    dev->capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    /* Block size (default 512 if feature not negotiated) */
    dev->blk_size = 512;
    /* Try reading it anyway — QEMU usually provides it */
    uint32_t bs = mmio_read32(dcfg + BLK_CFG_BLK_SIZE);
    if (bs >= 512 && bs <= 4096)
        dev->blk_size = bs;

    uart_puts("  Capacity: ");
    uart_putdec(dev->capacity);
    uart_puts(" sectors (");
    uart_putdec(dev->capacity * 512 / 1024);
    uart_puts(" KiB)\n");
    uart_puts("  Block size: ");
    uart_putdec(dev->blk_size);
    uart_puts(" bytes\n");

    /* Setup requestq (queue 0) */
    if (virtio_pci_setup_queue(&dev->vpci, &dev->vq, 0, 32) < 0)
        return -1;

    virtio_pci_set_driver_ok(&dev->vpci);

    uart_puts("[VIRTIO-BLK] === Ready ===\n\n");
    return 0;
}

/*
 * Submit a block I/O request using a 3-descriptor chain.
 */
static int blk_do_request(struct virtio_blk *dev, uint32_t type,
                          uint64_t sector, void *buf, uint32_t len)
{
    /* Fill request header */
    blk_req_hdr.type     = type;
    blk_req_hdr.reserved = 0;
    blk_req_hdr.sector   = sector;
    blk_status = 0xFF; /* sentinel */

    /*
     * Build 3-descriptor chain:
     *   [0] header — always device-readable
     *   [1] data   — writable for reads, readable for writes
     *   [2] status — always device-writable
     */
    struct vq_buf chain[3];

    /* Descriptor 0: request header (device reads it) */
    chain[0].addr  = &blk_req_hdr;
    chain[0].len   = sizeof(blk_req_hdr);
    chain[0].flags = 0; /* device-readable */

    /* Descriptor 1: data buffer */
    chain[1].addr  = buf;
    chain[1].len   = len;
    chain[1].flags = (type == VIRTIO_BLK_T_IN) ? VRING_DESC_F_WRITE : 0;

    /* Descriptor 2: status byte (device writes it) */
    chain[2].addr  = &blk_status;
    chain[2].len   = 1;
    chain[2].flags = VRING_DESC_F_WRITE;

    uint16_t head = virtqueue_add_chain(&dev->vq, chain, 3);
    if (head == 0xFFFF)
        return -1;

    virtqueue_kick(&dev->vq);

    /* Poll for completion */
    uint16_t used_idx;
    uint32_t written;
    uint64_t timeout = 50000000;
    while (!virtqueue_get_used(&dev->vq, &used_idx, &written)) {
        if (--timeout == 0) {
            uart_puts("[VIRTIO-BLK] Timeout!\n");
            return -1;
        }
    }

    if (blk_status != VIRTIO_BLK_S_OK) {
        uart_puts("[VIRTIO-BLK] Request failed, status=");
        uart_putdec(blk_status);
        uart_puts("\n");
        return -1;
    }

    return 0;
}

int virtio_blk_read(struct virtio_blk *dev, uint64_t sector,
                    uint32_t count, void *buf)
{
    return blk_do_request(dev, VIRTIO_BLK_T_IN, sector,
                          buf, count * 512);
}

int virtio_blk_write(struct virtio_blk *dev, uint64_t sector,
                     uint32_t count, const void *buf)
{
    return blk_do_request(dev, VIRTIO_BLK_T_OUT, sector,
                          (void *)buf, count * 512);
}
