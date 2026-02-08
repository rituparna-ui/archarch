#ifndef PCI_H
#define PCI_H

#include "types.h"

/*
 * QEMU virt machine PCI layout:
 *   ECAM base:       0x4010000000 (256 buses)
 *   PIO window:      0x3eff0000 (64K)
 *   32-bit MMIO:     0x10000000 - 0x3efeffff
 *   64-bit MMIO:     0x8000000000 - 0xffffffffff
 */
#define PCI_ECAM_BASE       0x4010000000UL
#define PCI_MMIO32_BASE     0x10000000UL
#define PCI_MMIO32_LIMIT    0x3EFEFFFFFUL
#define PCI_PIO_BASE        0x3EFF0000UL

/* PCI config space offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_CLASS_REVISION  0x08
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_CAP_PTR         0x34
#define PCI_SUBSYSTEM       0x2C

/* PCI command bits */
#define PCI_CMD_IO          (1 << 0)
#define PCI_CMD_MEMORY      (1 << 1)
#define PCI_CMD_BUS_MASTER  (1 << 2)

/* Virtio PCI vendor/device */
#define VIRTIO_PCI_VENDOR   0x1AF4
/* Transitional device IDs: 0x1000-0x103F, modern: 0x1040+ */
#define VIRTIO_PCI_DEVICE_RNG_TRANSITIONAL 0x1005
#define VIRTIO_PCI_DEVICE_RNG_MODERN       0x1044
#define VIRTIO_PCI_DEVICE_BLK_TRANSITIONAL 0x1001
#define VIRTIO_PCI_DEVICE_BLK_MODERN       0x1042
#define VIRTIO_PCI_DEVICE_NET_TRANSITIONAL 0x1000
#define VIRTIO_PCI_DEVICE_NET_MODERN       0x1041
#define VIRTIO_PCI_DEVICE_GPU_TRANSITIONAL 0x1050
#define VIRTIO_PCI_DEVICE_GPU_MODERN       0x1050  /* 0x1040 + 16 */

/* PCI capability IDs */
#define PCI_CAP_ID_VENDOR   0x09

/* PCI interrupt pin / line offsets */
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

/* Discovered PCI device info */
struct pci_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  irq_pin;       /* PCI interrupt pin (1=INTA, 2=INTB, ...) */
    uint32_t bar[6];
    uintptr_t bar_addr[6];  /* mapped addresses */
};

/*
 * Get the GIC IRQ ID for a PCI device based on its interrupt pin.
 * QEMU virt: INTA=SPI3(35), INTB=SPI4(36), INTC=SPI5(37), INTD=SPI6(38)
 * Returns 0 if no interrupt pin assigned.
 */
uint32_t pci_get_irq(struct pci_device *dev);

/* ECAM config space access */
static inline uintptr_t pci_ecam_addr(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    return PCI_ECAM_BASE
        | ((uintptr_t)bus  << 20)
        | ((uintptr_t)dev  << 15)
        | ((uintptr_t)func << 12)
        | (uintptr_t)offset;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
uint8_t  pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val);
void     pci_config_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint16_t val);

int  pci_find_virtio_rng(struct pci_device *out);
int  pci_find_virtio_blk(struct pci_device *out);
int  pci_find_virtio_net(struct pci_device *out);
int  pci_find_virtio_gpu(struct pci_device *out);
void pci_assign_bars(struct pci_device *dev);
void pci_enable_device(struct pci_device *dev);
void pci_enumerate(void);

#endif
