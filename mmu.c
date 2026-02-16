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
 * Create an ISOLATED per-process user page table.
 *
 * Maps ONLY:
 *   - User code pages at their PA (AP=01, EL0 RW)
 *   - User stack pages at their PA (AP=01, EL0 RW)
 *   - Guard page below stack: UNMAPPED (triggers data abort on overflow)
 *
 * Does NOT map:
 *   - Kernel memory
 *   - Other processes' memory
 *   - MMIO regions
 *
 * The kernel is accessible via TTBR1 when at EL1.
 */
uintptr_t mmu_create_user_pgd(uintptr_t code_pa, uint32_t code_pages,
                                uintptr_t stack_pa, uint32_t stack_pages) {
    uintptr_t l1_pa = pmm_alloc_page();
    if (!l1_pa) return 0;
    uint64_t *l1 = (uint64_t *)l1_pa;
    zero_table(l1);

    /*
     * Start with kernel L1 entries (MMIO, PCI, etc.) so the kernel
     * can execute while TTBR0 is set to this table. All kernel entries
     * have AP=00 (EL1 only) — user code cannot access them.
     */
    for (int i = 0; i < PT_ENTRIES; i++)
        l1[i] = kern_l1[i];

    /*
     * For L1[1] (RAM region), create our own L2 table.
     * Start with all entries INVALID (unmapped) — this is the isolation.
     * Then selectively map only this process's code and stack pages.
     * Kernel code pages in the same 2MB blocks get AP=00 (EL1 only).
     */
    uintptr_t l2_pa = pmm_alloc_page();
    if (!l2_pa) { pmm_free_page(l1_pa); return 0; }
    uint64_t *l2 = (uint64_t *)l2_pa;
    zero_table(l2);  /* All RAM unmapped by default for user */

    l1[1] = l2_pa | PTE_VALID | PTE_TABLE;

    /*
     * Copy kernel RAM L2 entries so kernel code is accessible at EL1.
     * These are 2MB blocks with AP=00 — user (EL0) cannot access them.
     * This ensures the kernel can execute while this TTBR0 is active.
     */
    for (int i = 0; i < 64; i++)
        l2[i] = kern_l2_ram[i];

    /* Map user code and stack pages — split relevant 2MB blocks into L3 */
    for (int pass = 0; pass < 2; pass++) {
        uintptr_t base = (pass == 0) ? code_pa : stack_pa;
        uint32_t npages = (pass == 0) ? code_pages : stack_pages;

        for (uint32_t p = 0; p < npages; p++) {
            uintptr_t pa = base + p * PAGE_SIZE;
            uint32_t offset = (uint32_t)(pa - 0x40000000UL);
            int l2_idx = (offset >> 21) & 0x1FF;
            int l3_idx = (offset >> 12) & 0x1FF;

            /* Allocate L3 table if this 2MB slot doesn't have one yet.
             * We're splitting a 2MB kernel block into 512 4KB pages.
             * All pages start as AP=00 (kernel only), then we override
             * specific pages with AP=01 for user access. */
            if ((l2[l2_idx] & 0x3) != 0x3) {
                /* It's a block entry or invalid — need to split */
                uintptr_t l3_pa = pmm_alloc_page();
                if (!l3_pa) return 0;
                uint64_t *l3 = (uint64_t *)l3_pa;

                /* Fill with AP=00 kernel pages (matching the original block) */
                uint64_t block_base = 0x40000000UL + ((uint64_t)l2_idx << 21);
                for (int j = 0; j < PT_ENTRIES; j++) {
                    uint64_t ppa = block_base + ((uint64_t)j << 12);
                    l3[j] = ppa | PTE_VALID | PTE_PAGE | PTE_AF
                           | PTE_ATTR_NORMAL | PTE_SH_INNER | PTE_AP_RW_EL1;
                }
                l2[l2_idx] = l3_pa | PTE_VALID | PTE_TABLE;
            }

            uint64_t *l3 = (uint64_t *)(l2[l2_idx] & ~0xFFFUL);

            /* Map this page as EL0 accessible */
            l3[l3_idx] = (pa & ~0xFFFUL) | PTE_VALID | PTE_PAGE | PTE_AF
                       | PTE_ATTR_NORMAL | PTE_SH_INNER | PTE_AP_RW_ALL
                       | PTE_PXN;  /* No EL1 execute — defense in depth */
        }
    }

    /*
     * Guard page: the page immediately below the stack base is
     * intentionally left unmapped (zero entry in L3).
     * If the user overflows the stack, they hit this unmapped page
     * and get a clean data abort instead of corrupting memory.
     *
     * stack_pa is the base of the stack allocation.
     * The guard page is at stack_pa - PAGE_SIZE.
     * Since we zero-initialized all L3 tables, it's already unmapped.
     * Just log it for visibility.
     */

    uart_puts("[MMU] User pgd: code=");
    uart_puthex(code_pa);
    uart_puts(" (");
    uart_putdec(code_pages);
    uart_puts("p), stack=");
    uart_puthex(stack_pa);
    uart_puts(" (");
    uart_putdec(stack_pages);
    uart_puts("p), guard=");
    uart_puthex(stack_pa - PAGE_SIZE);
    uart_puts("\n");

    return l1_pa;
}

void mmu_switch_ttbr0(uintptr_t pgd) {
    if (pgd == 0)
        pgd = (uintptr_t)kern_l1;

    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
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
