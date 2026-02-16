/*
 * AArch64 startup code — high VA kernel.
 *
 * Boot sequence:
 *   1. QEMU loads us at PA 0x40000000
 *   2. Drop from EL3/EL2 to EL1 (same as before)
 *   3. At EL1: set up early page tables (identity + high VA)
 *   4. Enable MMU
 *   5. Jump to high VA
 *   6. Zero BSS, set stack (at high VA), install vectors, call kernel_main
 *
 * The boot code in .text.boot runs at physical addresses.
 * After the MMU is enabled and we jump to high VA, all subsequent
 * code runs at 0xFFFFFF80xxxxxxxx.
 */

.section .text.boot
.global _start

/* Kernel VA offset — must match linker script and kva.h */
.equ KERN_VA_OFFSET, 0xFFFFFF8000000000

_start:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2

    cmp     x0, #3
    b.eq    at_el3
    cmp     x0, #2
    b.eq    at_el2
    b       at_el1

at_el3:
    mov     x0, #0x5b1
    msr     scr_el3, x0
    mrs     x0, sctlr_el2
    msr     sctlr_el2, x0
    adr     x0, at_el2
    msr     elr_el3, x0
    mov     x0, #0x3c9
    msr     spsr_el3, x0
    eret

at_el2:
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mrs     x0, S3_4_C12_C9_5
    orr     x0, x0, #(1 << 0)
    orr     x0, x0, #(1 << 3)
    msr     S3_4_C12_C9_5, x0
    isb
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 19)
    msr     sctlr_el1, x0
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, at_el1
    msr     elr_el2, x0
    eret

/*
 * ---- Running at EL1, MMU off, running at PA 0x40xxxxxx ----
 *
 * Set up early page tables:
 *   TTBR0: identity map (PA == VA) — needed to keep running after MMU on
 *   TTBR1: high VA map (VA = PA + KERN_VA_OFFSET)
 *
 * Both use the same L1 table (kern_l1) which maps:
 *   L1[0] → MMIO (0x00000000, device)
 *   L1[1] → RAM  (0x40000000, normal)
 *
 * For TTBR0, L1[0] maps VA 0x00000000 → PA 0x00000000
 * For TTBR1, L1[0] maps VA 0xFFFFFF8000000000 → PA 0x00000000
 * Same table works for both because the L1 index is derived from
 * bits [38:30] of the VA, and with T0SZ=T1SZ=25, the top bits
 * are stripped before indexing.
 */
at_el1:
    /* Temporary stack at PA for boot code */
    ldr     x0, =__stack_top_phys
    mov     sp, x0

    /*
     * Build early page tables.
     * Allocate from a fixed PA region right after the kernel image.
     * Use __kernel_end_phys as the base (4KB aligned).
     */
    ldr     x10, =__kernel_end_phys         /* L1 table */
    add     x11, x10, #4096                 /* L2 MMIO */
    add     x12, x11, #4096                 /* L2 RAM */
    add     x13, x12, #4096                 /* L2 ECAM */

    /* Zero all tables (512 entries × 8 bytes = 4096 bytes each) */
    mov     x0, x10
    bl      zero_page
    mov     x0, x11
    bl      zero_page
    mov     x0, x12
    bl      zero_page
    mov     x0, x13
    bl      zero_page

    /* Fill L2 MMIO: 512 × 2MB device blocks covering 0x00000000-0x3FFFFFFF */
    mov     x0, x11             /* L2 table base */
    mov     x1, #0              /* starting PA */
    mov     x2, #512            /* entries */
    mov     x3, #0x00000000000000E1  /* device flags: valid|AF|SH_outer|PXN|UXN */
    movk    x3, #0x0060, lsl #48    /* PXN|UXN in upper bits */
    /* Actually, let's compute the flags properly */
    /* Device-nGnRnE: AttrIndx=0, SH=outer(10), AF, PXN, UXN, valid, block */
    /* = 0x60000000000000E1 — but we need to be careful with encoding */
    /* Simpler: just use the numeric value */
    /* PTE_VALID=1, PTE_AF=0x400, PTE_SH_OUTER=0x200, PTE_PXN=1<<53, PTE_UXN=1<<54 */
    /* = 0x0060000000000601 — wait, AttrIndx=0 means bits[4:2]=000 */
    /* Let me just hardcode: valid(1) | block(0) | AttrIndx=0(0) | SH=outer(0x200) | AF(0x400) | PXN(1<<53) | UXN(1<<54) */
    mov     x3, #0x0601         /* valid | SH_outer | AF */
    movk    x3, #0x0060, lsl #48  /* PXN | UXN */
    mov     x4, #0              /* PA counter */
    mov     x5, #0              /* index */
fill_l2_mmio:
    cmp     x5, #512
    b.ge    fill_l2_mmio_done
    orr     x6, x4, x3         /* PA | flags */
    str     x6, [x11, x5, lsl #3]
    add     x4, x4, #(1 << 21) /* next 2MB */
    add     x5, x5, #1
    b       fill_l2_mmio
fill_l2_mmio_done:

    /* Fill L2 RAM: first 64 entries = normal cacheable, rest = device */
    /* Normal: valid(1) | AttrIndx=1(0x4) | SH=inner(0x300) | AF(0x400) | AP=00 */
    /* = 0x705 */
    mov     x3, #0x0705         /* normal flags */
    mov     x4, #0x40000000     /* starting PA */
    mov     x5, #0
fill_l2_ram:
    cmp     x5, #512
    b.ge    fill_l2_ram_done
    cmp     x5, #64
    b.ge    fill_l2_ram_dev
    /* Normal RAM */
    orr     x6, x4, x3
    str     x6, [x12, x5, lsl #3]
    b       fill_l2_ram_next
fill_l2_ram_dev:
    /* Device memory beyond RAM */
    mov     x7, #0x0601
    movk    x7, #0x0060, lsl #48
    orr     x6, x4, x7
    str     x6, [x12, x5, lsl #3]
fill_l2_ram_next:
    add     x4, x4, #(1 << 21)
    add     x5, x5, #1
    b       fill_l2_ram
fill_l2_ram_done:

    /* Fill L2 ECAM: device memory at PA 0x4000000000+ */
    mov     x3, #0x0601
    movk    x3, #0x0060, lsl #48
    mov     x4, #0x0000000000
    movk    x4, #0x0040, lsl #32   /* 0x4000000000 */
    mov     x5, #0
fill_l2_ecam:
    cmp     x5, #512
    b.ge    fill_l2_ecam_done
    orr     x6, x4, x3
    str     x6, [x13, x5, lsl #3]
    add     x4, x4, #(1 << 21)
    add     x5, x5, #1
    b       fill_l2_ecam
fill_l2_ecam_done:

    /* Build L1 table */
    /* L1[0] → L2 MMIO (table descriptor: PA | 0x3) */
    orr     x0, x11, #0x3
    str     x0, [x10, #0]          /* L1[0] */

    /* L1[1] → L2 RAM */
    orr     x0, x12, #0x3
    str     x0, [x10, #8]          /* L1[1] */

    /* L1[2..3] → 1GB device blocks */
    mov     x0, #0x80000000
    mov     x3, #0x0601
    movk    x3, #0x0060, lsl #48
    orr     x0, x0, x3
    str     x0, [x10, #16]         /* L1[2] = PA 0x80000000 */
    mov     x0, #0xC0000000
    orr     x0, x0, x3
    str     x0, [x10, #24]         /* L1[3] = PA 0xC0000000 */

    /* L1[256] → L2 ECAM */
    orr     x0, x13, #0x3
    str     x0, [x10, #(256*8)]

    /*
     * Configure MMU registers.
     * TTBR0 = TTBR1 = boot_l1 (same table for both)
     * This works because L1 indices are the same for both VA ranges.
     */
    /* MAIR: Attr0=0x00 (device), Attr1=0xFF (normal WB) */
    mov     x0, #0xFF00
    msr     mair_el1, x0

    /* TCR: T0SZ=25, T1SZ=25, 4KB granule, inner shareable */
    mov     x0, #25                 /* T0SZ */
    orr     x0, x0, #(1 << 8)      /* IRGN0 */
    orr     x0, x0, #(1 << 10)     /* ORGN0 */
    orr     x0, x0, #(3 << 12)     /* SH0 */
    mov     x1, #(25 << 16)        /* T1SZ */
    orr     x0, x0, x1
    orr     x0, x0, #(1 << 24)     /* IRGN1 */
    orr     x0, x0, #(1 << 26)     /* ORGN1 */
    mov     x1, #(3 << 28)         /* SH1 */
    orr     x0, x0, x1
    mov     x1, #(2 << 30)         /* TG1 = 4KB */
    orr     x0, x0, x1
    /* IPS from ID_AA64MMFR0_EL1 */
    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #0xF
    lsl     x1, x1, #32
    orr     x0, x0, x1
    msr     tcr_el1, x0

    /* Set TTBR0 and TTBR1 */
    msr     ttbr0_el1, x10
    msr     ttbr1_el1, x10
    isb

    /* Ensure all page table writes are visible before enabling MMU */
    dsb     sy
    isb

    /* Enable MMU + caches */
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 0)      /* M */
    orr     x0, x0, #(1 << 2)      /* C */
    orr     x0, x0, #(1 << 12)     /* I */
    msr     sctlr_el1, x0
    isb

    /*
     * MMU is on! We're still running at PA (via TTBR0 identity map).
     * Now jump to the high VA version of the kernel.
     */
    ldr     x0, =kernel_high_entry
    br      x0

/*
 * Zero a 4KB page at address x0.
 */
zero_page:
    mov     x1, #512
1:  str     xzr, [x0], #8
    subs    x1, x1, #1
    b.ne    1b
    ret

/*
 * ---- High VA kernel entry ----
 * From here on, everything runs at 0xFFFFFF80xxxxxxxx.
 */
.section .text
.global kernel_high_entry
kernel_high_entry:
    /* Set stack to high VA */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Zero BSS (using high VA addresses) */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:

    /* Install exception vector table (high VA) */
    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    /* Jump to C kernel_main (high VA) */
    bl      kernel_main

    /* Halt */
3:  wfe
    b       3b

/*
 * Exception vector table — must be 2048-byte aligned.
 */
.section .vectors, "ax"
.balign 2048
.global vectors
vectors:

/* Group 0: Current EL with SP0 */
.balign 128; b unhandled_exception
.balign 128; b unhandled_exception
.balign 128; b unhandled_exception
.balign 128; b unhandled_exception

/* Group 1: Current EL with SPx */
.balign 128; b unhandled_exception
.balign 128; b irq_entry
.balign 128; b unhandled_exception
.balign 128; b unhandled_exception

/* Group 2: Lower EL using AArch64 */
.balign 128; b sync_el0_entry
.balign 128; b irq_entry
.balign 128; b unhandled_exception
.balign 128; b unhandled_exception

/* Group 3: Lower EL using AArch32 */
.balign 128; b unhandled_exception
.balign 128; b unhandled_exception
.balign 128; b unhandled_exception
.balign 128; b unhandled_exception

/*
 * IRQ entry — saves all regs + SP_EL0 + ELR + SPSR
 */
.section .text
irq_entry:
    sub     sp, sp, #288
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
    mrs     x0, sp_el0
    str     x0, [sp, #248]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #256]

    bl      irq_handler

    ldp     x0, x1, [sp, #256]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ldr     x0, [sp, #248]
    msr     sp_el0, x0
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
    add     sp, sp, #288
    eret

unhandled_exception:
    wfe
    b       unhandled_exception
