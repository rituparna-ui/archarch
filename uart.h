#ifndef UART_H
#define UART_H

#include "types.h"

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint64_t val);
void uart_putdec(uint64_t val);

#endif
