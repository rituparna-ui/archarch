/*
 * GICv3 driver for QEMU virt machine.
 *
 * Initializes the Distributor (GICD), Redistributor (GICR), and
 * CPU interface via ICC system registers.
 */
#include "gic.h"
#include "uart.h"

/* ---- GICD (Distributor) registers ---- */
#define GICD_CTLR           (GICD_BASE + 0x0000)
#define GICD_TYPER          (GICD_BASE + 0x0004)
#define GICD_IGROUPR(n)     (GICD_BASE + 0x0080 + 4 * (n))
#define GICD_ISENABLER(n)   (GICD_BASE + 0x0100 + 4 * (n))
#define GICD_ICENABLER(n)   (GICD_BASE + 0x0180 + 4 * (n))
#define GICD_IPRIORITYR(n)  (GICD_BASE + 0x0400 + 4 * (n))
#define GICD_ITARGETSR(n)   (GICD_BASE + 0x0800 + 4 * (n))
#define GICD_ICFGR(n)       (GICD_BASE + 0x0C00 + 4 * (n))

/* GICD_CTLR bits for GICv3 */
#define GICD_CTLR_EN_GRP1NS (1 << 1)  /* Enable Group 1 Non-Secure */
#define GICD_CTLR_ARE_NS    (1 << 4)  /* Affinity Routing Enable, NS */

/* ---- GICR (Redistributor) registers ---- */
/* RD_base frame (64KB) */
#define GICR_WAKER          (GICR_BASE + 0x0014)
/* SGI_base frame (next 64KB) */
#define GICR_SGI_BASE       (GICR_BASE + 0x10000)
#define GICR_IGROUPR0       (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0     (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0     (GICR_SGI_BASE + 0x0180)
#define GICR_IPRIORITYR(n)  (GICR_SGI_BASE + 0x0400 + 4 * (n))

#define GICR_WAKER_PSLEEP   (1 << 1)
#define GICR_WAKER_CASLEEP  (1 << 2)

/* ---- ICC system register accessors (AArch64) ---- */

static inline void icc_sre_el1_write(uint64_t val) {
    __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(val)); /* ICC_SRE_EL1 */
    isb();
}

static inline void icc_pmr_el1_write(uint64_t val) {
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"(val));   /* ICC_PMR_EL1 */
    isb();
}

static inline void icc_bpr1_el1_write(uint64_t val) {
    __asm__ volatile("msr S3_0_C12_C12_3, %0" :: "r"(val)); /* ICC_BPR1_EL1 */
    isb();
}

static inline void icc_igrpen1_el1_write(uint64_t val) {
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"(val)); /* ICC_IGRPEN1_EL1 */
    isb();
}

static inline uint64_t icc_iar1_el1_read(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(val)); /* ICC_IAR1_EL1 */
    return val;
}

static inline void icc_eoir1_el1_write(uint64_t val) {
    __asm__ volatile("msr S3_0_C12_C11_1, %0" :: "r"(val)); /* ICC_EOIR1_EL1 */
    isb();
}

void gic_init(void) {
    uart_puts("[GIC] Initializing GICv3...\n");

    /* ---- Distributor init ---- */
    /* Disable distributor while configuring */
    mmio_write32(GICD_CTLR, 0);
    dsb();

    /* Read TYPER to find number of interrupt lines */
    uint32_t typer = mmio_read32(GICD_TYPER);
    uint32_t it_lines = (typer & 0x1F) + 1; /* number of 32-IRQ groups */
    uart_puts("  GICD_TYPER=");
    uart_puthex(typer);
    uart_puts(" ITLinesNumber=");
    uart_putdec(it_lines);
    uart_puts("\n");

    /* Set all SPIs (IRQ 32+) to Group 1 Non-Secure */
    for (uint32_t i = 1; i < it_lines; i++)
        mmio_write32(GICD_IGROUPR(i), 0xFFFFFFFF);

    /* Set all SPIs to level-triggered (ICFGR: 0 = level) */
    for (uint32_t i = 2; i < it_lines * 2; i++)
        mmio_write32(GICD_ICFGR(i), 0x00000000);

    /* Set all SPI priorities to 0xA0 (moderate) */
    for (uint32_t i = 8; i < it_lines * 8; i++)
        mmio_write32(GICD_IPRIORITYR(i), 0xA0A0A0A0);

    /* Disable all SPIs initially */
    for (uint32_t i = 1; i < it_lines; i++)
        mmio_write32(GICD_ICENABLER(i), 0xFFFFFFFF);

    dsb();

    /* Enable distributor: Group 1 NS + Affinity Routing */
    mmio_write32(GICD_CTLR, GICD_CTLR_EN_GRP1NS | GICD_CTLR_ARE_NS);
    dsb();
    isb();

    /* ---- Redistributor init ---- */
    /* Wake up the redistributor */
    uint32_t waker = mmio_read32(GICR_WAKER);
    waker &= ~GICR_WAKER_PSLEEP;
    mmio_write32(GICR_WAKER, waker);

    /* Wait for ChildrenAsleep to clear */
    uint64_t timeout = 1000000;
    while ((mmio_read32(GICR_WAKER) & GICR_WAKER_CASLEEP) && --timeout)
        ;
    if (timeout == 0)
        uart_puts("  WARNING: GICR wakeup timeout\n");

    /* Set SGIs/PPIs (IRQ 0-31) to Group 1 NS */
    mmio_write32(GICR_IGROUPR0, 0xFFFFFFFF);

    /* Set SGI/PPI priorities to 0xA0 */
    for (int i = 0; i < 8; i++)
        mmio_write32(GICR_IPRIORITYR(i), 0xA0A0A0A0);

    dsb();

    /* ---- CPU interface init (ICC system registers) ---- */
    /* Enable system register access */
    icc_sre_el1_write(0x7); /* SRE=1, DFB=1, DIB=1 */

    /* Priority mask: allow all priorities */
    icc_pmr_el1_write(0xFF);

    /* Binary point: no preemption grouping */
    icc_bpr1_el1_write(0);

    /* Enable Group 1 interrupts */
    icc_igrpen1_el1_write(1);

    isb();
    uart_puts("[GIC] GICv3 initialized\n");
}

void gic_enable_irq(uint32_t irq_id) {
    uint32_t reg = irq_id / 32;
    uint32_t bit = irq_id % 32;

    if (irq_id < 32) {
        /* SGI/PPI: use redistributor */
        mmio_write32(GICR_ISENABLER0, 1 << bit);
    } else {
        /* SPI: use distributor */
        mmio_write32(GICD_ISENABLER(reg), 1 << bit);
    }
    dsb();
}

void gic_set_priority(uint32_t irq_id, uint8_t prio) {
    uint32_t reg = irq_id / 4;
    uint32_t shift = (irq_id % 4) * 8;

    uintptr_t addr;
    if (irq_id < 32)
        addr = GICR_IPRIORITYR(reg);
    else
        addr = GICD_IPRIORITYR(reg);

    uint32_t val = mmio_read32(addr);
    val &= ~(0xFFU << shift);
    val |= (uint32_t)prio << shift;
    mmio_write32(addr, val);
}

uint32_t gic_ack(void) {
    return (uint32_t)icc_iar1_el1_read();
}

void gic_eoi(uint32_t irq_id) {
    icc_eoir1_el1_write((uint64_t)irq_id);
}
