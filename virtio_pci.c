/*
 * Common virtio PCI initialization helpers.
 * Shared by all virtio device drivers (RNG, BLK, etc).
 */
#include "virtio_pci.h"
#include "virtqueue.h"
#include "pci.h"
#include "uart.h"

int virtio_pci_parse_caps(struct pci_device *pci, struct virtio_pci_dev *vpci) {
    uint8_t bus = pci->bus, d = pci->dev, func = pci->func;
    uint8_t cap_off = pci_config_read8(bus, d, func, PCI_CAP_PTR) & 0xFC;
    int found_common = 0, found_notify = 0, found_isr = 0;

    while (cap_off) {
        uint8_t cap_id   = pci_config_read8(bus, d, func, cap_off);
        uint8_t cap_next = pci_config_read8(bus, d, func, cap_off + 1);

        if (cap_id == PCI_CAP_ID_VENDOR) {
            uint8_t  cfg_type = pci_config_read8(bus, d, func, cap_off + 3);
            uint8_t  bar      = pci_config_read8(bus, d, func, cap_off + 4);
            uint32_t offset   = pci_config_read32(bus, d, func, cap_off + 8);
            uint32_t length   = pci_config_read32(bus, d, func, cap_off + 12);

            uintptr_t bar_base = pci->bar_addr[bar];
            if (bar_base == 0) {
                cap_off = cap_next;
                continue;
            }

            uintptr_t region = bar_base + offset;

            uart_puts("  Cap type=");
            uart_putdec(cfg_type);
            uart_puts(" bar=");
            uart_putdec(bar);
            uart_puts(" off=");
            uart_puthex(offset);
            uart_puts(" len=");
            uart_puthex(length);
            uart_puts(" -> ");
            uart_puthex(region);
            uart_puts("\n");

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                vpci->common_cfg = region;
                found_common = 1;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                vpci->notify_base = region;
                vpci->notify_off_mul = pci_config_read32(bus, d, func, cap_off + 16);
                found_notify = 1;
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                vpci->isr_cfg = region;
                found_isr = 1;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                vpci->device_cfg = region;
                break;
            }
        }
        cap_off = cap_next;
    }

    if (!found_common || !found_notify || !found_isr) {
        uart_puts("  ERROR: missing required virtio caps\n");
        return -1;
    }
    return 0;
}

int virtio_pci_init_device(struct pci_device *pci, struct virtio_pci_dev *vpci,
                           uint32_t accepted_features_lo,
                           uint32_t required_features_hi_mask)
{
    uintptr_t common = vpci->common_cfg;

    /* Step 1: Reset */
    mmio_write8(common + VIRTIO_COMMON_STATUS, 0);
    dsb();
    while (mmio_read8(common + VIRTIO_COMMON_STATUS) != 0)
        ;

    /* Step 2: ACKNOWLEDGE */
    mmio_write8(common + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    dsb();

    /* Step 3: DRIVER */
    mmio_write8(common + VIRTIO_COMMON_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    dsb();

    /* Step 4: Feature negotiation */
    mmio_write32(common + VIRTIO_COMMON_DFSELECT, 0);
    dsb();
    uint32_t feat_lo = mmio_read32(common + VIRTIO_COMMON_DF);

    mmio_write32(common + VIRTIO_COMMON_DFSELECT, 1);
    dsb();
    uint32_t feat_hi = mmio_read32(common + VIRTIO_COMMON_DF);

    uart_puts("  Device features: lo=");
    uart_puthex(feat_lo);
    uart_puts(" hi=");
    uart_puthex(feat_hi);
    uart_puts("\n");

    /* Accept requested features + VERSION_1 */
    uint32_t guest_lo = feat_lo & accepted_features_lo;
    uint32_t guest_hi = feat_hi & (0x01 | required_features_hi_mask); /* VERSION_1 */

    mmio_write32(common + VIRTIO_COMMON_GFSELECT, 0);
    dsb();
    mmio_write32(common + VIRTIO_COMMON_GF, guest_lo);
    dsb();
    mmio_write32(common + VIRTIO_COMMON_GFSELECT, 1);
    dsb();
    mmio_write32(common + VIRTIO_COMMON_GF, guest_hi);
    dsb();

    uart_puts("  Accepted: lo=");
    uart_puthex(guest_lo);
    uart_puts(" hi=");
    uart_puthex(guest_hi);
    uart_puts("\n");

    /* Step 5: FEATURES_OK */
    mmio_write8(common + VIRTIO_COMMON_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    dsb();

    uint8_t status = mmio_read8(common + VIRTIO_COMMON_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        uart_puts("  ERROR: device rejected features\n");
        mmio_write8(common + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    (void)pci;
    return 0;
}

int virtio_pci_setup_queue(struct virtio_pci_dev *vpci, struct virtqueue *vq,
                           uint16_t queue_index, uint16_t desired_size)
{
    uintptr_t common = vpci->common_cfg;

    mmio_write16(common + VIRTIO_COMMON_Q_SELECT, queue_index);
    dsb();

    uint16_t max_size = mmio_read16(common + VIRTIO_COMMON_Q_SIZE);
    if (max_size == 0) {
        uart_puts("  Queue ");
        uart_putdec(queue_index);
        uart_puts(" not available\n");
        return -1;
    }

    uint16_t qsize = desired_size;
    if (qsize > max_size)
        qsize = max_size;

    mmio_write16(common + VIRTIO_COMMON_Q_SIZE, qsize);
    dsb();

    uint16_t notify_off = mmio_read16(common + VIRTIO_COMMON_Q_NOTIFYOFF);
    uintptr_t notify_addr = vpci->notify_base +
        (uintptr_t)notify_off * vpci->notify_off_mul;

    virtqueue_init(vq, qsize, queue_index, notify_addr);

    uintptr_t desc_addr  = (uintptr_t)vq->desc;
    uintptr_t avail_addr = (uintptr_t)vq->avail;
    uintptr_t used_addr  = (uintptr_t)vq->used;

    mmio_write32(common + VIRTIO_COMMON_Q_DESCLO,  (uint32_t)(desc_addr & 0xFFFFFFFF));
    mmio_write32(common + VIRTIO_COMMON_Q_DESCHI,  (uint32_t)(desc_addr >> 32));
    mmio_write32(common + VIRTIO_COMMON_Q_AVAILLO, (uint32_t)(avail_addr & 0xFFFFFFFF));
    mmio_write32(common + VIRTIO_COMMON_Q_AVAILHI, (uint32_t)(avail_addr >> 32));
    mmio_write32(common + VIRTIO_COMMON_Q_USEDLO,  (uint32_t)(used_addr & 0xFFFFFFFF));
    mmio_write32(common + VIRTIO_COMMON_Q_USEDHI,  (uint32_t)(used_addr >> 32));
    dsb();

    mmio_write16(common + VIRTIO_COMMON_Q_ENABLE, 1);
    dsb();

    return 0;
}

void virtio_pci_set_driver_ok(struct virtio_pci_dev *vpci) {
    uintptr_t common = vpci->common_cfg;
    mmio_write8(common + VIRTIO_COMMON_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    dsb();
}
