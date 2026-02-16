/* spin.bin — busy loop, proves preemptive scheduling (fd-based syscalls) */
.global _start
_start:
    mov     x0, #1; adr x1, msg_start; mov x2, #22; mov x8, #1; svc #0
    mov     x8, #4; svc #0; mov x19, x0    /* getpid */

    mov     x20, #0; mov x21, #0

.Lspin:
    add     x20, x20, #1
    mov     x22, #0x4B40; movk x22, #0x004C, lsl #16
    udiv    x23, x20, x22
    cmp     x23, x21; b.eq .Lspin
    mov     x21, x23

    mov     x0, #1; adr x1, msg_tick; mov x2, #6; mov x8, #1; svc #0

    add     x0, x19, #'0'
    sub     sp, sp, #16
    strb    w0, [sp]; mov w1, #']'; strb w1, [sp, #1]; mov w1, #' '; strb w1, [sp, #2]
    mov     x0, #1; mov x1, sp; mov x2, #3; mov x8, #1; svc #0

    mov     x22, x21
    add     x23, sp, #12; mov x24, x23
    cbz     x22, .Lz
.Ld: cbz x22, .Lp
    mov x25, #10; udiv x26, x22, x25; msub x27, x26, x25, x22
    add w27, w27, #'0'; sub x23, x23, #1; strb w27, [x23]; mov x22, x26; b .Ld
.Lz: mov w27, #'0'; sub x23, x23, #1; strb w27, [x23]
.Lp:
    mov     x0, #1; mov x1, x23; sub x2, x24, x23; mov x8, #1; svc #0
    add     sp, sp, #16

    mov     x0, #1; adr x1, msg_nl; mov x2, #1; mov x8, #1; svc #0

    cmp     x21, #5; b.lt .Lspin

    mov     x0, #1; adr x1, msg_done; mov x2, #13; mov x8, #1; svc #0
    mov     x0, #0; mov x8, #5; svc #0
    b       .

.balign 4
msg_start: .ascii "[spin] No yields! PID="
.balign 4
msg_tick:  .ascii "[spin "
.balign 4
msg_nl:    .ascii "\n"
.balign 4
msg_done:  .ascii "[spin] done!\n "
