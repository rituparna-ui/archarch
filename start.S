.section .text.boot
.global _start

_start:
    /* Set up stack pointer */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Zero BSS */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:
    cmp     x0, x1
    b.ge    2f
    str     xzr, [x0], #8
    b       1b
2:
    /* Jump to C entry */
    bl      main

    /* Halt if main returns */
3:
    wfe
    b       3b
