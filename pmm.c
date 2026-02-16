/*
 * Physical Memory Manager — bitmap page frame allocator.
 *
 * 128MB RAM / 4KB pages = 32768 pages = 4096 bytes of bitmap.
 */
#include "pmm.h"
#include "uart.h"

/* Bitmap: 1 bit per page, 1 = used, 0 = free */
#define BITMAP_SIZE (TOTAL_PAGES / 8)
static uint8_t bitmap[BITMAP_SIZE];

static uint32_t free_pages;
static uint32_t used_pages;

static inline void bit_set(uint32_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bit_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bit_test(uint32_t page) {
    return (bitmap[page / 8] >> (page % 8)) & 1;
}

static uint32_t addr_to_page(uintptr_t addr) {
    return (uint32_t)((addr - RAM_BASE) >> PAGE_SHIFT);
}

static uintptr_t page_to_addr(uint32_t page) {
    return RAM_BASE + ((uintptr_t)page << PAGE_SHIFT);
}

void pmm_init(uintptr_t kernel_end) {
    /* Start with everything free */
    for (uint32_t i = 0; i < BITMAP_SIZE; i++)
        bitmap[i] = 0;

    free_pages = TOTAL_PAGES;
    used_pages = 0;

    /* Reserve pages from RAM_BASE to kernel_end */
    uintptr_t reserved_end = PAGE_ALIGN_UP(kernel_end);
    uint32_t reserved_count = (uint32_t)((reserved_end - RAM_BASE) >> PAGE_SHIFT);

    for (uint32_t i = 0; i < reserved_count; i++) {
        bit_set(i);
    }
    used_pages += reserved_count;
    free_pages -= reserved_count;

    uart_puts("[PMM] Initialized: ");
    uart_putdec(TOTAL_PAGES);
    uart_puts(" total pages, ");
    uart_putdec(reserved_count);
    uart_puts(" reserved (kernel), ");
    uart_putdec(free_pages);
    uart_puts(" free (");
    uart_putdec((uint64_t)free_pages * PAGE_SIZE / 1024);
    uart_puts(" KB)\n");
}

uintptr_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            free_pages--;
            used_pages++;
            return page_to_addr(i);
        }
    }
    uart_puts("[PMM] ERROR: out of memory!\n");
    return 0;
}

void pmm_free_page(uintptr_t addr) {
    if (addr < RAM_BASE || addr >= RAM_END) return;
    uint32_t page = addr_to_page(addr);
    if (bit_test(page)) {
        bit_clear(page);
        free_pages++;
        used_pages--;
    }
}

uintptr_t pmm_alloc_pages(uint32_t count) {
    if (count == 0) return 0;

    /* Simple first-fit scan for contiguous run */
    uint32_t run = 0;
    uint32_t start = 0;

    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        if (!bit_test(i)) {
            if (run == 0) start = i;
            run++;
            if (run == count) {
                for (uint32_t j = start; j < start + count; j++)
                    bit_set(j);
                free_pages -= count;
                used_pages += count;
                return page_to_addr(start);
            }
        } else {
            run = 0;
        }
    }
    uart_puts("[PMM] ERROR: cannot allocate ");
    uart_putdec(count);
    uart_puts(" contiguous pages\n");
    return 0;
}

void pmm_free_pages(uintptr_t addr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        pmm_free_page(addr + i * PAGE_SIZE);
}

void pmm_mark_used(uintptr_t start, uintptr_t end) {
    if (start < RAM_BASE) start = RAM_BASE;
    if (end > RAM_END) end = RAM_END;

    uint32_t first = addr_to_page(PAGE_ALIGN_DOWN(start));
    uint32_t last = addr_to_page(PAGE_ALIGN_UP(end));

    for (uint32_t i = first; i < last && i < TOTAL_PAGES; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            free_pages--;
            used_pages++;
        }
    }
}

uint32_t pmm_free_count(void) { return free_pages; }
uint32_t pmm_used_count(void) { return used_pages; }
