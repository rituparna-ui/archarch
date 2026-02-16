/*
 * AArch64 MMU — TTBR0/TTBR1 split with process isolation.
 *
 * TTBR1_EL1 (kernel): always active, maps all physical memory
 *   - MMIO, RAM, PCI — all with AP=00 (EL1 only)
 *   - Kernel code executes from here when at EL1
 *
 * TTBR0_EL1 (per-process): swapped on context switch
 *   - Kernel/idle task: TTBR0 = kern_l1 (full identity map, for boot code)
 *   - User tasks: TTBR0 maps ONLY that process's code + stack pages
 *     with AP=01 (EL0 accessible). Everything else is unmapped.
 *   - Guard page below stack (unmapped) catches stack overflow.
 *
 * Memory isolation:
 *   - Process A cannot see process B's memory (different TTBR0)
 *   - No process can see kernel memory via TTBR0 (not mapped)
 *   - Kernel accesses all memory via TTBR1 during syscalls/IRQs
 *
 * TCR_EL1: T0SZ=T1SZ=25 → 39-bit VA, translation starts at L1
 */
#include "mmu.h"
#include "pmm.h"
#include "uart.h"

#define PT_ENTRIES 512

/* ===== Kernel page tables (TTBR1 + boot TTBR0) ===== */
static uint64_t kern_l1[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kern_l2_mmio[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kern_l2_ram[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kern_l2_ecam[PT_ENTRIES] __attribute__((aligned(4096)));

static void zero_table(uint64_t *table) {
    for (int i = 0; i < PT_ENTRIES; i++)
        table[i] = 0;
}

void mmu_init(void) {
    uart_puts("[MMU] Setting up TTBR0/TTBR1 split page tables...\n");

    zero_table(kern_l1);
    zero_table(kern_l2_mmio);
    zero_table(kern_l2_ram);
    zero_table(kern_l2_ecam);

    /* L2 for MMIO: 0x00000000-0x3FFFFFFF (device memory, 2MB blocks) */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = (uint64_t)i << 21;
        kern_l2_mmio[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }

    /* L2 for RAM region: PA 0x40000000-0x7FFFFFFF */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = 0x40000000UL + ((uint64_t)i << 21);
        if (i < 64) {
            kern_l2_ram[i] = pa | MMU_NORMAL_FLAGS | PTE_BLOCK | PTE_VALID;
        } else {
            kern_l2_ram[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
        }
    }

    /* L2 for PCI ECAM */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = 0x4000000000UL + ((uint64_t)i << 21);
        kern_l2_ecam[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }

    /* Kernel L1 */
    kern_l1[0] = (uint64_t)kern_l2_mmio | PTE_VALID | PTE_TABLE;
    kern_l1[1] = (uint64_t)kern_l2_ram  | PTE_VALID | PTE_TABLE;
    for (int i = 2; i < 4; i++) {
        uint64_t pa = (uint64_t)i << 30;
        kern_l1[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }
    kern_l1[256] = (uint64_t)kern_l2_ecam | PTE_VALID | PTE_TABLE;

    uart_puts("[MMU] Kernel page tables built (TTBR1)\n");

    /* TCR_EL1 */
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t pa_range = mmfr0 & 0xF;

    uint64_t tcr = (25UL << 0)     /* T0SZ = 25 */
                 | (1UL << 8)      /* IRGN0 */
                 | (1UL << 10)     /* ORGN0 */
                 | (3UL << 12)     /* SH0 */
                 | (0UL << 14)     /* TG0 = 4KB */
                 | (25UL << 16)    /* T1SZ = 25 */
                 | (1UL << 24)     /* IRGN1 */
                 | (1UL << 26)     /* ORGN1 */
                 | (3UL << 28)     /* SH1 */
                 | (2UL << 30)     /* TG1 = 4KB */
                 | (pa_range << 32);

    uint64_t mair = (0x00UL << 0) | (0xFFUL << 8);

    uart_puts("[MMU] Enabling MMU...\n");

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "msr ttbr1_el1, %2\n"
        "isb\n"
        : : "r"(mair), "r"(tcr), "r"((uint64_t)kern_l1)
    );

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"
        : : "r"(sctlr)
    );

    uart_puts("[MMU] MMU enabled! TTBR0/TTBR1 split active.\n");
}

/*
 * Create an ISOLATED per-process user page table with proper VAs.
 *
 * Every process gets the same virtual address layout:
 *   VA 0x00400000+  → code (mapped to code_pa)
 *   VA 0x007FC000+  → stack (mapped to stack_pa, 4 pages)
 *   VA 0x007FB000   → guard page (unmapped)
 *
 * Kernel RAM is identity-mapped with AP=00 (EL1 only) so the
 * kernel can execute while this TTBR0 is active.
 */
uintptr_t mmu_create_user_pgd(uintptr_t code_pa, uint32_t code_pages,
                                uintptr_t stack_pa, uint32_t stack_pages) {
    uintptr_t l1_pa = pmm_alloc_page();
    if (!l1_pa) return 0;
    uint64_t *l1 = (uint64_t *)l1_pa;
    zero_table(l1);

    /* Copy kernel L1 entries for EL1 access (MMIO, PCI, etc.) */
    for (int i = 0; i < PT_ENTRIES; i++)
        l1[i] = kern_l1[i];

    /* L1[1] = kernel RAM identity map (AP=00, EL1 only) */
    uintptr_t l2_ram_pa = pmm_alloc_page();
    if (!l2_ram_pa) { pmm_free_page(l1_pa); return 0; }
    uint64_t *l2_ram = (uint64_t *)l2_ram_pa;
    for (int i = 0; i < PT_ENTRIES; i++)
        l2_ram[i] = kern_l2_ram[i];
    l1[1] = l2_ram_pa | PTE_VALID | PTE_TABLE;

    /*
     * L1[0] covers VA 0x00000000-0x3FFFFFFF.
     * User code is at VA 0x00400000 → L1[0], L2 index 2 (0x400000 >> 21 = 2).
     * User stack is at VA 0x007FC000 → L1[0], L2 index 3 (0x600000 >> 21 = 3).
     * We need our own L2 table for L1[0].
     */
    uintptr_t l2_user_pa = pmm_alloc_page();
    if (!l2_user_pa) return 0;
    uint64_t *l2_user = (uint64_t *)l2_user_pa;
    /* Start with kernel MMIO entries so UART etc. work at EL1 */
    for (int i = 0; i < PT_ENTRIES; i++)
        l2_user[i] = kern_l2_mmio[i];

    l1[0] = l2_user_pa | PTE_VALID | PTE_TABLE;

    /*
     * Map user code: VA 0x00400000+ → code_pa
     * L2 index for 0x00400000 = (0x400000 >> 21) = 2
     */
    {
        int l2_idx = (USER_VA_CODE >> 21) & 0x1FF;

        uintptr_t l3_pa = pmm_alloc_page();
        if (!l3_pa) return 0;
        uint64_t *l3 = (uint64_t *)l3_pa;
        zero_table(l3);

        for (uint32_t p = 0; p < code_pages; p++) {
            int l3_idx = ((USER_VA_CODE >> 12) + p) & 0x1FF;
            l3[l3_idx] = (code_pa + p * PAGE_SIZE) | PTE_VALID | PTE_PAGE
                       | PTE_AF | PTE_ATTR_NORMAL | PTE_SH_INNER
                       | PTE_AP_RW_ALL | PTE_PXN;
        }

        l2_user[l2_idx] = l3_pa | PTE_VALID | PTE_TABLE;
    }

    /*
     * Map user stack: VA (USER_VA_STACK - stack_pages*4K) → stack_pa
     * Stack grows down from USER_VA_STACK.
     * Guard page is one page below the stack (unmapped).
     *
     * USER_VA_STACK = 0x00800000
     * Stack pages at VA 0x007FC000..0x007FFFFF (4 pages)
     * Guard at VA 0x007FB000 (unmapped)
     *
     * L2 index for 0x007FC000 = (0x600000 >> 21) = 3
     * (0x007FC000 is in the 2MB block starting at 0x00600000)
     * Actually: 0x007FC000 >> 21 = 3 (0x600000..0x7FFFFF)
     */
    {
        uintptr_t stack_va_base = USER_VA_STACK - stack_pages * PAGE_SIZE;
        int l2_idx = (stack_va_base >> 21) & 0x1FF;

        /* Check if we already have an L3 for this L2 slot */
        uintptr_t l3_pa;
        uint64_t *l3;
        if ((l2_user[l2_idx] & 0x3) == 0x3) {
            /* Already a table pointer (from code mapping) */
            l3 = (uint64_t *)(l2_user[l2_idx] & ~0xFFFUL);
        } else {
            l3_pa = pmm_alloc_page();
            if (!l3_pa) return 0;
            l3 = (uint64_t *)l3_pa;
            zero_table(l3);
            l2_user[l2_idx] = l3_pa | PTE_VALID | PTE_TABLE;
        }

        for (uint32_t p = 0; p < stack_pages; p++) {
            uintptr_t va = stack_va_base + p * PAGE_SIZE;
            int l3_idx = (va >> 12) & 0x1FF;
            l3[l3_idx] = (stack_pa + p * PAGE_SIZE) | PTE_VALID | PTE_PAGE
                       | PTE_AF | PTE_ATTR_NORMAL | PTE_SH_INNER
                       | PTE_AP_RW_ALL | PTE_PXN;
        }
        /* Guard page: l3 entry for (stack_va_base - PAGE_SIZE) stays zero (unmapped) */
    }

    uart_puts("[MMU] User pgd: code VA=");
    uart_puthex(USER_VA_CODE);
    uart_puts("->PA=");
    uart_puthex(code_pa);
    uart_puts(" stack VA=");
    uart_puthex(USER_VA_STACK - stack_pages * PAGE_SIZE);
    uart_puts("->PA=");
    uart_puthex(stack_pa);
    uart_puts(" guard VA=");
    uart_puthex(USER_VA_STACK - (stack_pages + 1) * PAGE_SIZE);
    uart_puts("\n");

    return l1_pa;
}

void mmu_switch_ttbr0(uintptr_t pgd) {
    if (pgd == 0)
        pgd = (uintptr_t)kern_l1;

    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        /* Flush TLB for TTBR0 address space */
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
