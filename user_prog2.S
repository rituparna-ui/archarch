/*
 * Second user program — computes Fibonacci numbers via syscalls.
 */

.global user_program2_start
.global user_program2_end

user_program2_start:

    /* write banner */
    adr     x0, msg_banner
    mov     x1, #24
    mov     x8, #0
    svc     #0

    /* getpid */
    mov     x8, #1
    svc     #0
    mov     x19, x0             /* pid */

    /* write "PID=" */
    adr     x0, msg_pid
    mov     x1, #4
    mov     x8, #0
    svc     #0

    /* print pid digit */
    add     x20, x19, #'0'
    sub     sp, sp, #16
    strb    w20, [sp]
    mov     w21, #'\n'
    strb    w21, [sp, #1]
    mov     x0, sp
    mov     x1, #2
    mov     x8, #0
    svc     #0
    add     sp, sp, #16

    /* Compute fib(1) through fib(10) */
    mov     x19, #1             /* a = 1 */
    mov     x20, #1             /* b = 1 */
    mov     x21, #0             /* counter */

fib_loop:
    cmp     x21, #10
    b.ge    fib_done

    /* yield() to let other tasks run */
    mov     x8, #3
    svc     #0

    /* Print "Fib: " */
    adr     x0, msg_fib
    mov     x1, #5
    mov     x8, #0
    svc     #0

    /* Print current fib number (x19) as decimal — simple for small numbers */
    mov     x22, x19            /* value to print */
    sub     sp, sp, #32
    mov     x23, sp
    add     x23, x23, #20      /* end of buffer */
    mov     x24, x23            /* save end pointer */

    /* Convert to decimal digits (reverse) */
    cbz     x22, fib_zero
fib_digits:
    cbz     x22, fib_print
    mov     x25, #10
    udiv    x26, x22, x25
    msub    x27, x26, x25, x22 /* remainder */
    add     w27, w27, #'0'
    sub     x23, x23, #1
    strb    w27, [x23]
    mov     x22, x26
    b       fib_digits

fib_zero:
    mov     w27, #'0'
    sub     x23, x23, #1
    strb    w27, [x23]

fib_print:
    /* Write the digits */
    mov     x0, x23
    sub     x1, x24, x23       /* length */
    mov     x8, #0
    svc     #0
    add     sp, sp, #32

    /* Write newline */
    adr     x0, msg_nl
    mov     x1, #1
    mov     x8, #0
    svc     #0

    /* Next fibonacci: a, b = b, a+b */
    add     x22, x19, x20
    mov     x19, x20
    mov     x20, x22
    add     x21, x21, #1
    b       fib_loop

fib_done:
    adr     x0, msg_done
    mov     x1, #20
    mov     x8, #0
    svc     #0

    mov     x0, #0
    mov     x8, #2
    svc     #0
    b       .

.balign 4
msg_banner: .ascii "[Fib] Fibonacci task!\n   "
.balign 4
msg_pid:    .ascii "PID="
.balign 4
msg_fib:    .ascii "Fib: "
.balign 4
msg_nl:     .ascii "\n"
.balign 4
msg_done:   .ascii "[Fib] Task complete!\n"

user_program2_end:
