/*
 * Interrupt dispatch for virtio PCI devices.
 *
 * QEMU virt maps PCI INTx pins to GIC SPIs:
 *   INTA -> SPI 3 (IRQ 35)
 *   INTB -> SPI 4 (IRQ 36)
 *   INTC -> SPI 5 (IRQ 37)
 *   INTD -> SPI 6 (IRQ 38)
 *
 * Each PCI device's interrupt pin is read from config space offset 0x3D.
 * The virtio ISR register (vpci->isr_cfg) is read to acknowledge the
 * device-level interrupt — bit 0 means "used buffer notification".
 *
 * We maintain a simple dispatch table mapping IRQ IDs to device handlers.
 */
#include "irq.h"
#include "gic.h"
#include "virtio_pci.h"
#include "pci.h"
#include "uart.h"

/* Interrupt pending flags — set by ISR, cleared by main loop */
volatile int irq_rng_pending;
volatile int irq_blk_pending;
volatile int irq_net_rx_pending;

/* Dispatch table entry */
struct irq_dev_entry {
    struct virtio_pci_dev *vpci;
    volatile int          *flag;
    uint32_t               irq_id;
    int                    active;
};

#define MAX_IRQ_DEVS 8
static struct irq_dev_entry irq_devs[MAX_IRQ_DEVS];
static int irq_dev_count;

void irq_init(void) {
    irq_rng_pending = 0;
    irq_blk_pending = 0;
    irq_net_rx_pending = 0;
    irq_dev_count = 0;
    for (int i = 0; i < MAX_IRQ_DEVS; i++)
        irq_devs[i].active = 0;
}

static void register_dev(struct virtio_pci_dev *vpci, volatile int *flag,
                          uint32_t irq_id)
{
    if (irq_dev_count >= MAX_IRQ_DEVS) {
        uart_puts("[IRQ] Too many devices!\n");
        return;
    }
    struct irq_dev_entry *e = &irq_devs[irq_dev_count++];
    e->vpci   = vpci;
    e->flag   = flag;
    e->irq_id = irq_id;
    e->active = 1;

    /* Enable this IRQ in the GIC */
    gic_set_priority(irq_id, 0xA0);
    gic_enable_irq(irq_id);

    /* Verify it's enabled */
    uart_puts("[IRQ] Registered device on IRQ ");
    uart_putdec(irq_id);
    uart_puts("\n");
}

void irq_register_rng(struct virtio_pci_dev *vpci, uint32_t irq_id) {
    register_dev(vpci, &irq_rng_pending, irq_id);
}

void irq_register_blk(struct virtio_pci_dev *vpci, uint32_t irq_id) {
    register_dev(vpci, &irq_blk_pending, irq_id);
}

void irq_register_net(struct virtio_pci_dev *vpci, uint32_t irq_id) {
    register_dev(vpci, &irq_net_rx_pending, irq_id);
}

/*
 * Main IRQ handler — called from the assembly exception vector.
 *
 * Reads IAR to acknowledge, checks all registered devices sharing
 * that IRQ line, reads their ISR status register to identify which
 * device actually fired, sets the corresponding flag.
 */
void irq_handler(void) {
    uint32_t iar = gic_ack();

    /* Spurious interrupt check (ID 1023) */
    if (iar >= 1020)
        return;

    /* Walk registered devices and check which ones share this IRQ */
    for (int i = 0; i < irq_dev_count; i++) {
        struct irq_dev_entry *e = &irq_devs[i];
        if (!e->active || e->irq_id != iar)
            continue;

        /* Read virtio ISR status — clears the interrupt at device level */
        uint8_t isr = mmio_read8(e->vpci->isr_cfg);
        if (isr & 1)
            *e->flag = 1;  /* used buffer notification */
    }

    /* Signal End of Interrupt */
    gic_eoi(iar);
}
