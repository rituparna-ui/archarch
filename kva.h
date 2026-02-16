/*
 * Kernel Virtual Address helpers.
 *
 * With T1SZ=25, TTBR1 covers VA 0xFFFFFF8000000000+.
 * The kernel identity-maps all physical memory at this offset:
 *   KVA = PA + KERN_VA_OFFSET
 *   PA  = KVA - KERN_VA_OFFSET
 *
 * All kernel pointers (after boot) are in the high VA range.
 * DMA buffers must be converted to PA before passing to devices.
 */
#ifndef KVA_H
#define KVA_H

#include "types.h"

/* With T1SZ=25: TTBR1 base = 0xFFFFFF8000000000 */
#define KERN_VA_OFFSET  0xFFFFFF8000000000UL

static inline uintptr_t phys_to_virt(uintptr_t pa) {
    return pa + KERN_VA_OFFSET;
}

static inline uintptr_t virt_to_phys(uintptr_t va) {
    return va - KERN_VA_OFFSET;
}

/* Pointer versions */
static inline void *pa_to_kva(uintptr_t pa) {
    return (void *)(pa + KERN_VA_OFFSET);
}

static inline uintptr_t kva_to_pa(const void *ptr) {
    return (uintptr_t)ptr - KERN_VA_OFFSET;
}

#endif
