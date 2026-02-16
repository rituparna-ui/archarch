/*
 * AArch64 startup code.
 *
 * Boot sequence:
 *   1. Detect current EL (could be EL3, EL2, or EL1)
 *   2. If EL3: configure SCR_EL3, drop to EL2
 *   3. If EL2: configure HCR_EL2, CNTHCTL_EL2, ICC_SRE_EL2, drop to EL1
 *   4. At EL1: zero BSS, set stack, install vectors, call main()
 *
 * This ensures the physical timer, GICv3, and all EL1 features
 * work correctly regardless of which EL QEMU boots us at.
 */

.section .text.boot
.global _start

_start:
    /* Check current exception level */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2         /* x0 = current EL (0-3) */

    cmp     x0, #3
    b.eq    at_el3
    cmp     x0, #2
    b.eq    at_el2
    b       at_el1              /* Already at EL1 */

/*
 * ---- EL3 -> EL2 transition ----
 * Configure Secure Configuration Register to allow NS EL2/EL1.
 */
at_el3:
    /* SCR_EL3: NS=1, HCE=1, SMD=1, RW=1 (AArch64 at EL2) */
    mov     x0, #0x5b1
    msr     scr_el3, x0

    /* SCTLR_EL2: safe reset value */
    mrs     x0, sctlr_el2
    msr     sctlr_el2, x0

    /* Set return address and SPSR for EL2h */
    adr     x0, at_el2
    msr     elr_el3, x0
    mov     x0, #0x3c9         /* SPSR: EL2h, DAIF masked */
    msr     spsr_el3, x0
    eret

/*
 * ---- EL2 -> EL1 transition ----
 * Configure:
 *   - HCR_EL2: RW=1 (AArch64 at EL1), no VM/trapping
 *   - CNTHCTL_EL2: enable physical timer/counter access from EL1
 *   - ICC_SRE_EL2: enable GICv3 system register access from EL1
 *   - VBAR_EL2: point to a safe vector (spin)
 */
at_el2:
    /* HCR_EL2: RW=1 (bit 31) — EL1 is AArch64. No other traps. */
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    /*
     * CNTHCTL_EL2: EL1PCTEN=1 (bit 0), EL1PCEN=1 (bit 1)
     * Allow EL1/EL0 access to physical counter and physical timer.
     */
    mov     x0, #3
    msr     cnthctl_el2, x0

    /* CNTVOFF_EL2 = 0: no virtual offset */
    msr     cntvoff_el2, xzr

    /* GICv3: enable system register interface at EL2 so EL1 can use it */
    mrs     x0, S3_4_C12_C9_5  /* ICC_SRE_EL2 */
    orr     x0, x0, #(1 << 0)  /* SRE = 1 */
    orr     x0, x0, #(1 << 3)  /* Enable = 1 (allow EL1 access) */
    msr     S3_4_C12_C9_5, x0
    isb

    /* SCTLR_EL1: RES1 bits, caches/MMU off */
    mrs     x0, sctlr_el1
    msr     sctlr_el1, x0

    /* SPSR_EL2: return to EL1h with DAIF masked */
    mov     x0, #0x3c5         /* EL1h = 0b00101, DAIF = 0b1111 << 6 */
    msr     spsr_el2, x0

    /* ELR_EL2: return address = at_el1 */
    adr     x0, at_el1
    msr     elr_el2, x0
    eret

/*
 * ---- Running at EL1 ----
 * Standard init: stack, BSS, vectors, call main.
 */
at_el1:
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
    bl      kernel_main

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
 * Save all registers + ELR_EL1 + SPSR_EL1,
 * call C handler, restore, eret.
 */
.section .text
irq_entry:
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

    bl      irq_handler

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

unhandled_exception:
    wfe
    b       unhandled_exception
