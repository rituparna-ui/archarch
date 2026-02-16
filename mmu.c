/*
 * AArch64 MMU — high VA kernel with TTBR0/TTBR1 split.
 *
 * The boot code in start.S sets up early page tables and enables the MMU.
 * This module manages the kernel page tables (TTBR1) and creates
 * per-process user page tables (TTBR0).
 *
 * Kernel VA: 0xFFFFFF8000000000 + PA (identity offset)
 * User VA: 0x00400000 (code), 0x007FC000 (stack)
 */
#include "mmu.h"
#include "pmm.h"
#include "uart.h"
#include "kva.h"

#define PT_ENTRIES 512

/*
 * Boot page tables — allocated at __kernel_end_phys by start.S.
 * We compute their high VA addresses here.
 */
extern uintptr_t __kernel_end;  /* high VA of kernel end */

/* The boot code places tables at __kernel_end_phys + 0, +4K, +8K, +12K */
static uint64_t *get_boot_l1(void)      { return (uint64_t *)((uintptr_t)&__kernel_end); }
static uint64_t *get_boot_l2_ram(void)  { return (uint64_t *)((uintptr_t)&__kernel_end + 2 * 4096); }

static void zero_table(uint64_t *table) {
    for (int i = 0; i < PT_ENTRIES; i++)
        table[i] = 0;
}

void mmu_init(void) {
    /*
     * MMU is already enabled by start.S boot code.
     * TTBR0 = TTBR1 = boot_l1 (identity map for both ranges).
     *
     * We just log the state. The boot tables are our permanent
     * kernel page tables (TTBR1). TTBR0 will be swapped per-process.
     */
    uart_puts("[MMU] High VA kernel active.\n");
    uart_puts("[MMU] Kernel VA base: ");
    uart_puthex(KERN_VA_OFFSET);
    uart_puts("\n");
    uart_puts("[MMU] TTBR1 (kernel): PA=");
    uart_puthex(kva_to_pa(get_boot_l1()));
    uart_puts("\n");
}

/*
 * Create per-process user page table (TTBR0).
 *
 * Maps:
 *   VA 0x00400000+ → code_pa (AP=01, EL0 RW)
 *   VA 0x007FC000+ → stack_pa (AP=01, EL0 RW)
 *   Guard page at VA 0x007FB000 (unmapped)
 *
 * Also includes kernel identity map (AP=00) so the kernel
 * can execute while this TTBR0 is active. The kernel runs
 * at high VA (TTBR1), but TTBR0 must also resolve kernel
 * addresses because the kernel is linked at high VA and
 * TTBR1 handles those. TTBR0 only needs to handle the low
 * VA range (user space).
 *
 * Actually — with the high VA kernel, TTBR0 does NOT need
 * kernel mappings at all! The kernel runs entirely through
 * TTBR1. TTBR0 only maps user space.
 */
uintptr_t mmu_create_user_pgd(uintptr_t code_pa, uint32_t code_pages,
                                uintptr_t stack_pa, uint32_t stack_pages) {
    /* Allocate L1 table (physical address) */
    uintptr_t l1_pa = pmm_alloc_page();
    if (!l1_pa) return 0;
    uint64_t *l1 = (uint64_t *)phys_to_virt(l1_pa);
    zero_table(l1);

    /*
     * L1[0] covers VA 0x00000000-0x3FFFFFFF.
     * User code at VA 0x00400000 and stack at VA 0x007FC000 are both here.
     */
    uintptr_t l2_pa = pmm_alloc_page();
    if (!l2_pa) { pmm_free_page(l1_pa); return 0; }
    uint64_t *l2 = (uint64_t *)phys_to_virt(l2_pa);
    zero_table(l2);  /* All unmapped — pure user space, no kernel */

    l1[0] = l2_pa | PTE_VALID | PTE_TABLE;

    /* Map user code: VA 0x00400000+ → code_pa */
    {
        int l2_idx = (USER_VA_CODE >> 21) & 0x1FF;

        uintptr_t l3_pa = pmm_alloc_page();
        if (!l3_pa) return 0;
        uint64_t *l3 = (uint64_t *)phys_to_virt(l3_pa);
        zero_table(l3);

        for (uint32_t p = 0; p < code_pages; p++) {
            int l3_idx = ((USER_VA_CODE >> 12) + p) & 0x1FF;
            l3[l3_idx] = (code_pa + p * PAGE_SIZE) | PTE_VALID | PTE_PAGE
                       | PTE_AF | PTE_ATTR_NORMAL | PTE_SH_INNER
                       | PTE_AP_RW_ALL | PTE_PXN;
        }

        l2[l2_idx] = l3_pa | PTE_VALID | PTE_TABLE;
    }

    /* Map user stack: VA 0x007FC000+ → stack_pa */
    {
        uintptr_t stack_va_base = USER_VA_STACK - stack_pages * PAGE_SIZE;
        int l2_idx = (stack_va_base >> 21) & 0x1FF;

        uint64_t *l3;
        if ((l2[l2_idx] & 0x3) == 0x3) {
            l3 = (uint64_t *)phys_to_virt(l2[l2_idx] & ~0xFFFUL);
        } else {
            uintptr_t l3_pa = pmm_alloc_page();
            if (!l3_pa) return 0;
            l3 = (uint64_t *)phys_to_virt(l3_pa);
            zero_table(l3);
            l2[l2_idx] = l3_pa | PTE_VALID | PTE_TABLE;
        }

        for (uint32_t p = 0; p < stack_pages; p++) {
            uintptr_t va = stack_va_base + p * PAGE_SIZE;
            int l3_idx = (va >> 12) & 0x1FF;
            l3[l3_idx] = (stack_pa + p * PAGE_SIZE) | PTE_VALID | PTE_PAGE
                       | PTE_AF | PTE_ATTR_NORMAL | PTE_SH_INNER
                       | PTE_AP_RW_ALL | PTE_PXN;
        }
    }

    uart_puts("[MMU] User pgd(PA=");
    uart_puthex(l1_pa);
    uart_puts("): code VA=");
    uart_puthex(USER_VA_CODE);
    uart_puts("->PA=");
    uart_puthex(code_pa);
    uart_puts(" stack->PA=");
    uart_puthex(stack_pa);
    uart_puts("\n");

    return l1_pa;
}

void mmu_switch_ttbr0(uintptr_t pgd) {
    if (pgd == 0)
        pgd = kva_to_pa(get_boot_l1());  /* PA of kernel page table for idle task */

    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        "tlbi aside1, xzr\n"
        "dsb sy\n"
        "isb\n"
        : : "r"(pgd)
    );
}

/* Legacy stubs */
void mmu_map_user_range(uintptr_t start, uint32_t num_pages) {
    (void)start; (void)num_pages;
}
void mmu_map_user_page(uintptr_t pa) { (void)pa; }
void mmu_map_page(uintptr_t va, uintptr_t pa, uint64_t flags) {
    (void)va; (void)pa; (void)flags;
}
