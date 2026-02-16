/* fib.bin — Fibonacci with fd-based syscalls */
.global _start
_start:
    mov     x0, #1; adr x1, msg_start; mov x2, #16; mov x8, #1; svc #0

    mov     x8, #4; svc #0     /* getpid */
    mov     x19, x0

    mov     x0, #1; adr x1, msg_pid; mov x2, #4; mov x8, #1; svc #0

    add     x20, x19, #'0'
    sub     sp, sp, #16
    strb    w20, [sp]; mov w21, #'\n'; strb w21, [sp, #1]
    mov     x0, #1; mov x1, sp; mov x2, #2; mov x8, #1; svc #0
    add     sp, sp, #16

    mov     x19, #1; mov x20, #1; mov x21, #0

.Lloop:
    cmp     x21, #12; b.ge .Ldone

    mov     x0, #1; adr x1, msg_fib; mov x2, #4; mov x8, #1; svc #0

    /* Print x19 as decimal */
    mov     x22, x19
    sub     sp, sp, #32
    mov     x23, sp; add x23, x23, #20; mov x24, x23
    cbz     x22, .Lzero
.Ld: cbz x22, .Lp
    mov x25, #10; udiv x26, x22, x25; msub x27, x26, x25, x22
    add w27, w27, #'0'; sub x23, x23, #1; strb w27, [x23]; mov x22, x26; b .Ld
.Lzero: mov w27, #'0'; sub x23, x23, #1; strb w27, [x23]
.Lp:
    mov     x0, #1; mov x1, x23; sub x2, x24, x23; mov x8, #1; svc #0
    add     sp, sp, #32

    mov     x0, #1; adr x1, msg_nl; mov x2, #1; mov x8, #1; svc #0

    add     x22, x19, x20; mov x19, x20; mov x20, x22
    add     x21, x21, #1
    mov     x8, #6; svc #0     /* yield */
    b       .Lloop

.Ldone:
    mov     x0, #1; adr x1, msg_done; mov x2, #6; mov x8, #1; svc #0
    mov     x0, #0; mov x8, #5; svc #0
    b       .

.balign 4
msg_start: .ascii "Fibonacci seq:\n  "
.balign 4
msg_pid:   .ascii "PID="
.balign 4
msg_fib:   .ascii "fib="
.balign 4
msg_nl:    .ascii "\n"
.balign 4
msg_done:  .ascii "done!\n"
