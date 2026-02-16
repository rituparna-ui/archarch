/* spin.bin — busy loop that never yields, proves preemptive scheduling works */
.global _start
_start:
    /* Print banner */
    adr     x0, msg_start
    mov     x1, #22
    mov     x8, #0
    svc     #0

    /* getpid */
    mov     x8, #1
    svc     #0
    mov     x19, x0         /* pid */

    /* Busy-loop counter: print every ~5M iterations */
    mov     x20, #0          /* iteration counter */
    mov     x21, #0          /* print counter */

.Lspin:
    add     x20, x20, #1

    /* Check if we've done ~5M iterations */
    mov     x22, #0x4B40
    movk    x22, #0x004C, lsl #16
    udiv    x23, x20, x22
    cmp     x23, x21
    b.eq    .Lspin           /* not yet */

    mov     x21, x23         /* update print counter */

    /* Print "[spin PID] tick N" */
    adr     x0, msg_tick
    mov     x1, #6
    mov     x8, #0
    svc     #0

    /* Print pid digit */
    add     x0, x19, #'0'
    sub     sp, sp, #16
    strb    w0, [sp]
    mov     w1, #']'
    strb    w1, [sp, #1]
    mov     w1, #' '
    strb    w1, [sp, #2]
    mov     x0, sp
    mov     x1, #3
    mov     x8, #0
    svc     #0

    /* Print iteration count as decimal */
    mov     x22, x21
    add     x23, sp, #12    /* end of buffer area */
    mov     x24, x23

    cbz     x22, .Lzero
.Ldigits:
    cbz     x22, .Lpr
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
.Lpr:
    mov     x0, x23
    sub     x1, x24, x23
    mov     x8, #0
    svc     #0
    add     sp, sp, #16

    /* Newline */
    adr     x0, msg_nl
    mov     x1, #1
    mov     x8, #0
    svc     #0

    /* Exit after 5 prints */
    cmp     x21, #100
    b.lt    .Lspin

    adr     x0, msg_done
    mov     x1, #13
    mov     x8, #0
    svc     #0

    mov     x0, #0
    mov     x8, #2
    svc     #0
    b       .

.balign 4
msg_start: .ascii "[spin] No yields! PID="
.balign 4
msg_tick:  .ascii "[spin "
.balign 4
msg_nl:    .ascii "\n"
.balign 4
msg_done:  .ascii "[spin] done!\n "
