/*
 * ARM Generic Timer driver (EL1 physical timer).
 *
 * On QEMU virt, the EL1 physical timer is PPI 14 (GIC IRQ ID 30).
 * Uses CNTPCT_EL0 for reading the counter, CNTP_TVAL_EL0 / CNTP_CTL_EL0
 * for programming the timer interrupt.
 *
 * The EL2 -> EL1 drop in start.S sets CNTHCTL_EL2 to allow EL1
 * access to the physical timer and counter.
 */
#ifndef TIMER_H
#define TIMER_H

#include "types.h"

/* PPI 14 = GIC IRQ ID 30 (16 + 14) */
#define TIMER_IRQ_ID    30

/* Timer control register bits */
#define CNTP_CTL_ENABLE     (1 << 0)
#define CNTP_CTL_IMASK      (1 << 1)
#define CNTP_CTL_ISTATUS    (1 << 2)

static inline uint64_t timer_get_freq(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

static inline uint64_t timer_get_count(void) {
    uint64_t cnt;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    return cnt;
}

static inline void timer_set_tval(uint32_t ticks) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"((uint64_t)ticks));
    isb();
}

static inline void timer_enable(void) {
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)CNTP_CTL_ENABLE));
    isb();
}

static inline void timer_disable(void) {
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)0));
    isb();
}

static inline void timer_mask(void) {
    __asm__ volatile("msr cntp_ctl_el0, %0"
        :: "r"((uint64_t)(CNTP_CTL_ENABLE | CNTP_CTL_IMASK)));
    isb();
}

void timer_init(uint32_t interval_ms);
void timer_rearm(void);
uint64_t timer_ms(void);
uint32_t timer_get_interval(void);

#endif
