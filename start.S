.section .text.boot
.global _start

_start:
    /*
     * Determine current exception level and drop to EL1 if needed.
     * QEMU virt machine may start at EL1, EL2, or EL3 depending on
     * version and firmware configuration.
     */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2

    cmp     x0, #3
    b.eq    .from_el3
    cmp     x0, #2
    b.eq    .from_el2
    b       .at_el1

.from_el3:
    mov     x0, #(1 << 10)     /* SCR_EL3.RW: EL2 is AArch64 */
    msr     scr_el3, x0
    mov     x0, #0x3c9          /* DAIF masked, EL2h */
    msr     spsr_el3, x0
    adr     x0, .from_el2
    msr     elr_el3, x0
    eret

.from_el2:
    mov     x0, #(1 << 31)     /* HCR_EL2.RW: EL1 is AArch64 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5          /* DAIF masked, EL1h */
    msr     spsr_el2, x0
    adr     x0, .at_el1
    msr     elr_el2, x0
    eret

.at_el1:
    /* Set up stack pointer */
    ldr     x0, =__stack_top
    mov     sp, x0

    /*
     * Clear SCTLR_EL1.A (bit 1) — disable alignment fault checking.
     * QEMU >= ~8.0 changed the default to A=1, which faults on the
     * unaligned accesses that GCC may emit without -mstrict-align.
     * We clear it as belt-and-suspenders; the real fix is compiling
     * with -mstrict-align so the compiler never emits them.
     */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 1)
    msr     sctlr_el1, x0
    isb

    /* Zero BSS */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:
    cmp     x0, x1
    b.ge    2f
    str     xzr, [x0], #8
    b       1b
2:
    bl      main

3:
    wfe
    b       3b
