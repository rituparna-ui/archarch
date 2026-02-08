#ifndef VIRTIO_PCI_H
#define VIRTIO_PCI_H

#include "types.h"

/*
 * Virtio PCI capability types (virtio spec 4.1.4)
 */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

/*
 * Virtio PCI capability structure (in PCI config space)
 */
struct virtio_pci_cap {
    uint8_t  cap_vndr;      /* PCI cap ID: 0x09 */
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cfg_type;      /* VIRTIO_PCI_CAP_* */
    uint8_t  bar;           /* BAR index */
    uint8_t  id;            /* multiple caps of same type */
    uint8_t  padding[2];
    uint32_t offset;        /* offset within BAR */
    uint32_t length;        /* length of structure */
};

/*
 * Common configuration structure (virtio spec 4.1.4.3)
 * Accessed via MMIO at BAR[cap.bar] + cap.offset
 */
#define VIRTIO_COMMON_DFSELECT      0x00  /* u32 */
#define VIRTIO_COMMON_DF            0x04  /* u32 */
#define VIRTIO_COMMON_GFSELECT      0x08  /* u32 */
#define VIRTIO_COMMON_GF            0x0C  /* u32 */
#define VIRTIO_COMMON_MSIX          0x10  /* u16 */
#define VIRTIO_COMMON_NUMQ          0x12  /* u16 */
#define VIRTIO_COMMON_STATUS        0x14  /* u8  */
#define VIRTIO_COMMON_CFGGEN        0x15  /* u8  */
#define VIRTIO_COMMON_Q_SELECT      0x16  /* u16 */
#define VIRTIO_COMMON_Q_SIZE        0x18  /* u16 */
#define VIRTIO_COMMON_Q_MSIX        0x1A  /* u16 */
#define VIRTIO_COMMON_Q_ENABLE      0x1C  /* u16 */
#define VIRTIO_COMMON_Q_NOTIFYOFF   0x1E  /* u16 */
#define VIRTIO_COMMON_Q_DESCLO      0x20  /* u32 */
#define VIRTIO_COMMON_Q_DESCHI      0x24  /* u32 */
#define VIRTIO_COMMON_Q_AVAILLO     0x28  /* u32 */
#define VIRTIO_COMMON_Q_AVAILHI     0x2C  /* u32 */
#define VIRTIO_COMMON_Q_USEDLO      0x30  /* u32 */
#define VIRTIO_COMMON_Q_USEDHI      0x34  /* u32 */

/* Device status bits (virtio spec 2.1) */
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FAILED        128

/* Feature bits */
#define VIRTIO_F_VERSION_1          (1UL << 32)

/* Resolved virtio PCI regions */
struct virtio_pci_dev {
    uintptr_t common_cfg;       /* MMIO address of common config */
    uintptr_t notify_base;      /* MMIO address of notify region */
    uint32_t  notify_off_mul;   /* notify offset multiplier */
    uintptr_t isr_cfg;          /* MMIO address of ISR */
    uintptr_t device_cfg;       /* MMIO address of device-specific config */
};

/* Forward declarations */
struct pci_device;
struct virtqueue;

/* Parse PCI capability list to find virtio config regions */
int virtio_pci_parse_caps(struct pci_device *pci, struct virtio_pci_dev *vpci);

/*
 * Run virtio init steps 1-5 (reset through FEATURES_OK).
 * accepted_features_lo: device-specific feature bits to accept (low 32).
 * required_features_hi_mask: additional high feature bits beyond VERSION_1.
 */
int virtio_pci_init_device(struct pci_device *pci, struct virtio_pci_dev *vpci,
                           uint32_t accepted_features_lo,
                           uint32_t required_features_hi_mask);

/* Setup a single virtqueue (step 6 helper) */
int virtio_pci_setup_queue(struct virtio_pci_dev *vpci, struct virtqueue *vq,
                           uint16_t queue_index, uint16_t desired_size);

/* Set DRIVER_OK (step 7) */
void virtio_pci_set_driver_ok(struct virtio_pci_dev *vpci);

#endif
