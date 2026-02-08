/*
 * Live kernel update implementation.
 *
 * The key trick: we use a staging area in high RAM to hold the new
 * kernel image, then a small position-independent copy loop overwrites
 * the running kernel and jumps to _start. The handoff region at the
 * very top of RAM is never overwritten, so state persists.
 */
#include "liveupdate.h"
#include "uart.h"
#include "irq.h"
#include "timer.h"

/* Linker symbols */
extern uint8_t _start[];

/* Simple memcpy */
static void lu_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}

/* Simple memset */
static void lu_memset(void *dst, uint8_t val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = val;
}

/* Simple additive checksum */
static uint32_t lu_checksum(const void *data, uint32_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size; i++)
        sum += p[i];
    return sum;
}

struct handoff_data *liveupdate_check_handoff(void) {
    struct handoff_data *h = (struct handoff_data *)HANDOFF_ADDR;
    if (h->magic == HANDOFF_MAGIC)
        return h;
    return NULL;
}

void liveupdate_cold_init(void) {
    struct handoff_data *h = (struct handoff_data *)HANDOFF_ADDR;
    lu_memset(h, 0, sizeof(*h));
    h->magic = HANDOFF_MAGIC;
    h->generation = 0;
    h->prev_uptime_ms = 0;
    h->total_boots = 1;
    h->total_ticks = 0;

    const char *msg = "cold boot";
    for (int i = 0; msg[i] && i < 63; i++)
        h->message[i] = msg[i];
}

int liveupdate_check_disk(struct virtio_blk *blk) {
    static uint8_t hdr_buf[512] __attribute__((aligned(512)));

    /* Read sector 4 (update header) */
    if (virtio_blk_read(blk, UPDATE_HEADER_SECTOR, 1, hdr_buf) < 0)
        return 0;

    struct update_header *hdr = (struct update_header *)hdr_buf;
    if (hdr->magic != UPDATE_HEADER_MAGIC)
        return 0;

    /* Valid update header found */
    struct handoff_data *h = (struct handoff_data *)HANDOFF_ADDR;
    if (hdr->generation <= h->generation) {
        /* Already applied this generation — skip */
        return 0;
    }

    return 1;
}

/*
 * The final jump routine. This must be position-independent because
 * it copies over the running kernel (including itself). We put it
 * in a separate section and copy it to high RAM before executing.
 *
 * Written as a C function but called via function pointer from
 * the staging area copy.
 */
typedef void (*jump_fn)(void *dst, const void *src, uint32_t size,
                        void *entry);

/*
 * This is the trampoline that runs from high RAM.
 * It copies the new kernel image to 0x40000000 and jumps to it.
 */
static void __attribute__((noinline, section(".text")))
trampoline_copy_and_jump(void *dst, const void *src, uint32_t size,
                         void *entry)
{
    /* Copy new image over running kernel */
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < size; i++)
        d[i] = s[i];

    /* Data synchronization + instruction cache invalidate */
    __asm__ volatile("dsb sy");
    __asm__ volatile("ic iallu");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    /* Jump to new kernel entry point */
    void (*go)(void) = (void (*)(void))entry;
    go();

    /* Never reached */
    for (;;) __asm__ volatile("wfe");
}

void liveupdate_apply(struct virtio_blk *blk, uint64_t uptime_ms,
                      uint64_t ticks)
{
    static uint8_t hdr_buf[512] __attribute__((aligned(512)));

    uart_puts("[UPDATE] === LIVE UPDATE STARTING ===\n");

    /* Read header again to get size info */
    if (virtio_blk_read(blk, UPDATE_HEADER_SECTOR, 1, hdr_buf) < 0) {
        uart_puts("[UPDATE] Failed to read header\n");
        for (;;) __asm__ volatile("wfe");
    }

    struct update_header *hdr = (struct update_header *)hdr_buf;
    uint32_t img_size = hdr->image_size;
    uint32_t img_sectors = hdr->image_sectors;
    uint32_t new_gen = hdr->generation;
    uint32_t expected_csum = hdr->checksum;

    uart_puts("[UPDATE] Image size: ");
    uart_putdec(img_size);
    uart_puts(" bytes (");
    uart_putdec(img_sectors);
    uart_puts(" sectors)\n");
    uart_puts("[UPDATE] Generation: ");
    uart_putdec(new_gen);
    uart_puts("\n");

    if (img_size > MAX_IMAGE_SIZE) {
        uart_puts("[UPDATE] Image too large!\n");
        for (;;) __asm__ volatile("wfe");
    }

    /* Read new kernel image into staging area */
    uart_puts("[UPDATE] Loading image to staging area...\n");
    uint8_t *staging = (uint8_t *)STAGING_ADDR;

    /* Read in chunks of 1 sector at a time (simple and safe) */
    for (uint32_t s = 0; s < img_sectors; s++) {
        if (virtio_blk_read(blk, UPDATE_IMAGE_SECTOR + s, 1,
                            staging + s * 512) < 0) {
            uart_puts("[UPDATE] Read failed at sector ");
            uart_putdec(s);
            uart_puts("\n");
            for (;;) __asm__ volatile("wfe");
        }
    }

    /* Verify checksum */
    uint32_t actual_csum = lu_checksum(staging, img_size);
    if (actual_csum != expected_csum) {
        uart_puts("[UPDATE] Checksum mismatch! expected=");
        uart_puthex(expected_csum);
        uart_puts(" actual=");
        uart_puthex(actual_csum);
        uart_puts("\n");
        for (;;) __asm__ volatile("wfe");
    }
    uart_puts("[UPDATE] Checksum OK\n");

    /* Save state to handoff region */
    struct handoff_data *h = (struct handoff_data *)HANDOFF_ADDR;
    h->magic = HANDOFF_MAGIC;
    h->generation = new_gen;
    h->prev_uptime_ms = (uint32_t)uptime_ms;
    h->total_boots++;
    h->total_ticks += ticks;

    /* Write a message for the next generation */
    lu_memset(h->message, 0, 64);
    const char *msg = "live update";
    for (int i = 0; msg[i] && i < 63; i++)
        h->message[i] = msg[i];

    uart_puts("[UPDATE] Handoff saved (gen=");
    uart_putdec(new_gen);
    uart_puts(", uptime=");
    uart_putdec(uptime_ms);
    uart_puts("ms, boots=");
    uart_putdec(h->total_boots);
    uart_puts(")\n");

    /* Disable all interrupts */
    irq_disable();
    timer_disable();

    uart_puts("[UPDATE] IRQs disabled. Jumping to new kernel...\n");
    uart_puts("[UPDATE] ========================================\n\n");

    /*
     * Copy the trampoline to a safe location in high RAM
     * (just above the staging area, below handoff).
     * The trampoline is small (~100 bytes), we copy 4KB to be safe.
     */
    uint8_t *trampoline_dst = (uint8_t *)(HANDOFF_ADDR - 4096);
    lu_memcpy(trampoline_dst, (const void *)trampoline_copy_and_jump, 4096);

    /* Flush caches and sync */
    __asm__ volatile("dsb sy");
    __asm__ volatile("ic iallu");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    /* Call the trampoline from its new location */
    jump_fn trampoline = (jump_fn)trampoline_dst;
    trampoline((void *)0x40000000, staging, img_size, (void *)0x40000000);

    /* Never reached */
    for (;;) __asm__ volatile("wfe");
}

int liveupdate_stage(struct virtio_blk *blk, const void *image,
                     uint32_t size, uint32_t generation)
{
    static uint8_t sector_buf[512] __attribute__((aligned(512)));
    uint32_t sectors = (size + 511) / 512;

    uart_puts("[UPDATE] Staging image: ");
    uart_putdec(size);
    uart_puts(" bytes (");
    uart_putdec(sectors);
    uart_puts(" sectors) gen=");
    uart_putdec(generation);
    uart_puts("\n");

    /* Write image sectors */
    const uint8_t *src = (const uint8_t *)image;
    for (uint32_t s = 0; s < sectors; s++) {
        /* Copy to aligned buffer (last sector may be partial) */
        lu_memset(sector_buf, 0, 512);
        uint32_t chunk = size - s * 512;
        if (chunk > 512) chunk = 512;
        lu_memcpy(sector_buf, src + s * 512, chunk);

        if (virtio_blk_write(blk, UPDATE_IMAGE_SECTOR + s, 1,
                             sector_buf) < 0) {
            uart_puts("[UPDATE] Write failed at sector ");
            uart_putdec(s);
            uart_puts("\n");
            return -1;
        }
    }

    /* Compute checksum */
    uint32_t csum = lu_checksum(image, size);

    /* Write header (sector 4) */
    lu_memset(sector_buf, 0, 512);
    struct update_header *hdr = (struct update_header *)sector_buf;
    hdr->magic = UPDATE_HEADER_MAGIC;
    hdr->image_size = size;
    hdr->image_sectors = sectors;
    hdr->generation = generation;
    hdr->checksum = csum;

    if (virtio_blk_write(blk, UPDATE_HEADER_SECTOR, 1, sector_buf) < 0) {
        uart_puts("[UPDATE] Header write failed\n");
        return -1;
    }

    uart_puts("[UPDATE] Image staged on disk (checksum=0x");
    uart_puthex(csum);
    uart_puts(")\n");
    return 0;
}
