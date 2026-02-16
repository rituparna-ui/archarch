/*
 * AArch64 MMU setup — identity-mapped, 4KB granule.
 *
 * Strategy:
 *   - 2MB block mappings at L2 for kernel (AP=00, EL1 only)
 *   - 4KB page mappings at L3 for user pages (AP=01, EL0+EL1)
 *
 * When a user task is created, mmu_map_user_page() breaks the
 * relevant 2MB block into 512 individual 4KB pages, then sets
 * AP=01 on just the pages the user needs.
 *
 * Memory map for QEMU virt:
 *   0x00000000 - 0x3FFFFFFF  (1GB)  — MMIO (device memory)
 *   0x40000000 - 0x47FFFFFF  (128MB) — RAM (normal cacheable)
 *   0x4010000000             — PCI ECAM
 */
#include "mmu.h"
#include "pmm.h"
#include "uart.h"

#define PT_ENTRIES 512

/* Static page tables for kernel */
static uint64_t l0_table[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l1_table[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_table_low[PT_ENTRIES] __attribute__((aligned(4096)));
uint64_t l2_table_ram[PT_ENTRIES] __attribute__((aligned(4096)));

/* PCI ECAM */
static uint64_t l1_table_high[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_table_ecam[PT_ENTRIES] __attribute__((aligned(4096)));

/*
 * L3 tables for fine-grained user page mapping.
 * We allocate these dynamically when needed, but keep a cache
 * of pointers so we can find them again.
 * l3_tables[i] corresponds to l2_table_ram[i] — if non-NULL,
 * the L2 entry has been split into an L3 table.
 */
static uint64_t *l3_tables[PT_ENTRIES];

static void zero_table(uint64_t *table) {
    for (int i = 0; i < PT_ENTRIES; i++)
        table[i] = 0;
}

void mmu_init(void) {
    uart_puts("[MMU] Setting up page tables...\n");

    zero_table(l0_table);
    zero_table(l1_table);
    zero_table(l2_table_low);
    zero_table(l2_table_ram);
    zero_table(l1_table_high);
    zero_table(l2_table_ecam);
    for (int i = 0; i < PT_ENTRIES; i++)
        l3_tables[i] = NULL;

    /* L2 for 0x00000000-0x3FFFFFFF: device memory (2MB blocks) */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = (uint64_t)i << 21;
        l2_table_low[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }

    /* L2 for 0x40000000-0x7FFFFFFF: RAM + device */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = 0x40000000UL + ((uint64_t)i << 21);
        if (i < 64) {
            /* RAM: AP=00 (EL1 only) — kernel pages */
            l2_table_ram[i] = pa | MMU_NORMAL_FLAGS | PTE_BLOCK | PTE_VALID;
        } else {
            l2_table_ram[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
        }
    }

    /* L1 table */
    l1_table[0] = (uint64_t)l2_table_low | PTE_VALID | PTE_TABLE;
    l1_table[1] = (uint64_t)l2_table_ram | PTE_VALID | PTE_TABLE;
    for (int i = 2; i < 4; i++) {
        uint64_t pa = (uint64_t)i << 30;
        l1_table[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }

    /* L0 table */
    l0_table[0] = (uint64_t)l1_table | PTE_VALID | PTE_TABLE;

    /* PCI ECAM at L1[256] */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = 0x4000000000UL + ((uint64_t)i << 21);
        l2_table_ecam[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }
    l1_table[256] = (uint64_t)l2_table_ecam | PTE_VALID | PTE_TABLE;

    /* High PCI MMIO */
    l1_table_high[0] = 0x8000000000UL | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    l0_table[1] = (uint64_t)l1_table_high | PTE_VALID | PTE_TABLE;

    uart_puts("[MMU] Page tables built\n");

    /* Configure MAIR, TCR, TTBR0, enable MMU */
    uint64_t mair = (0x00UL << 0) | (0xFFUL << 8);
    uint64_t tcr = (16UL << 0) | (1UL << 8) | (1UL << 10) | (3UL << 12);

    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    tcr |= ((mmfr0 & 0xF) << 32);

    uart_puts("[MMU] Enabling MMU...\n");

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        : : "r"(mair), "r"(tcr), "r"((uint64_t)l0_table)
    );

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1 << 0) | (1 << 2) | (1 << 12);
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"
        : : "r"(sctlr)
    );

    uart_puts("[MMU] MMU enabled! Identity-mapped, caches on.\n");
}

/*
 * Split a 2MB L2 block entry into 512 individual 4KB L3 page entries.
 * The L3 table is allocated from PMM.
 * All 512 pages inherit the same attributes as the original block.
 */
static uint64_t *split_l2_to_l3(int l2_idx) {
    if (l3_tables[l2_idx])
        return l3_tables[l2_idx];

    /* Allocate an L3 table (must be 4KB aligned — one page) */
    uintptr_t l3_page = pmm_alloc_page();
    if (!l3_page) {
        uart_puts("[MMU] ERROR: cannot allocate L3 table\n");
        return NULL;
    }

    uint64_t *l3 = (uint64_t *)l3_page;
    uint64_t block_base = 0x40000000UL + ((uint64_t)l2_idx << 21);

    /* Fill L3 with 512 page entries matching the original block attributes */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = block_base + ((uint64_t)i << 12);
        /* L3 page entry: bit[1]=1 (page), bit[0]=1 (valid) */
        l3[i] = pa | PTE_VALID | PTE_PAGE | PTE_AF | PTE_ATTR_NORMAL
               | PTE_SH_INNER | PTE_AP_RW_EL1;
    }

    /* Replace L2 block entry with table pointer */
    l2_table_ram[l2_idx] = (uint64_t)l3 | PTE_VALID | PTE_TABLE;

    l3_tables[l2_idx] = l3;

    return l3;
}

/*
 * Map a 4KB page as EL0-accessible (AP=01).
 * This splits the containing 2MB block into L3 pages if needed,
 * then sets AP=01 on just the target page.
 */
void mmu_map_user_page(uintptr_t pa) {
    if (pa < 0x40000000UL || pa >= 0x48000000UL) return;

    uint32_t offset = (uint32_t)(pa - 0x40000000UL);
    int l2_idx = offset >> 21;          /* Which 2MB block */
    int l3_idx = (offset >> 12) & 0x1FF; /* Which 4KB page within block */

    uint64_t *l3 = split_l2_to_l3(l2_idx);
    if (!l3) return;

    /* Set AP=01 (EL0+EL1 RW) on this specific page */
    l3[l3_idx] = (pa & ~0xFFFUL) | PTE_VALID | PTE_PAGE | PTE_AF
               | PTE_ATTR_NORMAL | PTE_SH_INNER | PTE_AP_RW_ALL;
}

/*
 * Map a range of pages as EL0-accessible.
 */
void mmu_map_user_range(uintptr_t start, uint32_t num_pages) {
    for (uint32_t i = 0; i < num_pages; i++)
        mmu_map_user_page(start + i * PAGE_SIZE);

    /* TLB invalidate — use raw encoding to avoid assembler issues */
    __asm__ volatile("dsb sy" ::: "memory");
    /* tlbi vmalle1 = sys #0, c8, c7, #0 = 0xd508871f */
    __asm__ volatile(".word 0xd508871f" ::: "memory");
    __asm__ volatile("dsb sy\n isb\n" ::: "memory");
}

void mmu_map_page(uintptr_t va, uintptr_t pa, uint64_t flags) {
    (void)va; (void)pa; (void)flags;
}
