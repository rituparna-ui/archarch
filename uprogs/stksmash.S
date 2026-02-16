/* stksmash.bin — tests stack guard page by overflowing the stack */
.global _start
_start:
    adr     x0, msg_start
    mov     x1, #28
    mov     x8, #0
    svc     #0

    /* Burn through the stack by recursing deeply.
     * User stack is 4 pages = 16KB. Each frame ~32 bytes.
     * ~512 calls should overflow. */
    mov     x19, #0
    bl      recurse

    /* Should not reach here */
    adr     x0, msg_fail
    mov     x1, #28
    mov     x8, #0
    svc     #0
    mov     x0, #1
    mov     x8, #2
    svc     #0
    b       .

recurse:
    /* Push a frame to consume stack */
    sub     sp, sp, #64
    stp     x29, x30, [sp]
    mov     x29, sp

    add     x19, x19, #1

    /* Print progress every 100 calls */
    mov     x20, #100
    udiv    x21, x19, x20
    msub    x22, x21, x20, x19
    cbnz    x22, .Lskip

    adr     x0, msg_depth
    mov     x1, #7
    mov     x8, #0
    svc     #0

    /* Print depth */
    mov     x22, x19
    sub     sp, sp, #16
    mov     x23, sp
    add     x23, x23, #8
    mov     x24, x23
.Ld:
    cbz     x22, .Lp
    mov     x25, #10
    udiv    x26, x22, x25
    msub    x27, x26, x25, x22
    add     w27, w27, #'0'
    sub     x23, x23, #1
    strb    w27, [x23]
    mov     x22, x26
    b       .Ld
.Lp:
    mov     x0, x23
    sub     x1, x24, x23
    mov     x8, #0
    svc     #0
    add     sp, sp, #16

    adr     x0, msg_nl
    mov     x1, #1
    mov     x8, #0
    svc     #0

.Lskip:
    bl      recurse

    ldp     x29, x30, [sp]
    add     sp, sp, #64
    ret

.balign 4
msg_start: .ascii "[stksmash] Stack overflow test\n"
.balign 4
msg_depth: .ascii "depth: "
.balign 4
msg_nl:    .ascii "\n"
.balign 4
msg_fail:  .ascii "[stksmash] BUG: no fault!\n  "
