#ifndef GIC_H
#define GIC_H

#include "types.h"

/*
 * GICv3 driver for QEMU virt machine.
 *
 * QEMU virt GICv3 memory map:
 *   Distributor (GICD):     0x0800_0000
 *   Redistributor (GICR):   0x080A_0000
 *
 * Interrupt ID ranges:
 *   SGI:  0-15
 *   PPI: 16-31
 *   SPI: 32-1019
 *
 * QEMU virt PCI INTx mapping:
 *   INTA -> SPI 3  (IRQ ID 35)
 *   INTB -> SPI 4  (IRQ ID 36)
 *   INTC -> SPI 5  (IRQ ID 37)
 *   INTD -> SPI 6  (IRQ ID 38)
 */

#define GICD_BASE       0x08000000UL
#define GICR_BASE       0x080A0000UL

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
