/*
 * Virtio entropy device (RNG) driver over PCI.
 *
 * Implements the full virtio 1.x initialization sequence per spec section 3.1:
 *   1. Reset device
 *   2. Set ACKNOWLEDGE status bit
 *   3. Set DRIVER status bit
 *   4. Negotiate features
 *   5. Set FEATURES_OK
 *   6. Device-specific setup (virtqueue)
 *   7. Set DRIVER_OK
 *
 * The entropy device (spec 5.4) has a single virtqueue (requestq, index 0).
 * The driver places device-writable buffers on the queue, and the device
 * fills them with random data.
 */
#include "virtio_rng.h"
#include "uart.h"

/*
 * Walk PCI capability list to find virtio PCI capabilities.
 * Each capability tells us which BAR and offset to use for
 * common config, notifications, ISR, and device config.
 */
static int parse_virtio_caps(struct virtio_rng *dev) {
    struct pci_device *pci = &dev->pci;
    uint8_t bus = pci->bus, d = pci->dev, func = pci->func;

    uint8_t cap_off = pci_config_read8(bus, d, func, PCI_CAP_PTR) & 0xFC;
    int found_common = 0, found_notify = 0, found_isr = 0;

    while (cap_off) {
        uint8_t cap_id = pci_config_read8(bus, d, func, cap_off);
        uint8_t cap_next = pci_config_read8(bus, d, func, cap_off + 1);

        if (cap_id == PCI_CAP_ID_VENDOR) {
            uint8_t cfg_type = pci_config_read8(bus, d, func, cap_off + 3);
            uint8_t bar      = pci_config_read8(bus, d, func, cap_off + 4);
            uint32_t offset  = pci_config_read32(bus, d, func, cap_off + 8);
            uint32_t length  = pci_config_read32(bus, d, func, cap_off + 12);

            uintptr_t bar_base = pci->bar_addr[bar];
            if (bar_base == 0) {
                uart_puts("[VIRTIO] WARNING: cap references unmapped BAR ");
                uart_putdec(bar);
                uart_puts("\n");
                cap_off = cap_next;
                continue;
            }

            uintptr_t region = bar_base + offset;

            uart_puts("[VIRTIO] Cap type=");
            uart_putdec(cfg_type);
            uart_puts(" bar=");
            uart_putdec(bar);
            uart_puts(" offset=");
            uart_puthex(offset);
            uart_puts(" len=");
            uart_puthex(length);
            uart_puts(" -> ");
            uart_puthex(region);
            uart_puts("\n");

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                dev->vpci.common_cfg = region;
                found_common = 1;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                dev->vpci.notify_base = region;
                /* notify_off_multiplier is at cap_off + 16 for notify cap */
                dev->vpci.notify_off_mul = pci_config_read32(bus, d, func, cap_off + 16);
                found_notify = 1;
                uart_puts("[VIRTIO] Notify multiplier=");
                uart_putdec(dev->vpci.notify_off_mul);
                uart_puts("\n");
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                dev->vpci.isr_cfg = region;
                found_isr = 1;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                dev->vpci.device_cfg = region;
                break;
            }
        }

        cap_off = cap_next;
    }

    if (!found_common || !found_notify || !found_isr) {
        uart_puts("[VIRTIO] ERROR: missing required capabilities\n");
        return -1;
    }
    return 0;
}

/*
 * Full virtio initialization sequence (spec 3.1.1).
 */
int virtio_rng_init(struct virtio_rng *dev) {
    uart_puts("\n[VIRTIO-RNG] === Initialization Start ===\n");

    /* Step 0: Find the PCI device */
    if (!pci_find_virtio_rng(&dev->pci)) {
        uart_puts("[VIRTIO-RNG] Device not found on PCI bus\n");
        return -1;
    }

    /* Parse PCI capabilities to locate virtio config regions */
    if (parse_virtio_caps(dev) < 0)
        return -1;

    uintptr_t common = dev->vpci.common_cfg;

    /* Step 1: Reset the device (write 0 to status) */
    uart_puts("[VIRTIO-RNG] Step 1: Reset device\n");
    mmio_write8(common + VIRTIO_COMMON_STATUS, 0);
    dsb();

    /* Wait for reset to complete (status reads back as 0) */
    while (mmio_read8(common + VIRTIO_COMMON_STATUS) != 0)
        ;

    /* Step 2: Set ACKNOWLEDGE status bit */
    uart_puts("[VIRTIO-RNG] Step 2: Set ACKNOWLEDGE\n");
    mmio_write8(common + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    dsb();

    /* Step 3: Set DRIVER status bit */
    uart_puts("[VIRTIO-RNG] Step 3: Set DRIVER\n");
    mmio_write8(common + VIRTIO_COMMON_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    dsb();

    /* Step 4: Read and negotiate features */
    uart_puts("[VIRTIO-RNG] Step 4: Negotiate features\n");

    /* Read device features (low 32 bits) */
    mmio_write32(common + VIRTIO_COMMON_DFSELECT, 0);
    dsb();
    uint32_t feat_lo = mmio_read32(common + VIRTIO_COMMON_DF);
    uart_puts("  Device features[0]: ");
    uart_puthex(feat_lo);
    uart_puts("\n");

    /* Read device features (high 32 bits) */
    mmio_write32(common + VIRTIO_COMMON_DFSELECT, 1);
    dsb();
    uint32_t feat_hi = mmio_read32(common + VIRTIO_COMMON_DF);
    uart_puts("  Device features[1]: ");
    uart_puthex(feat_hi);
    uart_puts("\n");

    /*
     * The RNG device has no device-specific features.
     * We must accept VIRTIO_F_VERSION_1 (bit 32, in feat_hi bit 0).
     */
    uint32_t guest_lo = 0;
    uint32_t guest_hi = feat_hi & 0x01; /* accept VERSION_1 */

    mmio_write32(common + VIRTIO_COMMON_GFSELECT, 0);
    dsb();
    mmio_write32(common + VIRTIO_COMMON_GF, guest_lo);
    dsb();

    mmio_write32(common + VIRTIO_COMMON_GFSELECT, 1);
    dsb();
    mmio_write32(common + VIRTIO_COMMON_GF, guest_hi);
    dsb();

    uart_puts("  Accepted features: lo=");
    uart_puthex(guest_lo);
    uart_puts(" hi=");
    uart_puthex(guest_hi);
    uart_puts("\n");

    /* Step 5: Set FEATURES_OK */
    uart_puts("[VIRTIO-RNG] Step 5: Set FEATURES_OK\n");
    mmio_write8(common + VIRTIO_COMMON_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    dsb();

    /* Re-read to confirm device accepted features */
    uint8_t status = mmio_read8(common + VIRTIO_COMMON_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        uart_puts("[VIRTIO-RNG] ERROR: Device did not accept features!\n");
        mmio_write8(common + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    uart_puts("  Features accepted by device\n");

    /* Step 6: Device-specific setup — configure virtqueue 0 */
    uart_puts("[VIRTIO-RNG] Step 6: Setup virtqueue 0\n");

    /* Select queue 0 */
    mmio_write16(common + VIRTIO_COMMON_Q_SELECT, 0);
    dsb();

    uint16_t max_size = mmio_read16(common + VIRTIO_COMMON_Q_SIZE);
    uart_puts("  Max queue size: ");
    uart_putdec(max_size);
    uart_puts("\n");

    uint16_t qsize = VIRTQ_SIZE;
    if (qsize > max_size)
        qsize = max_size;

    /* Set our desired queue size */
    mmio_write16(common + VIRTIO_COMMON_Q_SIZE, qsize);
    dsb();

    /* Calculate notify address for this queue */
    uint16_t notify_off = mmio_read16(common + VIRTIO_COMMON_Q_NOTIFYOFF);
    uintptr_t notify_addr = dev->vpci.notify_base +
        (uintptr_t)notify_off * dev->vpci.notify_off_mul;

    uart_puts("  Notify offset=");
    uart_putdec(notify_off);
    uart_puts(" addr=");
    uart_puthex(notify_addr);
    uart_puts("\n");

    /* Initialize our virtqueue data structures */
    virtqueue_init(&dev->vq, qsize, notify_addr);

    /* Tell device where the queue structures are */
    uintptr_t desc_addr  = (uintptr_t)dev->vq.desc;
    uintptr_t avail_addr = (uintptr_t)dev->vq.avail;
    uintptr_t used_addr  = (uintptr_t)dev->vq.used;

    mmio_write32(common + VIRTIO_COMMON_Q_DESCLO,  (uint32_t)(desc_addr & 0xFFFFFFFF));
    mmio_write32(common + VIRTIO_COMMON_Q_DESCHI,  (uint32_t)(desc_addr >> 32));
    mmio_write32(common + VIRTIO_COMMON_Q_AVAILLO, (uint32_t)(avail_addr & 0xFFFFFFFF));
    mmio_write32(common + VIRTIO_COMMON_Q_AVAILHI, (uint32_t)(avail_addr >> 32));
    mmio_write32(common + VIRTIO_COMMON_Q_USEDLO,  (uint32_t)(used_addr & 0xFFFFFFFF));
    mmio_write32(common + VIRTIO_COMMON_Q_USEDHI,  (uint32_t)(used_addr >> 32));
    dsb();

    /* Enable the queue */
    mmio_write16(common + VIRTIO_COMMON_Q_ENABLE, 1);
    dsb();

    uart_puts("  Queue 0 enabled\n");

    /* Step 7: Set DRIVER_OK — device is now live */
    uart_puts("[VIRTIO-RNG] Step 7: Set DRIVER_OK\n");
    mmio_write8(common + VIRTIO_COMMON_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    dsb();

    uart_puts("[VIRTIO-RNG] === Initialization Complete ===\n\n");
    return 0;
}

int virtio_rng_read(struct virtio_rng *dev, void *buf, uint32_t len) {
    /* Place a device-writable buffer on the queue */
    uint16_t desc_idx = virtqueue_add_buf_write(&dev->vq, buf, len);
    if (desc_idx == 0xFFFF)
        return -1;

    /* Notify the device */
    virtqueue_kick(&dev->vq);

    /* Poll for completion */
    uint16_t used_idx;
    uint32_t written;
    uint64_t timeout = 10000000;
    while (!virtqueue_get_used(&dev->vq, &used_idx, &written)) {
        if (--timeout == 0) {
            uart_puts("[VIRTIO-RNG] Timeout waiting for device!\n");
            return -1;
        }
    }

    return (int)written;
}
