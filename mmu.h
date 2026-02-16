/*
 * AArch64 MMU — 4KB granule, 48-bit VA.
 *
 * Translation: L0 → L1 → L2 → L3 → 4KB page
 *
 * We use a simple identity mapping:
 *   VA == PA for all mapped regions.
 *
 * Memory attributes:
 *   - Normal cacheable (write-back) for RAM
 *   - Device-nGnRnE for MMIO (UART, GIC, PCI)
 *
 * MAIR_EL1 index assignments:
 *   0: Device-nGnRnE  (0x00)
 *   1: Normal WB cacheable (0xFF)
 */
#ifndef MMU_H
#define MMU_H

#include "types.h"

/* Page table entry flags */
#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)  /* For L0-L2: points to next-level table */
#define PTE_PAGE        (1UL << 1)  /* For L3: this is a 4KB page */
#define PTE_AF          (1UL << 10) /* Access flag (must be set) */
#define PTE_SH_INNER    (3UL << 8)  /* Inner shareable */
#define PTE_SH_OUTER    (2UL << 8)  /* Outer shareable */

/* AP (Access Permission) bits [7:6] */
#define PTE_AP_RW_EL1   (0UL << 6)  /* EL1 R/W, EL0 none */
#define PTE_AP_RW_ALL   (1UL << 6)  /* EL1 R/W, EL0 R/W */
#define PTE_AP_RO_EL1   (2UL << 6)  /* EL1 RO, EL0 none */
#define PTE_AP_RO_ALL   (3UL << 6)  /* EL1 RO, EL0 RO */

/* PXN/UXN (execute-never) */
#define PTE_PXN         (1UL << 53)
#define PTE_UXN         (1UL << 54)

/* MAIR index in AttrIndx field [4:2] */
#define PTE_ATTR_DEVICE (0UL << 2)  /* MAIR index 0: Device */
#define PTE_ATTR_NORMAL (1UL << 2)  /* MAIR index 1: Normal */

/* Convenience: block/page attribute combos */
#define MMU_DEVICE_FLAGS (PTE_VALID | PTE_AF | PTE_ATTR_DEVICE | PTE_SH_OUTER | PTE_PXN | PTE_UXN)
#define MMU_NORMAL_FLAGS (PTE_VALID | PTE_AF | PTE_ATTR_NORMAL | PTE_SH_INNER | PTE_AP_RW_EL1)

/* Block entry flag (for L1/L2 block mappings) — bit 1 is 0 */
#define PTE_BLOCK       (0UL << 1)

/*
 * Initialize page tables and enable the MMU.
 * Must be called after pmm_init().
 *
 * Creates identity mapping:
 *   - RAM region: normal cacheable
 *   - MMIO regions: device memory
 */
void mmu_init(void);

/*
 * Map a single 4KB page. va and pa must be page-aligned.
 * flags: PTE flags (use MMU_DEVICE_FLAGS or MMU_NORMAL_FLAGS | PTE_PAGE).
 */
void mmu_map_page(uintptr_t va, uintptr_t pa, uint64_t flags);

/*
 * Create a per-process user page table (for TTBR0).
 * Identity-maps user code and stack pages with AP=01 (EL0 accessible).
 * Returns physical address of the L1 table, or 0 on failure.
 */
uintptr_t mmu_create_user_pgd(uintptr_t code_pa, uint32_t code_pages,
                                uintptr_t stack_pa, uint32_t stack_pages);

/*
 * Switch TTBR0 to a user process page table.
 * Pass 0 to switch back to kernel-only context.
 */
void mmu_switch_ttbr0(uintptr_t pgd);

/* Legacy — kept for compatibility */
void mmu_map_user_range(uintptr_t start, uint32_t num_pages);
void mmu_map_user_page(uintptr_t pa);

#endif
