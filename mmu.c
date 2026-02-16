/*
 * AArch64 MMU setup — identity-mapped, 4KB granule.
 *
 * Strategy: use 2MB block mappings at L2 level for large regions,
 * which avoids needing thousands of L3 page table entries.
 *
 * L0 table: 1 entry covers 512GB — we only need entry 0
 * L1 table: each entry covers 1GB
 * L2 table: each entry covers 2MB (block mapping)
 *
 * Memory map for QEMU virt:
 *   0x00000000 - 0x3FFFFFFF  (1GB)  — MMIO (UART, GIC, PCI PIO, etc.)
 *   0x40000000 - 0x47FFFFFF  (128MB) — RAM
 *   0x4010000000             — PCI ECAM (needs separate mapping)
 *   0x8000000000             — PCI 64-bit MMIO window
 *
 * For simplicity we map the first 4GB as:
 *   [0x00000000, 0x40000000) → device memory
 *   [0x40000000, 0x48000000) → normal cacheable (RAM)
 *   [0x48000000, 0x100000000) → device memory (PCI MMIO32 etc.)
 *
 * PCI ECAM at 0x40_10000000 needs a separate L1 entry.
 */
#include "mmu.h"
#include "pmm.h"
#include "uart.h"

/* Page table sizes */
#define PT_ENTRIES 512

/* We allocate page tables from a static region to avoid chicken-and-egg
 * with the page allocator (PMM uses RAM that we're about to map). */
static uint64_t l0_table[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l1_table[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_table_low[PT_ENTRIES] __attribute__((aligned(4096)));  /* 0x00000000-0x3FFFFFFF */
static uint64_t l2_table_ram[PT_ENTRIES] __attribute__((aligned(4096)));  /* 0x40000000-0x7FFFFFFF */

/* For PCI ECAM: 0x40_00000000 - 0x40_3FFFFFFF (1GB at L1 index 0 of a second L0 region) */
static uint64_t l1_table_high[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_table_ecam[PT_ENTRIES] __attribute__((aligned(4096)));

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

    /*
     * L2 table for 0x00000000 - 0x3FFFFFFF (first 1GB): device memory
     * Each L2 entry = 2MB block
     */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = (uint64_t)i << 21;  /* i * 2MB */
        l2_table_low[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }

    /*
     * L2 table for 0x40000000 - 0x7FFFFFFF (second 1GB)
     * First 64 entries (128MB) = RAM (normal cacheable)
     * Rest = device memory (PCI MMIO32 window extends here)
     */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = 0x40000000UL + ((uint64_t)i << 21);
        if (i < 64) {
            /* RAM: 0x40000000 - 0x47FFFFFF */
            l2_table_ram[i] = pa | MMU_NORMAL_FLAGS | PTE_BLOCK | PTE_VALID;
        } else {
            /* Beyond RAM: device memory */
            l2_table_ram[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
        }
    }

    /*
     * L1 table: each entry covers 1GB
     * Entry 0: 0x00000000 - 0x3FFFFFFF → L2 table (device)
     * Entry 1: 0x40000000 - 0x7FFFFFFF → L2 table (RAM + device)
     * Entries 2-3: 0x80000000 - 0xFFFFFFFF → 1GB device blocks
     */
    l1_table[0] = (uint64_t)l2_table_low | PTE_VALID | PTE_TABLE;
    l1_table[1] = (uint64_t)l2_table_ram | PTE_VALID | PTE_TABLE;

    /* Map 0x80000000 - 0xFFFFFFFF as device (1GB blocks at L1) */
    for (int i = 2; i < 4; i++) {
        uint64_t pa = (uint64_t)i << 30;
        l1_table[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }

    /*
     * L0 table: each entry covers 512GB
     * Entry 0: 0x0000000000 - 0x7FFFFFFFFF → L1 table
     */
    l0_table[0] = (uint64_t)l1_table | PTE_VALID | PTE_TABLE;

    /*
     * PCI ECAM at 0x40_10000000 falls in L0 entry 1 (0x80_00000000 range?).
     * Actually 0x40_10000000 = 0x00000040_10000000
     * L0 index = (addr >> 39) & 0x1FF = (0x4010000000 >> 39) = 0x80 >> 7 = 1? No.
     * 0x4010000000 >> 39 = 0x4010000000 / 0x8000000000 = ~0.5 → index 0.
     * L1 index = (0x4010000000 >> 30) & 0x1FF = 0x100 >> 0 = 0x100 = 256.
     * Wait: 0x4010000000 >> 30 = 0x100400... let me recalculate.
     * 0x4010000000 = 274,877,906,944
     * >> 39 = 274877906944 / 549755813888 = 0 → L0 index 0 ✓
     * >> 30 = 274877906944 / 1073741824 = 256 → L1 index 256
     *
     * So ECAM is at L0[0] → L1[256]. We need L1 entry 256 to point
     * to an L2 table that maps 0x4010000000 as device memory.
     */
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t pa = 0x4000000000UL + ((uint64_t)i << 21);
        l2_table_ecam[i] = pa | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    }
    l1_table[256] = (uint64_t)l2_table_ecam | PTE_VALID | PTE_TABLE;

    /* Also map a few more 1GB regions for high PCI MMIO if needed */
    /* L0[1] covers 0x80_00000000+ — map as device for 64-bit PCI MMIO */
    l1_table_high[0] = 0x8000000000UL | MMU_DEVICE_FLAGS | PTE_BLOCK | PTE_VALID;
    l0_table[1] = (uint64_t)l1_table_high | PTE_VALID | PTE_TABLE;

    uart_puts("[MMU] Page tables built\n");

    /*
     * Configure and enable MMU.
     *
     * MAIR_EL1:
     *   Attr0 = 0x00 (Device-nGnRnE)
     *   Attr1 = 0xFF (Normal, WB/WA/RA inner+outer)
     *
     * TCR_EL1:
     *   T0SZ = 16 (48-bit VA space)
     *   IRGN0 = 01 (WB/WA inner)
     *   ORGN0 = 01 (WB/WA outer)
     *   SH0 = 11 (inner shareable)
     *   TG0 = 00 (4KB granule)
     *   IPS = autodetect
     */
    uint64_t mair = (0x00UL << 0) | (0xFFUL << 8);

    uint64_t tcr = (16UL << 0)    /* T0SZ = 16 → 48-bit VA */
                 | (1UL << 8)     /* IRGN0 = WB/WA */
                 | (1UL << 10)    /* ORGN0 = WB/WA */
                 | (3UL << 12)    /* SH0 = inner shareable */
                 | (0UL << 14);   /* TG0 = 4KB */

    /* Read ID_AA64MMFR0_EL1 for PARange and set IPS */
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t pa_range = mmfr0 & 0xF;
    tcr |= (pa_range << 32);  /* IPS = PARange */

    uart_puts("[MMU] Enabling MMU...\n");

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        :
        : "r"(mair), "r"(tcr), "r"((uint64_t)l0_table)
    );

    /* Enable MMU + caches in SCTLR_EL1 */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1 << 0);   /* M: MMU enable */
    sctlr |= (1 << 2);   /* C: data cache enable */
    sctlr |= (1 << 12);  /* I: instruction cache enable */
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"
        :
        : "r"(sctlr)
    );

    uart_puts("[MMU] MMU enabled! Identity-mapped, caches on.\n");
}

void mmu_map_page(uintptr_t va, uintptr_t pa, uint64_t flags) {
    /* For future use — individual 4KB page mapping.
     * Would walk L0→L1→L2→L3, allocating tables as needed.
     * Not needed yet since we use 2MB block mappings. */
    (void)va; (void)pa; (void)flags;
}
