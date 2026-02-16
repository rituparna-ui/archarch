/* stksmash.bin — stack overflow test (fd-based syscalls) */
.global _start
_start:
    mov     x0, #1; adr x1, msg_start; mov x2, #28; mov x8, #1; svc #0
    mov     x19, #0
    bl      recurse
    mov     x0, #1; adr x1, msg_fail; mov x2, #28; mov x8, #1; svc #0
    mov     x0, #1; mov x8, #5; svc #0
    b       .

recurse:
    sub     sp, sp, #64
    stp     x29, x30, [sp]; mov x29, sp
    add     x19, x19, #1

    mov     x20, #100; udiv x21, x19, x20; msub x22, x21, x20, x19
    cbnz    x22, .Lskip

    mov     x0, #1; adr x1, msg_depth; mov x2, #7; mov x8, #1; svc #0
    mov     x22, x19
    sub     sp, sp, #16; mov x23, sp; add x23, x23, #8; mov x24, x23
.Ld: cbz x22, .Lp
    mov x25, #10; udiv x26, x22, x25; msub x27, x26, x25, x22
    add w27, w27, #'0'; sub x23, x23, #1; strb w27, [x23]; mov x22, x26; b .Ld
.Lp:
    mov     x0, #1; mov x1, x23; sub x2, x24, x23; mov x8, #1; svc #0
    add     sp, sp, #16
    mov     x0, #1; adr x1, msg_nl; mov x2, #1; mov x8, #1; svc #0

.Lskip:
    bl      recurse
    ldp     x29, x30, [sp]; add sp, sp, #64; ret

.balign 4
msg_start: .ascii "[stksmash] Stack overflow test\n"
.balign 4
msg_depth: .ascii "depth: "
.balign 4
msg_nl:    .ascii "\n"
.balign 4
msg_fail:  .ascii "[stksmash] BUG: no fault!\n  "
