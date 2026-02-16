/*
 * Kernel linker script — high VA kernel.
 *
 * Strategy: use a single PHDR with VMA at high VA and LMA at PA.
 * The boot code (.text.boot) is special — it runs at PA before MMU.
 * We handle this by placing it first and using position-independent
 * code in the boot section.
 *
 * The key insight: all sections after .text.boot have
 *   VMA = LMA + KERN_VA_OFFSET
 * So we set VMA = high VA and use AT() to place LMA at PA.
 */
ENTRY(_start)

_kern_va_offset = 0xFFFFFF8000000000;
_phys_base = 0x40000000;
_virt_base = _phys_base + _kern_va_offset;

SECTIONS
{
    /* Boot code at physical address (runs before MMU) */
    . = _phys_base;
    .text.boot : { *(.text.boot) }

    /* Everything else at high VA, loaded contiguously after boot */
    . = _virt_base + (. - _phys_base);

    .vectors ALIGN(2048) : AT(ADDR(.vectors) - _kern_va_offset) {
        *(.vectors)
    }

    .text : AT(ADDR(.text) - _kern_va_offset) {
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - _kern_va_offset) {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - _kern_va_offset) {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    .bss (NOLOAD) : AT(ADDR(.bss) - _kern_va_offset) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    . = ALIGN(4096);
    __stack_top = . + 0x10000;

    . = ALIGN(4096);
    __virtq_area = .;

    . = __virtq_area + 0x100000;
    . = ALIGN(4096);
    __kernel_end = .;
    /* Boot page tables placed at __kernel_end_phys by start.S (4 x 4KB) */

    /* Physical address symbols for boot code */
    __bss_start_phys = __bss_start - _kern_va_offset;
    __bss_end_phys = __bss_end - _kern_va_offset;
    __stack_top_phys = __stack_top - _kern_va_offset;
    __kernel_end_phys = __kernel_end - _kern_va_offset;
}
