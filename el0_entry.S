/*
 * EL0 support assembly routines.
 *
 * drop_to_el0(entry, user_sp):
 *   Sets up SPSR_EL1 for EL0t, sets ELR_EL1 to entry point,
 *   sets SP_EL0 to user stack, then erets into user mode.
 *
 * sync_el0_entry:
 *   Synchronous exception handler for exceptions from EL0 (lower EL, AArch64).
 *   Saves all registers, reads ESR_EL1 to determine if it's an SVC,
 *   calls syscall_handler, restores registers, erets back to EL0.
 */

.global drop_to_el0
.type drop_to_el0, %function

/*
 * void drop_to_el0(uintptr_t entry, uintptr_t user_sp)
 *   x0 = entry point address
 *   x1 = user stack pointer
 */
drop_to_el0:
    /* Set the user stack pointer (SP_EL0) */
    msr     sp_el0, x1

    /* Set return address to user entry point */
    msr     elr_el1, x0

    /*
     * SPSR_EL1: return to EL0t (EL0 with SP_EL0)
     *   M[4:0] = 0b00000 = EL0t
     *   DAIF = 0 (all interrupts unmasked in user mode)
     */
    mov     x2, #0x0
    msr     spsr_el1, x2

    /* Clear all general-purpose registers for clean user entry */
    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    mov     x6, #0
    mov     x7, #0
    mov     x8, #0
    mov     x9, #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0
    mov     x17, #0
    mov     x18, #0
    mov     x19, #0
    mov     x20, #0
    mov     x21, #0
    mov     x22, #0
    mov     x23, #0
    mov     x24, #0
    mov     x25, #0
    mov     x26, #0
    mov     x27, #0
    mov     x28, #0
    mov     x29, #0
    mov     x30, #0

    eret


/*
 * Synchronous exception entry from EL0 (lower EL, AArch64).
 *
 * Save full register state, call C handler, restore, eret.
 * The frame layout matches what syscall_handler expects:
 *   [sp, #0]   = x0  (also syscall return value)
 *   [sp, #8]   = x1
 *   ...
 *   [sp, #240] = x30
 *   [sp, #248] = SP_EL0
 *   [sp, #256] = ELR_EL1
 *   [sp, #264] = SPSR_EL1
 */
.global sync_el0_entry
.type sync_el0_entry, %function
sync_el0_entry:
    /* We're on the kernel stack (SP_EL1) now */
    sub     sp, sp, #272

    /* Save x0-x30 */
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

    /* Save SP_EL0, ELR_EL1, SPSR_EL1 */
    mrs     x0, sp_el0
    mrs     x1, elr_el1
    mrs     x2, spsr_el1
    str     x0, [sp, #248]
    stp     x1, x2, [sp, #256]

    /* Check ESR_EL1 to verify this is an SVC */
    mrs     x0, esr_el1
    lsr     x1, x0, #26        /* EC field [31:26] */
    cmp     x1, #0x15           /* EC=0x15 = SVC from AArch64 */
    b.ne    sync_el0_not_svc

    /* Call syscall_handler(regs) — x0 = pointer to saved frame */
    mov     x0, sp
    bl      syscall_handler

    /* Restore SP_EL0, ELR_EL1, SPSR_EL1 */
    ldr     x0, [sp, #248]
    ldp     x1, x2, [sp, #256]
    msr     sp_el0, x0
    msr     elr_el1, x1
    msr     spsr_el1, x2

    /* Restore x0-x30 (x0 may have been modified by syscall return) */
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

sync_el0_not_svc:
    /* Not an SVC — fault handler will kill the task via sched_exit.
     * sched_exit does context_switch and never returns here. */
    bl      unhandled_sync_el0
    /* If we somehow get here, just spin */
    b       .
