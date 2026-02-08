/*
 * Bare metal AArch64 virtio-rng demo on QEMU virt machine.
 *
 * Demonstrates:
 *   - PCI ECAM enumeration and BAR assignment
 *   - Virtio PCI capability parsing
 *   - Full virtio 1.x device initialization sequence
 *   - Split virtqueue operation
 *   - Reading entropy from virtio-rng device
 */
#include "uart.h"
#include "pci.h"
#include "virtio_rng.h"

static struct virtio_rng rng_dev;

/* Buffer for random data — must be accessible by device (identity mapped) */
static uint8_t rng_buf[64] __attribute__((aligned(64)));

void main(void) {
    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("  AArch64 Bare Metal Virtio-RNG Demo\n");
    uart_puts("  PCI ECAM / Virtio 1.x / Split VQ\n");
    uart_puts("========================================\n\n");

    /* Enumerate PCI bus to show what's there */
    pci_enumerate();
    uart_puts("\n");

    /* Initialize the virtio-rng device */
    if (virtio_rng_init(&rng_dev) < 0) {
        uart_puts("FATAL: Failed to initialize virtio-rng\n");
        goto halt;
    }

    /* Request random data multiple times to demonstrate the device works */
    for (int round = 0; round < 5; round++) {
        uart_puts("[MAIN] Requesting 64 bytes of entropy (round ");
        uart_putdec((uint64_t)(round + 1));
        uart_puts(")...\n");

        /* Clear buffer */
        for (int i = 0; i < 64; i++)
            rng_buf[i] = 0;

        int got = virtio_rng_read(&rng_dev, rng_buf, 64);
        if (got < 0) {
            uart_puts("[MAIN] ERROR: read failed\n");
            continue;
        }

        uart_puts("[MAIN] Received ");
        uart_putdec((uint64_t)got);
        uart_puts(" bytes: ");

        /* Print first 16 bytes as hex */
        int show = got < 16 ? got : 16;
        for (int i = 0; i < show; i++) {
            const char hex[] = "0123456789abcdef";
            uart_putc(hex[rng_buf[i] >> 4]);
            uart_putc(hex[rng_buf[i] & 0xf]);
            uart_putc(' ');
        }
        uart_puts("...\n");
    }

    uart_puts("\n========================================\n");
    uart_puts("  Demo complete. System halted.\n");
    uart_puts("========================================\n");

halt:
    for (;;)
        __asm__ volatile("wfe");
}
