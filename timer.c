/*
 * ARM Generic Timer driver.
 *
 * Uses the EL1 physical timer (CNTP) which fires as PPI 14 (IRQ 30).
 * The EL2->EL1 drop in start.S enables physical timer access via
 * CNTHCTL_EL2.
 */
#include "timer.h"
#include "gic.h"
#include "uart.h"

static uint64_t ticks_per_ms;
static uint32_t tick_interval;   /* in counter ticks */
static uint64_t start_count;     /* counter value at init */

void timer_init(uint32_t interval_ms) {
    uint64_t freq = timer_get_freq();
    ticks_per_ms = freq / 1000;
    tick_interval = (uint32_t)(ticks_per_ms * interval_ms);

    uart_puts("[TIMER] Frequency: ");
    uart_putdec(freq);
    uart_puts(" Hz (");
    uart_putdec(freq / 1000000);
    uart_puts(" MHz)\n");
    uart_puts("[TIMER] Tick interval: ");
    uart_putdec(interval_ms);
    uart_puts(" ms (");
    uart_putdec(tick_interval);
    uart_puts(" ticks)\n");

    /* Enable PPI 30 in the GIC redistributor */
    gic_set_priority(TIMER_IRQ_ID, 0xA0);
    gic_enable_irq(TIMER_IRQ_ID);

    /* Record start time */
    start_count = timer_get_count();

    /* Program the timer and enable it */
    timer_set_tval(tick_interval);
    timer_enable();

    uart_puts("[TIMER] Armed and running\n");
}

void timer_rearm(void) {
    timer_set_tval(tick_interval);
}

uint64_t timer_ms(void) {
    uint64_t elapsed = timer_get_count() - start_count;
    return elapsed / ticks_per_ms;
}

uint32_t timer_get_interval(void) {
    return tick_interval;
}
