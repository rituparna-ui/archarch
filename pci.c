/*
 * PCI ECAM driver for QEMU virt machine.
 * Scans bus 0 for devices, assigns BARs from a simple allocator.
 */
#include "pci.h"
#include "gic.h"
#include "uart.h"

/* Simple allocators for BAR assignment */
static uintptr_t mmio32_alloc = PCI_MMIO32_BASE + 0x01000000UL; /* skip first 16MB used by QEMU */
static uintptr_t pio_alloc = PCI_PIO_BASE + 0x1000UL;           /* I/O port allocator */

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    return mmio_read32(pci_ecam_addr(bus, dev, func, offset));
}

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    return mmio_read16(pci_ecam_addr(bus, dev, func, offset));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    return mmio_read8(pci_ecam_addr(bus, dev, func, offset));
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val) {
    mmio_write32(pci_ecam_addr(bus, dev, func, offset), val);
}

void pci_config_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint16_t val) {
    mmio_write16(pci_ecam_addr(bus, dev, func, offset), val);
}

static uintptr_t alloc_mmio32(uint32_t size) {
    mmio32_alloc = (mmio32_alloc + size - 1) & ~((uintptr_t)size - 1);
    uintptr_t addr = mmio32_alloc;
    mmio32_alloc += size;
    return addr;
}

static uintptr_t alloc_pio(uint32_t size) {
    pio_alloc = (pio_alloc + size - 1) & ~((uintptr_t)size - 1);
    uintptr_t addr = pio_alloc;
    pio_alloc += size;
    return addr;
}

static void pci_assign_bars(struct pci_device *dev) {
    uint8_t bus = dev->bus, d = dev->dev, func = dev->func;

    for (int i = 0; i < 6; i++) {
        uint16_t bar_off = PCI_BAR0 + (uint16_t)(i * 4);

        /* Save original, write all 1s to probe size */
        uint32_t orig = pci_config_read32(bus, d, func, bar_off);
        pci_config_write32(bus, d, func, bar_off, 0xFFFFFFFF);
        uint32_t sized = pci_config_read32(bus, d, func, bar_off);
        pci_config_write32(bus, d, func, bar_off, orig);

        if (sized == 0 || sized == 0xFFFFFFFF) {
            dev->bar[i] = 0;
            dev->bar_addr[i] = 0;
            continue;
        }

        if (sized & 1) {
            /* I/O BAR */
            uint32_t mask = sized & 0xFFFFFFFC;
            uint32_t size = (~mask + 1) & 0xFFFF;
            uintptr_t addr = alloc_pio(size);
            /* Write the I/O base address (low bit stays 1 to indicate I/O) */
            uint32_t pio_offset = (uint32_t)(addr - PCI_PIO_BASE);
            pci_config_write32(bus, d, func, bar_off, pio_offset | 1);
            dev->bar[i] = pio_offset | 1;
            dev->bar_addr[i] = addr;

            uart_puts("  BAR");
            uart_putdec((uint64_t)i);
            uart_puts(": [I/O] addr=");
            uart_puthex(addr);
            uart_puts(" size=");
            uart_puthex((uint64_t)size);
            uart_puts("\n");
            continue;
        }

        /* Memory BAR */
        uint8_t type = (sized >> 1) & 3;
        uint32_t mask = sized & 0xFFFFFFF0;
        uint32_t size = ~mask + 1;

        if (type == 0x02) {
            /*
             * 64-bit BAR: uses this BAR + next BAR for upper 32 bits.
             * We assign a 32-bit MMIO address (fits in low 4GB) and
             * set the upper 32 bits to 0.
             */
            uintptr_t addr = alloc_mmio32(size);
            pci_config_write32(bus, d, func, bar_off, (uint32_t)addr);
            pci_config_write32(bus, d, func, bar_off + 4, 0); /* upper 32 = 0 */
            dev->bar[i] = (uint32_t)addr;
            dev->bar_addr[i] = addr;
            dev->bar[i + 1] = 0;
            dev->bar_addr[i + 1] = 0;

            uart_puts("  BAR");
            uart_putdec((uint64_t)i);
            uart_puts(": [MEM64] addr=");
            uart_puthex(addr);
            uart_puts(" size=");
            uart_puthex((uint64_t)size);
            uart_puts("\n");

            i++; /* skip next BAR (upper 32 bits) */
            continue;
        }

        /* 32-bit MMIO BAR */
        uintptr_t addr = alloc_mmio32(size);
        pci_config_write32(bus, d, func, bar_off, (uint32_t)addr);
        dev->bar[i] = (uint32_t)addr;
        dev->bar_addr[i] = addr;

        uart_puts("  BAR");
        uart_putdec((uint64_t)i);
        uart_puts(": [MEM32] addr=");
        uart_puthex(addr);
        uart_puts(" size=");
        uart_puthex((uint64_t)size);
        uart_puts("\n");
    }
}

void pci_enable_device(struct pci_device *dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->dev, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_IO | PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER;
    /* Make sure Interrupt Disable bit (bit 10) is NOT set */
    cmd &= ~(1 << 10);
    pci_config_write16(dev->bus, dev->dev, dev->func, PCI_COMMAND, cmd);

    /* Read interrupt pin (1=INTA, 2=INTB, 3=INTC, 4=INTD, 0=none) */
    dev->irq_pin = pci_config_read8(dev->bus, dev->dev, dev->func, PCI_INTERRUPT_PIN);

    uart_puts("  IRQ pin=");
    uart_putdec(dev->irq_pin);
    uart_puts("\n");
}

uint32_t pci_get_irq(struct pci_device *dev) {
    if (dev->irq_pin == 0)
        return 0;
    /*
     * QEMU virt PCI interrupt swizzle (from device tree interrupt-map):
     *   IRQ = SPI((pin - 1 + dev_slot) % 4 + 3) = GIC_SPI(3 + (pin-1+slot)%4)
     *
     * Device 0 INTA -> SPI 3 (IRQ 35)
     * Device 1 INTA -> SPI 4 (IRQ 36)
     * Device 2 INTA -> SPI 5 (IRQ 37)
     * Device 3 INTA -> SPI 6 (IRQ 38)
     * etc.
     */
    uint32_t spi = 3 + ((uint32_t)(dev->irq_pin - 1) + (uint32_t)dev->dev) % 4;
    return GIC_SPI(spi);
}

void pci_enumerate(void) {
    uart_puts("[PCI] Enumerating bus 0...\n");
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint16_t vendor = pci_config_read16(0, dev, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF)
            continue;
        uint16_t device = pci_config_read16(0, dev, 0, PCI_DEVICE_ID);
        uint32_t class_rev = pci_config_read32(0, dev, 0, PCI_CLASS_REVISION);
        uart_puts("[PCI] ");
        uart_putdec(dev);
        uart_puts(": vendor=");
        uart_puthex(vendor);
        uart_puts(" device=");
        uart_puthex(device);
        uart_puts(" class=");
        uart_puthex(class_rev >> 8);
        uart_puts("\n");
    }
}

int pci_find_virtio_rng(struct pci_device *out) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint16_t vendor = pci_config_read16(0, dev, 0, PCI_VENDOR_ID);
        if (vendor != VIRTIO_PCI_VENDOR)
            continue;

        uint16_t device = pci_config_read16(0, dev, 0, PCI_DEVICE_ID);
        if (device != VIRTIO_PCI_DEVICE_RNG_TRANSITIONAL &&
            device != VIRTIO_PCI_DEVICE_RNG_MODERN)
            continue;

        /* Check subsystem ID for modern non-transitional */
        uint32_t subsys = pci_config_read32(0, dev, 0, PCI_SUBSYSTEM);
        (void)subsys;

        out->bus = 0;
        out->dev = dev;
        out->func = 0;
        out->vendor_id = vendor;
        out->device_id = device;

        uart_puts("[PCI] Found virtio-rng at slot ");
        uart_putdec(dev);
        uart_puts("\n");

        pci_assign_bars(out);
        pci_enable_device(out);
        return 1;
    }
    return 0;
}
int pci_find_virtio_blk(struct pci_device *out) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint16_t vendor = pci_config_read16(0, dev, 0, PCI_VENDOR_ID);
        if (vendor != VIRTIO_PCI_VENDOR)
            continue;

        uint16_t device = pci_config_read16(0, dev, 0, PCI_DEVICE_ID);
        if (device != VIRTIO_PCI_DEVICE_BLK_TRANSITIONAL &&
            device != VIRTIO_PCI_DEVICE_BLK_MODERN)
            continue;

        out->bus = 0;
        out->dev = dev;
        out->func = 0;
        out->vendor_id = vendor;
        out->device_id = device;

        uart_puts("[PCI] Found virtio-blk at slot ");
        uart_putdec(dev);
        uart_puts("\n");

        pci_assign_bars(out);
        pci_enable_device(out);
        return 1;
    }
    return 0;
}
int pci_find_virtio_gpu(struct pci_device *out) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint16_t vendor = pci_config_read16(0, dev, 0, PCI_VENDOR_ID);
        if (vendor != VIRTIO_PCI_VENDOR)
            continue;

        uint16_t device = pci_config_read16(0, dev, 0, PCI_DEVICE_ID);
        if (device != VIRTIO_PCI_DEVICE_GPU_TRANSITIONAL &&
            device != VIRTIO_PCI_DEVICE_GPU_MODERN)
            continue;

        out->bus = 0;
        out->dev = dev;
        out->func = 0;
        out->vendor_id = vendor;
        out->device_id = device;

        uart_puts("[PCI] Found virtio-gpu at slot ");
        uart_putdec(dev);
        uart_puts("\n");

        pci_assign_bars(out);
        pci_enable_device(out);
        return 1;
    }
    return 0;
}
int pci_find_virtio_net(struct pci_device *out) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint16_t vendor = pci_config_read16(0, dev, 0, PCI_VENDOR_ID);
        if (vendor != VIRTIO_PCI_VENDOR)
            continue;

        uint16_t device = pci_config_read16(0, dev, 0, PCI_DEVICE_ID);
        if (device != VIRTIO_PCI_DEVICE_NET_TRANSITIONAL &&
            device != VIRTIO_PCI_DEVICE_NET_MODERN)
            continue;

        out->bus = 0;
        out->dev = dev;
        out->func = 0;
        out->vendor_id = vendor;
        out->device_id = device;

        uart_puts("[PCI] Found virtio-net at slot ");
        uart_putdec(dev);
        uart_puts("\n");

        pci_assign_bars(out);
        pci_enable_device(out);
        return 1;
    }
    return 0;
}
