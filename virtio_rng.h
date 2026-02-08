#ifndef VIRTIO_RNG_H
#define VIRTIO_RNG_H

#include "pci.h"
#include "virtio_pci.h"
#include "virtqueue.h"

struct virtio_rng {
    struct pci_device    pci;
    struct virtio_pci_dev vpci;
    struct virtqueue     vq;
};

/*
 * Initialize the virtio-rng device:
 *  1. Parse PCI capabilities to find virtio config regions
 *  2. Perform virtio device initialization sequence
 *  3. Set up virtqueue 0 (the requestq)
 *
 * Returns 0 on success, -1 on failure.
 */
int virtio_rng_init(struct virtio_rng *dev);

/*
 * Request random bytes from the device.
 * Blocks until the device fills the buffer.
 * Returns number of bytes actually received.
 */
int virtio_rng_read(struct virtio_rng *dev, void *buf, uint32_t len);

#endif
