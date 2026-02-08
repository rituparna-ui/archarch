/*
 * Virtio entropy device (RNG) driver over PCI.
 * Uses shared virtio PCI init helpers.
 */
#include "virtio_rng.h"
#include "uart.h"

int virtio_rng_init(struct virtio_rng *dev) {
    uart_puts("\n[VIRTIO-RNG] === Init ===\n");

    if (!pci_find_virtio_rng(&dev->pci)) {
        uart_puts("[VIRTIO-RNG] Device not found\n");
        return -1;
    }

    if (virtio_pci_parse_caps(&dev->pci, &dev->vpci) < 0)
        return -1;

    /* RNG has no device-specific features */
    if (virtio_pci_init_device(&dev->pci, &dev->vpci, 0, 0) < 0)
        return -1;

    /* Setup queue 0 (requestq) */
    if (virtio_pci_setup_queue(&dev->vpci, &dev->vq, 0, 16) < 0)
        return -1;

    virtio_pci_set_driver_ok(&dev->vpci);

    uart_puts("[VIRTIO-RNG] === Ready ===\n\n");
    return 0;
}

int virtio_rng_read(struct virtio_rng *dev, void *buf, uint32_t len) {
    uint16_t desc_idx = virtqueue_add_buf_write(&dev->vq, buf, len);
    if (desc_idx == 0xFFFF)
        return -1;

    virtqueue_kick(&dev->vq);

    uint16_t used_idx;
    uint32_t written;
    uint64_t timeout = 10000000;
    while (!virtqueue_get_used(&dev->vq, &used_idx, &written)) {
        if (--timeout == 0) {
            uart_puts("[VIRTIO-RNG] Timeout!\n");
            return -1;
        }
    }

    return (int)written;
}
