/* Linker script for standalone user programs.
 * Produces a flat binary that can be loaded at any address.
 * Entry point is at offset 0. */
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) }
}
