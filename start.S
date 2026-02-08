/*
 * AArch64 startup code with GICv3 exception vector table.
 *
 * Vector table layout (ARMv8-A):
 *   4 groups × 4 entries × 128 bytes = 2048 bytes
 *   Group 0: Current EL with SP0
 *   Group 1: Current EL with SPx  <-- we use this
 *   Group 2: Lower EL using AArch64
 *   Group 3: Lower EL using AArch32
 *
 * Each entry is 128 bytes (32 instructions max).
 * We only handle IRQ from current EL with SPx.
 */

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
    /* Install exception vector table */
    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    /* Jump to C entry (IRQs still masked — main enables them) */
    bl      main

    /* Halt if main returns */
3:
    wfe
    b       3b

/*
 * Exception vector table — must be 2048-byte aligned.
 * 16 entries × 128 bytes each.
 */
.section .vectors, "ax"
.balign 2048
.global vectors
vectors:

/* ---- Group 0: Current EL with SP0 ---- */
/* Synchronous */
.balign 128
    b       unhandled_exception
/* IRQ */
.balign 128
    b       unhandled_exception
/* FIQ */
.balign 128
    b       unhandled_exception
/* SError */
.balign 128
    b       unhandled_exception

/* ---- Group 1: Current EL with SPx ---- */
/* Synchronous */
.balign 128
    b       unhandled_exception
/* IRQ — this is the one we actually use */
.balign 128
    b       irq_entry
/* FIQ */
.balign 128
    b       unhandled_exception
/* SError */
.balign 128
    b       unhandled_exception

/* ---- Group 2: Lower EL using AArch64 ---- */
/* Synchronous */
.balign 128
    b       unhandled_exception
/* IRQ */
.balign 128
    b       unhandled_exception
/* FIQ */
.balign 128
    b       unhandled_exception
/* SError */
.balign 128
    b       unhandled_exception

/* ---- Group 3: Lower EL using AArch32 ---- */
/* Synchronous */
.balign 128
    b       unhandled_exception
/* IRQ */
.balign 128
    b       unhandled_exception
/* FIQ */
.balign 128
    b       unhandled_exception
/* SError */
.balign 128
    b       unhandled_exception

/*
 * IRQ entry point.
 * Save caller-saved registers + ELR_EL1 + SPSR_EL1,
 * call C handler, restore, eret.
 */
.section .text
irq_entry:
    /* Save x0-x30, ELR, SPSR on stack (34 * 8 = 272 bytes, 16-aligned = 272) */
    sub     sp, sp, #272

    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,       [sp, #240]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #248]

    /* Call C handler */
    bl      irq_handler

    /* Restore ELR, SPSR */
    ldp     x0, x1, [sp, #248]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,       [sp, #240]

    add     sp, sp, #272
    eret

/*
 * Unhandled exception — just spin.
 * In a real system you'd dump registers and halt.
 */
unhandled_exception:
    wfe
    b       unhandled_exception
