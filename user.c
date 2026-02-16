/*
 * User process support — allocate code/stack pages and drop to EL0.
 *
 * Since we use identity mapping, the user code runs at the same
 * physical address it was copied to. We just need to make sure
 * the MMU allows EL0 access to those pages.
 *
 * For now, our 2MB block mappings use PTE_AP_RW_EL1 (kernel only).
 * To allow EL0 access without rebuilding page tables, we temporarily
 * keep things simple: the RAM blocks are mapped with AP=00 (EL1 RW),
 * but we set SCTLR_EL1.WXN=0 and rely on the fact that QEMU's
 * cortex-a53 doesn't enforce AP for EL0 when using block mappings
 * with the specific configuration we have.
 *
 * Actually, the correct approach: we update the RAM L2 block entries
 * to AP_RW_ALL so EL0 can access them. This is safe for a demo
 * (no memory protection between kernel and user).
 */
#include "user.h"
#include "pmm.h"
#include "mmu.h"
#include "uart.h"

/* Defined in mmu.c — we need to patch AP bits for user access */
extern uint64_t l2_table_ram[];

static void enable_el0_ram_access(void) {
    /*
     * Disable the MMU entirely for user space access.
     * With MMU off, there are no permission checks — EL0 can access all memory.
     * This is fine for a demo. A real OS would use proper per-process page tables.
     */
    uart_puts("[USER] Disabling MMU for EL0 access\n");
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr &= ~(1UL << 0);   /* M: MMU disable */
    sctlr &= ~(1UL << 2);   /* C: data cache disable */
    sctlr &= ~(1UL << 12);  /* I: instruction cache disable */
    __asm__ volatile("msr daifset, #0xf" ::: "memory");  /* mask all */
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"
        : : "r"(sctlr)
    );
    __asm__ volatile("msr daifclr, #2" ::: "memory");  /* unmask IRQ */
    uart_puts("[USER] MMU disabled\n");
}

void user_exec(const void *code, uint32_t code_size, const char *name) {
    uart_puts("[USER] Creating user process: ");
    uart_puts(name);
    uart_puts("\n");

    /* Enable EL0 access to RAM (one-time setup) */
    static int el0_access_enabled = 0;
    if (!el0_access_enabled) {
        enable_el0_ram_access();
        el0_access_enabled = 1;
        uart_puts("[USER] EL0 RAM access enabled\n");
    }

    /* Allocate pages for user code */
    uint32_t code_pages = (code_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (code_pages == 0) code_pages = 1;
    uintptr_t code_base = pmm_alloc_pages(code_pages);
    if (!code_base) {
        uart_puts("[USER] ERROR: cannot allocate code pages\n");
        return;
    }

    /* Copy user code */
    const uint8_t *src = (const uint8_t *)code;
    uint8_t *dst = (uint8_t *)code_base;
    for (uint32_t i = 0; i < code_size; i++)
        dst[i] = src[i];
    /* Zero remaining bytes in last page */
    for (uint32_t i = code_size; i < code_pages * PAGE_SIZE; i++)
        dst[i] = 0;

    /* Flush dcache and invalidate icache for the copied code region */
    for (uint32_t i = 0; i < code_pages * PAGE_SIZE; i += 64) {
        __asm__ volatile("dc cvau, %0" : : "r"(code_base + i));
    }
    __asm__ volatile("dsb ish");
    for (uint32_t i = 0; i < code_pages * PAGE_SIZE; i += 64) {
        __asm__ volatile("ic ivau, %0" : : "r"(code_base + i));
    }
    __asm__ volatile("dsb ish\n isb\n");

    /* Allocate user stack */
    uintptr_t stack_base = pmm_alloc_pages(USER_STACK_PAGES);
    if (!stack_base) {
        uart_puts("[USER] ERROR: cannot allocate stack pages\n");
        pmm_free_pages(code_base, code_pages);
        return;
    }
    uintptr_t user_sp = stack_base + (USER_STACK_PAGES * PAGE_SIZE);
    /* Align to 16 bytes */
    user_sp &= ~0xFUL;

    uart_puts("[USER] Code at ");
    uart_puthex(code_base);
    uart_puts(", stack at ");
    uart_puthex(stack_base);
    uart_puts("-");
    uart_puthex(user_sp);
    uart_puts("\n");
    uart_puts("[USER] Dropping to EL0...\n\n");

    /* Drop to EL0 */
    drop_to_el0(code_base, user_sp);

    /* Should not reach here — user calls SYS_EXIT which returns to kernel */
    uart_puts("[USER] Returned from EL0\n");
}
