ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        *(.text.boot)
        . = ALIGN(2048);
        *(.vectors)
        *(.text*)
    } > RAM

    .rodata : {
        *(.rodata*)
    } > RAM

    .data : {
        *(.data*)
    } > RAM

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(4096);
    __stack_top = . + 0x10000;

    . = ALIGN(4096);
    __virtq_area = .;

    /* After virtq area, reserve some space then mark kernel end */
    . = __virtq_area + 0x100000;  /* 1MB for virtqueue buffers */
    . = ALIGN(4096);
    __kernel_end = .;
}
