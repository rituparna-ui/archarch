/*
 * Bare metal AArch64 virtio demo on QEMU virt machine.
 *
 * Demonstrates:
 *   - PCI ECAM enumeration and BAR assignment (I/O + 64-bit MMIO)
 *   - Virtio PCI capability parsing
 *   - Full virtio 1.x device initialization (shared helpers)
 *   - Split virtqueue with multi-descriptor chains
 *   - virtio-rng: single-descriptor device-writable buffers
 *   - virtio-blk: 3-descriptor chains (header/data/status), read + write
 */
#include "uart.h"
#include "pci.h"
#include "virtio_rng.h"
#include "virtio_blk.h"

static struct virtio_rng rng_dev;
static struct virtio_blk blk_dev;

static uint8_t rng_buf[64] __attribute__((aligned(64)));
static uint8_t blk_buf[512] __attribute__((aligned(512)));
static uint8_t blk_readback[512] __attribute__((aligned(512)));

static void demo_rng(void) {
    uart_puts("--- virtio-rng demo ---\n");
    if (virtio_rng_init(&rng_dev) < 0) {
        uart_puts("SKIP: virtio-rng not available\n\n");
        return;
    }

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 64; j++) rng_buf[j] = 0;
        int got = virtio_rng_read(&rng_dev, rng_buf, 64);
        if (got < 0) { uart_puts("  read failed\n"); continue; }

        uart_puts("  ");
        uart_putdec((uint64_t)got);
        uart_puts("B: ");
        int show = got < 16 ? got : 16;
        const char hex[] = "0123456789abcdef";
        for (int j = 0; j < show; j++) {
            uart_putc(hex[rng_buf[j] >> 4]);
            uart_putc(hex[rng_buf[j] & 0xf]);
            uart_putc(' ');
        }
        uart_puts("...\n");
    }
    uart_puts("\n");
}

static void demo_blk(void) {
    uart_puts("--- virtio-blk demo ---\n");
    if (virtio_blk_init(&blk_dev) < 0) {
        uart_puts("SKIP: virtio-blk not available\n\n");
        return;
    }

    /* Fill a sector with a recognizable pattern */
    uart_puts("[BLK] Writing pattern to sector 0...\n");
    for (int i = 0; i < 512; i++)
        blk_buf[i] = (uint8_t)(i & 0xFF);

    /* Stamp a signature at the start */
    const char *sig = "VIRTIO-BLK-TEST!";
    for (int i = 0; sig[i]; i++)
        blk_buf[i] = (uint8_t)sig[i];

    if (virtio_blk_write(&blk_dev, 0, 1, blk_buf) < 0) {
        uart_puts("[BLK] Write FAILED\n");
        return;
    }
    uart_puts("[BLK] Write OK\n");

    /* Read it back */
    uart_puts("[BLK] Reading sector 0...\n");
    for (int i = 0; i < 512; i++)
        blk_readback[i] = 0;

    if (virtio_blk_read(&blk_dev, 0, 1, blk_readback) < 0) {
        uart_puts("[BLK] Read FAILED\n");
        return;
    }
    uart_puts("[BLK] Read OK\n");

    /* Verify */
    uart_puts("[BLK] First 32 bytes: ");
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        uart_putc(hex[blk_readback[i] >> 4]);
        uart_putc(hex[blk_readback[i] & 0xf]);
        uart_putc(' ');
    }
    uart_puts("\n");

    /* ASCII view of signature */
    uart_puts("[BLK] Signature: \"");
    for (int i = 0; i < 16; i++)
        uart_putc((char)blk_readback[i]);
    uart_puts("\"\n");

    /* Compare */
    int match = 1;
    for (int i = 0; i < 512; i++) {
        if (blk_buf[i] != blk_readback[i]) {
            match = 0;
            uart_puts("[BLK] MISMATCH at byte ");
            uart_putdec((uint64_t)i);
            uart_puts(": wrote ");
            uart_puthex(blk_buf[i]);
            uart_puts(" read ");
            uart_puthex(blk_readback[i]);
            uart_puts("\n");
            break;
        }
    }
    if (match)
        uart_puts("[BLK] VERIFY OK — write/read roundtrip passed!\n");

    /* Write a second sector and read both back */
    uart_puts("[BLK] Writing sector 1...\n");
    for (int i = 0; i < 512; i++)
        blk_buf[i] = (uint8_t)(0xFF - (i & 0xFF));

    if (virtio_blk_write(&blk_dev, 1, 1, blk_buf) == 0) {
        uart_puts("[BLK] Write sector 1 OK\n");

        if (virtio_blk_read(&blk_dev, 1, 1, blk_readback) == 0) {
            uart_puts("[BLK] Read sector 1 OK, first 16 bytes: ");
            for (int i = 0; i < 16; i++) {
                uart_putc(hex[blk_readback[i] >> 4]);
                uart_putc(hex[blk_readback[i] & 0xf]);
                uart_putc(' ');
            }
            uart_puts("\n");
        }
    }

    uart_puts("\n");
}

void main(void) {
    uart_init();
    uart_puts("\n==========================================\n");
    uart_puts("  AArch64 Bare Metal Virtio Demo\n");
    uart_puts("  PCI ECAM / Virtio 1.x / Split VQ\n");
    uart_puts("  Devices: RNG + Block\n");
    uart_puts("==========================================\n\n");

    pci_enumerate();
    uart_puts("\n");

    demo_rng();
    demo_blk();

    uart_puts("==========================================\n");
    uart_puts("  All demos complete. System halted.\n");
    uart_puts("==========================================\n");

    for (;;)
        __asm__ volatile("wfe");
}
