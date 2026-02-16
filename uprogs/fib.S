/* fib.bin — computes and prints Fibonacci numbers */
.global _start
_start:
    adr     x0, msg_start
    mov     x1, #16
    mov     x8, #0
    svc     #0

    mov     x19, #1         /* a */
    mov     x20, #1         /* b */
    mov     x21, #0         /* counter */

.Lloop:
    cmp     x21, #100
    b.ge    .Ldone

    /* Print "fib(N) = " */
    adr     x0, msg_fib
    mov     x1, #4
    mov     x8, #0
    svc     #0

    /* Print x19 as decimal */
    mov     x22, x19
    sub     sp, sp, #32
    mov     x23, sp
    add     x23, x23, #20

    cbz     x22, .Lzero
.Ldigits:
    cbz     x22, .Lprint
    mov     x25, #10
    udiv    x26, x22, x25
    msub    x27, x26, x25, x22
    add     w27, w27, #'0'
    sub     x23, x23, #1
    strb    w27, [x23]
    mov     x22, x26
    b       .Ldigits

.Lzero:
    mov     w27, #'0'
    sub     x23, x23, #1
    strb    w27, [x23]

.Lprint:
    mov     x0, x23
    mov     x24, sp
    add     x24, x24, #20
    sub     x1, x24, x23
    mov     x8, #0
    svc     #0
    add     sp, sp, #32

    /* Newline */
    adr     x0, msg_nl
    mov     x1, #1
    mov     x8, #0
    svc     #0

    /* Next: a, b = b, a+b */
    add     x22, x19, x20
    mov     x19, x20
    mov     x20, x22
    add     x21, x21, #1

    /* yield */
    mov     x8, #3
    svc     #0

    b       .Lloop

.Ldone:
    adr     x0, msg_done
    mov     x1, #6
    mov     x8, #0
    svc     #0

    mov     x0, #0
    mov     x8, #2
    svc     #0
    b       .

.balign 4
msg_start: .ascii "Fibonacci seq:\n  "
.balign 4
msg_fib:   .ascii "fib="
.balign 4
msg_nl:    .ascii "\n"
.balign 4
msg_done:  .ascii "done!\n"
