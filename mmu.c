/*
 * AArch64 MMU — TTBR0/TTBR1 split.
 *
 * TTBR1_EL1 (kernel): upper VA range 0xFFFF_xxxx_xxxx_xxxx
 *   - Identity maps PA to VA with 0xFFFF_0000_0000_0000 offset
 *   - Kernel code at VA 0xFFFF000040000000 → PA 0x40000000
 *   - MMIO at VA 0xFFFF000000000000 → PA 0x00000000
 *   - AP=00 (EL1 only), never changes
 *
 * TTBR0_EL1 (user): lower VA range 0x0000_xxxx_xxxx_xxxx
 *   - Per-process page table
 *   - User code mapped at VA 0x00400000 (4MB)
 *   - User stack mapped at VA 0x7FFFF000 (below 2GB)
 *   - AP=01 (EL0+EL1 RW)
 *   - Swapped on context switch
 *
 * TCR_EL1:
 *   T0SZ = 25 → 39-bit user VA space (512GB)
 *   T1SZ = 25 → 39-bit kernel VA space
 *   This means:
 *     TTBR0 handles VA 0x0000_0000_0000_0000 - 0x0000_007F_FFFF_FFFF
 *     TTBR1 handles VA 0xFFFF_FF80_0000_0000 - 0xFFFF_FFFF_FFFF_FFFF
 *
 * With T0SZ=T1SZ=25, translation starts at L1 (skipping L0).
 * L1: 512 entries × 1GB each = 512GB
 * L2: 512 entries × 2MB each = 1GB
 */
#include "mmu.h"
#include "pmm.h"
#include "uart.h"

#define PT_ENTRIES 512

/* ===== Kernel page tables (TTBR1) ===== */
/* With T1SZ=25, translation starts at L1 */
static uint64_t kern_l1[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kern_l2_mmio[PT_ENTRIES] __attribute__((aligned(4096)));  /* 0-1GB: MMIO */
static uint64_t kern_l2_ram[PT_ENTRIES] __attribute__((aligned(4096)));   /* 1-2GB: RAM+PCI */
static uint64_t kern_l2_ecam[PT_ENTRIES] __attribute__((aligned(4096)));  /* PCI ECAM */

/* ===== Empty user page table (TTBR0) for kernel-only context ===== */
static uint64_t empty_l1[PT_ENTRIES] __attribute__((aligned(4096)));

static void zero_table(uint64_t *table) {
    for (int i = 0; i < PT_ENTRIES; i++)
        table[i] = 0;
}

/*
 * Kernel VA offset: add this to PA to get kernel VA.
 * With T1SZ=25, kernel VA starts at 0xFFFFFF8000000000.
 */
#define KERN_VA_BASE 0xFFFFFF8000000000UL

/* Convert PA to kernel VA */
#define PA_TO_KVA(pa) ((pa) + KERN_VA_BASE)

void mmu_init(void) {
    uart_puts("[MMU] Setting up TTBR0/TTBR1 split page tables...\n");

    zero_table(kern_l1);
    zero_table(kern_l2_mmio);
    zero_table(kern_l2_ram);
    zero_table(kern_l2_ecam);
    zero_table(empty_l1);

    /*
     * Kernel L1 index mapping (each entry = 1GB):
     *   KVA 0xFFFFFF8000000000 → L1[0] → PA 0x00000000 (MMIO)
     *   KVA 0xFFFFFF8040000000 → L1[1] → PA 0x40000000 (RAM)
     *   KVA 0xFFFFFF8080000000 → L1[2] → PA 0x80000000 (PCI MMIO32)
     *   ...
     *
     * But we also need the ECAM at PA 0x4010000000.
     * L1 index for ECAM: (0x4010000000 >> 30) & 0x1FF = 256
     * KVA for ECAM: 0xFFFFFF8000000000 + 0x4000000000 = 0xFFFFFFC000000000
     * L1[256] → ECAM
     */

    /* L2 for MMIO: 0x00000000-0x3FFFFFFF (device memory, 2MB blocks) */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = (uint64_t)i << 21;
        kern_l2_mmio[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }

    /* L2 for RAM region: PA 0x40000000-0x7FFFFFFF */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = 0x40000000UL + ((uint64_t)i << 21);
        if (i < 64) {
            /* RAM: AP=00 (EL1 only) */
            kern_l2_ram[i] = pa | MMU_NORMAL_FLAGS | PTE_BLOCK | PTE_VALID;
        } else {
            kern_l2_ram[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
        }
    }

    /* L2 for PCI ECAM: PA 0x4000000000-0x403FFFFFFF */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = 0x4000000000UL + ((uint64_t)i << 21);
        kern_l2_ecam[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }

    /* Kernel L1 */
    kern_l1[0] = (uint64_t)kern_l2_mmio | PTE_VALID | PTE_TABLE;
    kern_l1[1] = (uint64_t)kern_l2_ram  | PTE_VALID | PTE_TABLE;
    /* L1[2..3]: 1GB device blocks for PCI MMIO32 */
    for (int i = 2; i < 4; i++) {
        uint64_t pa = (uint64_t)i << 30;
        kern_l1[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }
    /* ECAM */
    kern_l1[256] = (uint64_t)kern_l2_ecam | PTE_VALID | PTE_TABLE;

    uart_puts("[MMU] Kernel page tables built (TTBR1)\n");

    /*
     * TCR_EL1 configuration:
     *   T0SZ = 25 → 39-bit TTBR0 VA (512GB user space)
     *   T1SZ = 25 → 39-bit TTBR1 VA (512GB kernel space)
     *   TG0 = 00 (4KB granule for TTBR0)
     *   TG1 = 10 (4KB granule for TTBR1)
     *   IRGN0/ORGN0 = 01/01 (WB/WA)
     *   IRGN1/ORGN1 = 01/01 (WB/WA)
     *   SH0 = 11 (inner shareable)
     *   SH1 = 11 (inner shareable)
     *   IPS = from ID_AA64MMFR0_EL1
     */
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t pa_range = mmfr0 & 0xF;

    uint64_t tcr = (25UL << 0)     /* T0SZ = 25 */
                 | (1UL << 8)      /* IRGN0 = WB/WA */
                 | (1UL << 10)     /* ORGN0 = WB/WA */
                 | (3UL << 12)     /* SH0 = inner shareable */
                 | (0UL << 14)     /* TG0 = 4KB */
                 | (25UL << 16)    /* T1SZ = 25 */
                 | (1UL << 24)     /* IRGN1 = WB/WA */
                 | (1UL << 26)     /* ORGN1 = WB/WA */
                 | (3UL << 28)     /* SH1 = inner shareable */
                 | (2UL << 30)     /* TG1 = 4KB (encoded as 10) */
                 | (pa_range << 32); /* IPS */

    uint64_t mair = (0x00UL << 0) | (0xFFUL << 8);

    uart_puts("[MMU] Enabling MMU (TTBR0=empty, TTBR1=kernel)...\n");

    /*
     * IMPORTANT: We're currently running at PA 0x40000000.
     * We need TTBR0 to also map this PA during the transition,
     * so the instruction after msr sctlr_el1 can be fetched.
     *
     * Strategy: temporarily set TTBR0 = kern_l1 (same as TTBR1)
     * so both low and high VAs resolve. After jumping to high VA,
     * we can set TTBR0 to the empty table.
     */
    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"  /* Temporary: same as kernel */
        "msr ttbr1_el1, %2\n"  /* Kernel page table */
        "isb\n"
        : : "r"(mair), "r"(tcr), "r"((uint64_t)kern_l1)
    );

    /* Enable MMU */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"
        : : "r"(sctlr)
    );

    uart_puts("[MMU] MMU enabled!\n");

    /*
     * Now switch TTBR0 to the empty table.
     * Kernel code continues to work via TTBR1.
     * We're still running at low VA (PA-based) which is fine
     * because our code is linked at 0x40000000 and TTBR0 still
     * maps it. We'll keep running at low VA for now — the kernel
     * doesn't need to be at high VA to benefit from the split.
     * The key benefit is that TTBR0 can be swapped per-process.
     */

    /* Actually, keep TTBR0 = kern_l1 for now since kernel code
     * is linked at 0x40000000 (low VA). We'll swap to user tables
     * only when switching to a user task. */

    uart_puts("[MMU] TTBR0/TTBR1 split active. Kernel via TTBR0+TTBR1.\n");
}

/*
 * Create a per-process user page table (TTBR0).
 * Returns the physical address of the L1 table.
 *
 * Maps:
 *   user_code_pa → VA user_code_pa (identity mapped for simplicity)
 *   user_stack_pa → VA user_stack_pa
 *
 * All with AP=01 (EL0+EL1 RW).
 */
uintptr_t mmu_create_user_pgd(uintptr_t code_pa, uint32_t code_pages,
                                uintptr_t stack_pa, uint32_t stack_pages) {
    /* Allocate L1 table */
    uintptr_t l1_pa = pmm_alloc_page();
    if (!l1_pa) return 0;
    uint64_t *l1 = (uint64_t *)l1_pa;
    zero_table(l1);

    /*
     * Copy kernel L1 entries into user L1 so kernel code remains
     * accessible when TTBR0 is switched. This is necessary because
     * the kernel is linked at low VA (0x40000000) which falls under TTBR0.
     *
     * We copy all kernel L1 entries, then add user-specific L3 mappings
     * on top for the user code/stack pages with AP=01.
     */
    for (int i = 0; i < PT_ENTRIES; i++)
        l1[i] = kern_l1[i];

    /*
     * Now we need to create L3 mappings for user pages with AP=01.
     * User pages are in the 0x40000000-0x48000000 range (L1 index 1).
     * We need our own L2 table for L1[1] so we can add L3 tables
     * without modifying the kernel's L2.
     */
    uintptr_t l2_pa = pmm_alloc_page();
    if (!l2_pa) { pmm_free_page(l1_pa); return 0; }
    uint64_t *l2 = (uint64_t *)l2_pa;

    /* Copy kernel L2 entries for the RAM region */
    for (int i = 0; i < PT_ENTRIES; i++)
        l2[i] = kern_l2_ram[i];

    /* Override L1[1] to point to our copy */
    l1[1] = l2_pa | PTE_VALID | PTE_TABLE;

    /* Now split specific 2MB blocks and set AP=01 on user pages */
    for (int pass = 0; pass < 2; pass++) {
        uintptr_t base = (pass == 0) ? code_pa : stack_pa;
        uint32_t npages = (pass == 0) ? code_pages : stack_pages;

        for (uint32_t p = 0; p < npages; p++) {
            uintptr_t pa = base + p * PAGE_SIZE;
            uint32_t offset = (uint32_t)(pa - 0x40000000UL);
            int l2_idx = (offset >> 21) & 0x1FF;
            int l3_idx = (offset >> 12) & 0x1FF;

            /* If this L2 entry is still a 2MB block, split it into L3 pages */
            if ((l2[l2_idx] & 0x3) != 0x3) {
                /* bits[1:0] != 11 means it's a block (01) or invalid (00) */
                uintptr_t l3_pa = pmm_alloc_page();
                if (!l3_pa) return 0;
                uint64_t *l3 = (uint64_t *)l3_pa;

                /* Fill L3 with kernel-only pages (AP=00) */
                uint64_t block_base = 0x40000000UL + ((uint64_t)l2_idx << 21);
                for (int j = 0; j < PT_ENTRIES; j++) {
                    uint64_t ppa = block_base + ((uint64_t)j << 12);
                    l3[j] = ppa | PTE_VALID | PTE_PAGE | PTE_AF
                           | PTE_ATTR_NORMAL | PTE_SH_INNER | PTE_AP_RW_EL1;
                }
                l2[l2_idx] = l3_pa | PTE_VALID | PTE_TABLE;
            }

            /* Get L3 table and set AP=01 on this page */
            uint64_t *l3 = (uint64_t *)(l2[l2_idx] & ~0xFFFUL);
            l3[l3_idx] = (pa & ~0xFFFUL) | PTE_VALID | PTE_PAGE | PTE_AF
                       | PTE_ATTR_NORMAL | PTE_SH_INNER | PTE_AP_RW_ALL;
        }
    }

    return l1_pa;
}

/*
 * Switch TTBR0 to a user process page table.
 * Pass 0 to switch back to kernel-only (no user mappings).
 */
void mmu_switch_ttbr0(uintptr_t pgd) {
    if (pgd == 0)
        pgd = (uintptr_t)kern_l1;  /* Kernel identity map */

    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        : : "r"(pgd)
    );
}

/* Legacy — keep for compatibility but no longer used */
void mmu_map_user_range(uintptr_t start, uint32_t num_pages) {
    (void)start; (void)num_pages;
}
void mmu_map_user_page(uintptr_t pa) { (void)pa; }
void mmu_map_page(uintptr_t va, uintptr_t pa, uint64_t flags) {
    (void)va; (void)pa; (void)flags;
}
