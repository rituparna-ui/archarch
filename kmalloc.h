/*
 * Kernel heap allocator.
 *
 * Simple free-list allocator backed by the page frame allocator.
 * Grows the heap by requesting pages from PMM as needed.
 *
 * Not thread-safe (fine for single-core bare metal).
 */
#ifndef KMALLOC_H
#define KMALLOC_H

#include "types.h"

/*
 * Initialize the heap. Must be called after pmm_init().
 */
void kmalloc_init(void);

/*
 * Allocate size bytes. Returns pointer or NULL on failure.
 * Minimum alignment: 16 bytes.
 */
void *kmalloc(size_t size);

/*
 * Allocate size bytes, zeroed. Returns pointer or NULL.
 */
void *kzalloc(size_t size);

/*
 * Free a previously allocated pointer.
 */
void kfree(void *ptr);

/*
 * Print heap stats.
 */
void kmalloc_dump_stats(void);

#endif
