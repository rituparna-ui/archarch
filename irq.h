#ifndef IRQ_H
#define IRQ_H

#include "types.h"

/*
 * Interrupt dispatch layer.
 *
 * Called from the assembly exception vector. Reads the GIC IAR,
 * dispatches to the appropriate virtio device handler, and signals EOI.
 *
 * Volatile flags are set for the main loop to consume via WFI.
 */

/* Flags set by IRQ handler, polled by main loop */
extern volatile int irq_rng_pending;
extern volatile int irq_blk_pending;
extern volatile int irq_net_rx_pending;
extern volatile int irq_gpu_pending;

/* Called from assembly vector */
void irq_handler(void);

/* Register virtio devices for interrupt dispatch */
struct virtio_pci_dev;

void irq_register_rng(struct virtio_pci_dev *vpci, uint32_t irq_id);
void irq_register_blk(struct virtio_pci_dev *vpci, uint32_t irq_id);
void irq_register_net(struct virtio_pci_dev *vpci, uint32_t irq_id);
void irq_register_gpu(struct virtio_pci_dev *vpci, uint32_t irq_id);

/* Initialize interrupt dispatch (call after gic_init) */
void irq_init(void);

/* Enable/disable IRQs at CPU level */
static inline void irq_enable(void) {
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void irq_disable(void) {
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static inline void wfi(void) {
    __asm__ volatile("wfi" ::: "memory");
}

#endif
