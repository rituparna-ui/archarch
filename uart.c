/*
 * PL011 UART driver for QEMU virt machine.
 * UART0 is at 0x09000000.
 */
#include "uart.h"

#define UART0_BASE  0x09000000UL
#define UART_DR     (UART0_BASE + 0x00)
#define UART_FR     (UART0_BASE + 0x18)
#define UART_FR_TXFF (1 << 5)

void uart_init(void) {
    /* QEMU PL011 works out of the box, no init needed */
}

void uart_putc(char c) {
    while (mmio_read32(UART_FR) & UART_FR_TXFF)
        ;
    mmio_write32(UART_DR, (uint32_t)c);
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_puthex(uint64_t val) {
    const char hex[] = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(hex[(val >> i) & 0xf]);
    }
}

void uart_putdec(uint64_t val) {
    if (val == 0) {
        uart_putc('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (--i >= 0)
        uart_putc(buf[i]);
}
