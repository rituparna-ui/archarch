/* Linker script for standalone user programs.
 * All user programs are loaded at VA 0x00400000.
 * Entry point is _start at the beginning of .text. */
ENTRY(_start)
SECTIONS {
    . = 0x00400000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) }
}
