#ifndef GIC_H
#define GIC_H

#include "types.h"
#include "kva.h"

/*
 * GICv3 driver for QEMU virt machine.
 *
 * QEMU virt GICv3 memory map (physical):
 *   Distributor (GICD):     0x0800_0000
 *   Redistributor (GICR):   0x080A_0000
 *
 * Accessed via kernel high VA.
 */

#define GICD_BASE       (0x08000000UL + KERN_VA_OFFSET)
#define GICR_BASE       (0x080A0000UL + KERN_VA_OFFSET)

/* SPI number to GIC IRQ ID */
#define GIC_SPI(n)      ((n) + 32)

/* PCI INTx -> SPI mapping on QEMU virt */
#define PCI_INTA_IRQ    GIC_SPI(3)   /* 35 */
#define PCI_INTB_IRQ    GIC_SPI(4)   /* 36 */
#define PCI_INTC_IRQ    GIC_SPI(5)   /* 37 */
#define PCI_INTD_IRQ    GIC_SPI(6)   /* 38 */

void gic_init(void);
void gic_enable_irq(uint32_t irq_id);
void gic_set_priority(uint32_t irq_id, uint8_t prio);
uint32_t gic_ack(void);
void gic_eoi(uint32_t irq_id);

#endif
