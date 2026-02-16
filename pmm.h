/*
 * Physical Memory Manager — bitmap-based page frame allocator.
 *
 * Tracks every 4KB page in the system. Each bit in the bitmap
 * represents one page: 1 = used, 0 = free.
 *
 * At boot, pmm_init() marks everything as free, then reserves
 * the kernel image, stack, and any other known regions.
 */
#ifndef PMM_H
#define PMM_H

#include "types.h"

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12

/* QEMU virt: RAM starts at 0x40000000, we have 128MB */
#define RAM_BASE        0x40000000UL
#define RAM_SIZE        (128UL * 1024 * 1024)
#define RAM_END         (RAM_BASE + RAM_SIZE)

#define TOTAL_PAGES     (RAM_SIZE / PAGE_SIZE)

/* Align address up/down to page boundary */
#define PAGE_ALIGN_UP(a)   (((a) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(a) ((a) & ~(PAGE_SIZE - 1))

/*
 * Initialize the page allocator.
 * kernel_end: first address after all kernel data (BSS, stack, etc.)
 * This marks pages [RAM_BASE, kernel_end) as used, rest as free.
 */
void pmm_init(uintptr_t kernel_end);

/*
 * Allocate a single 4KB page. Returns physical address, or 0 on failure.
 */
uintptr_t pmm_alloc_page(void);

/*
 * Free a previously allocated page.
 */
void pmm_free_page(uintptr_t addr);

/*
 * Allocate n contiguous pages. Returns physical address, or 0 on failure.
 */
uintptr_t pmm_alloc_pages(uint32_t count);

/*
 * Free n contiguous pages starting at addr.
 */
void pmm_free_pages(uintptr_t addr, uint32_t count);

/*
 * Mark a range of physical addresses as used (e.g. for MMIO reservations).
 */
void pmm_mark_used(uintptr_t start, uintptr_t end);

/*
 * Stats.
 */
uint32_t pmm_free_count(void);
uint32_t pmm_used_count(void);

#endif
