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
#include "timer.h"
#include "sched.h"
#include "signal.h"
#include "uart.h"

/* Interrupt pending flags — set by ISR, cleared by main loop */
volatile int irq_rng_pending;
volatile int irq_blk_pending;
volatile int irq_net_rx_pending;
volatile int irq_gpu_pending;
volatile int irq_input_pending;
volatile uint64_t irq_timer_ticks;

static int timer_registered;

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
    irq_gpu_pending = 0;
    irq_input_pending = 0;
    irq_timer_ticks = 0;
    timer_registered = 0;
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

void irq_register_gpu(struct virtio_pci_dev *vpci, uint32_t irq_id) {
    register_dev(vpci, &irq_gpu_pending, irq_id);
}

void irq_register_input(struct virtio_pci_dev *vpci, uint32_t irq_id) {
    register_dev(vpci, &irq_input_pending, irq_id);
}

void irq_register_timer(void) {
    timer_registered = 1;
    uart_puts("[IRQ] Timer registered on IRQ ");
    uart_putdec(TIMER_IRQ_ID);
    uart_puts("\n");
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

    /* Timer interrupt (PPI 14 = IRQ 30) */
    if (iar == TIMER_IRQ_ID && timer_registered) {
        irq_timer_ticks++;
        /*
         * IMPORTANT: rearm BEFORE EOI!
         * The physical timer PPI is level-triggered. If we EOI while
         * ISTATUS is still set (TVAL <= 0), the GIC will immediately
         * re-pend the interrupt, causing an IRQ storm.
         * Rearming sets a new TVAL, which clears ISTATUS.
         */
        timer_rearm();
        gic_eoi(iar);
        sched_tick();
        return;
    }

    /* Debug: print unexpected IRQ IDs to catch timer issues */
    if (iar == TIMER_IRQ_ID && !timer_registered) {
        /* Timer fired but not registered yet — just ack and ignore */
        gic_eoi(iar);
        return;
    }

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

/*
 * Called when a synchronous exception from EL0 is NOT an SVC.
 * Print diagnostic info and halt.
 */
void unhandled_sync_el0(void) {
    uint64_t esr, elr, far;
    __asm__ volatile("mrs %0, esr_el1"  : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1"  : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1"  : "=r"(far));

    uint64_t ec = (esr >> 26) & 0x3F;

    uart_puts("\n[FAULT] Task ");
    uart_putdec((uint64_t)sched_current_id());
    uart_puts(" killed: ");

    if (ec == 0x20 || ec == 0x21) {
        uart_puts("instruction abort at ");
        uart_puthex(far);
        uart_puts("\n");
    } else if (ec == 0x24 || ec == 0x25) {
        uint64_t dfsc = esr & 0x3F;
        if (dfsc >= 0x04 && dfsc <= 0x07) {
            uart_puts("SEGFAULT (translation fault) accessing ");
        } else if (dfsc >= 0x0C && dfsc <= 0x0F) {
            uart_puts("SEGFAULT (permission fault) accessing ");
        } else {
            uart_puts("data abort accessing ");
        }
        uart_puthex(far);
        uart_puts(" at PC ");
        uart_puthex(elr);
        uart_puts("\n");
    } else {
        uart_puts("unhandled exception EC=");
        uart_puthex(ec);
        uart_puts(" at PC ");
        uart_puthex(elr);
        uart_puts("\n");
    }

    uart_puts("  ESR=");
    uart_puthex(esr);
    uart_puts(" ELR=");
    uart_puthex(elr);
    uart_puts(" FAR=");
    uart_puthex(far);
    uart_puts("\n");

    /* Send SIGSEGV to the faulting task — if it has a handler, it'll run */
    struct task *t = sched_get_task(sched_current_id());
    if (t && t->sig.handlers[SIGSEGV] > SIG_IGN) {
        /* User has a SIGSEGV handler — deliver it */
        signal_send(sched_current_id(), SIGSEGV);
        return;  /* Will be delivered on return to EL0 */
    }

    /* No handler — kill the task (default action) */
    sched_exit();
}
