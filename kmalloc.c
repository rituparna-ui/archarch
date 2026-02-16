/*
 * Kernel heap allocator — simple first-fit free list.
 *
 * Each allocation has a header storing size and free/used state.
 * Free blocks are coalesced on kfree(). New pages are requested
 * from PMM when the heap runs out.
 */
#include "kmalloc.h"
#include "pmm.h"
#include "uart.h"

#define HEAP_INITIAL_PAGES  16  /* 64KB initial heap */
#define ALIGN16(x) (((x) + 15) & ~15UL)

struct block_header {
    size_t   size;      /* Size of usable area (excludes header) */
    int      free;
    struct block_header *next;
};

#define HEADER_SIZE ALIGN16(sizeof(struct block_header))

static struct block_header *heap_start;
static struct block_header *heap_end;
static size_t total_allocated;
static size_t total_freed;
static size_t heap_size;

static void heap_grow(uint32_t pages) {
    uintptr_t new_pages = pmm_alloc_pages(pages);
    if (!new_pages) {
        uart_puts("[HEAP] ERROR: cannot grow heap\n");
        return;
    }

    size_t grow_size = pages * PAGE_SIZE;

    /* If this is contiguous with existing heap, extend last block */
    if (heap_end && (uintptr_t)heap_end + HEADER_SIZE + heap_end->size == new_pages) {
        if (heap_end->free) {
            heap_end->size += grow_size;
        } else {
            /* Add new free block after the last block */
            struct block_header *new_block = (struct block_header *)new_pages;
            new_block->size = grow_size - HEADER_SIZE;
            new_block->free = 1;
            new_block->next = NULL;
            heap_end->next = new_block;
            heap_end = new_block;
        }
    } else {
        /* Non-contiguous: create a new free block */
        struct block_header *new_block = (struct block_header *)new_pages;
        new_block->size = grow_size - HEADER_SIZE;
        new_block->free = 1;
        new_block->next = NULL;

        if (heap_end)
            heap_end->next = new_block;
        else
            heap_start = new_block;
        heap_end = new_block;
    }

    heap_size += grow_size;
}

void kmalloc_init(void) {
    heap_start = NULL;
    heap_end = NULL;
    total_allocated = 0;
    total_freed = 0;
    heap_size = 0;

    heap_grow(HEAP_INITIAL_PAGES);

    uart_puts("[HEAP] Initialized: ");
    uart_putdec(heap_size / 1024);
    uart_puts(" KB\n");
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN16(size);

    /* First-fit search */
    struct block_header *curr = heap_start;
    while (curr) {
        if (curr->free && curr->size >= size) {
            /* Split if remaining space is large enough */
            if (curr->size >= size + HEADER_SIZE + 16) {
                struct block_header *new_block =
                    (struct block_header *)((uint8_t *)curr + HEADER_SIZE + size);
                new_block->size = curr->size - size - HEADER_SIZE;
                new_block->free = 1;
                new_block->next = curr->next;

                if (curr == heap_end)
                    heap_end = new_block;

                curr->size = size;
                curr->next = new_block;
            }

            curr->free = 0;
            total_allocated += curr->size;
            return (void *)((uint8_t *)curr + HEADER_SIZE);
        }
        curr = curr->next;
    }

    /* No fit found — grow heap */
    uint32_t pages_needed = (uint32_t)((size + HEADER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (pages_needed < 4) pages_needed = 4;  /* Grow at least 16KB */
    heap_grow(pages_needed);

    /* Retry */
    return kmalloc(size);
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < size; i++)
            p[i] = 0;
    }
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;

    struct block_header *block =
        (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);
    block->free = 1;
    total_freed += block->size;

    /* Coalesce with next block if free */
    if (block->next && block->next->free) {
        block->size += HEADER_SIZE + block->next->size;
        if (block->next == heap_end)
            heap_end = block;
        block->next = block->next->next;
    }

    /* Coalesce with previous block if free */
    struct block_header *prev = NULL;
    struct block_header *curr = heap_start;
    while (curr && curr != block) {
        prev = curr;
        curr = curr->next;
    }
    if (prev && prev->free) {
        prev->size += HEADER_SIZE + block->size;
        prev->next = block->next;
        if (block == heap_end)
            heap_end = prev;
    }
}

void kmalloc_dump_stats(void) {
    uint32_t free_blocks = 0, used_blocks = 0;
    size_t free_bytes = 0, used_bytes = 0;

    struct block_header *curr = heap_start;
    while (curr) {
        if (curr->free) {
            free_blocks++;
            free_bytes += curr->size;
        } else {
            used_blocks++;
            used_bytes += curr->size;
        }
        curr = curr->next;
    }

    uart_puts("[HEAP] Stats: ");
    uart_putdec(used_blocks);
    uart_puts(" used (");
    uart_putdec(used_bytes);
    uart_puts(" B), ");
    uart_putdec(free_blocks);
    uart_puts(" free (");
    uart_putdec(free_bytes);
    uart_puts(" B), heap=");
    uart_putdec(heap_size / 1024);
    uart_puts(" KB\n");
}
