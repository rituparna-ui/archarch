/*
 * Third user program — prints a character repeatedly with yields.
 * Shows interleaving with other tasks.
 */

.global user_program3_start
.global user_program3_end

user_program3_start:

    /* getpid to determine which character to print */
    mov     x8, #1
    svc     #0
    mov     x19, x0             /* pid */

    /* Print banner */
    adr     x0, msg_start
    mov     x1, #16
    mov     x8, #0
    svc     #0

    /* Print pid */
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

    /* Print our character 8 times, yielding each time */
    /* Use 'A' + (pid % 3) as our character */
    mov     x22, #3
    udiv    x23, x19, x22
    msub    x24, x23, x22, x19  /* pid % 3 */
    add     x24, x24, #'A'      /* character to print */

    mov     x21, #0              /* counter */
tick_loop:
    cmp     x21, #8
    b.ge    tick_done

    /* yield first to show interleaving */
    mov     x8, #3
    svc     #0

    /* Print "[X] tick N\n" */
    sub     sp, sp, #16
    mov     w25, #'['
    strb    w25, [sp, #0]
    strb    w24, [sp, #1]
    mov     w25, #']'
    strb    w25, [sp, #2]
    mov     w25, #' '
    strb    w25, [sp, #3]
    mov     w25, #'t'
    strb    w25, [sp, #4]
    mov     w25, #'i'
    strb    w25, [sp, #5]
    mov     w25, #'c'
    strb    w25, [sp, #6]
    mov     w25, #'k'
    strb    w25, [sp, #7]
    mov     w25, #' '
    strb    w25, [sp, #8]
    add     w25, w21, #'0'
    strb    w25, [sp, #9]
    mov     w25, #'\n'
    strb    w25, [sp, #10]
    mov     x0, sp
    mov     x1, #11
    mov     x8, #0
    svc     #0
    add     sp, sp, #16

    add     x21, x21, #1
    b       tick_loop

tick_done:
    adr     x0, msg_done
    mov     x1, #14
    mov     x8, #0
    svc     #0

    mov     x0, #0
    mov     x8, #2
    svc     #0
    b       .

.balign 4
msg_start:  .ascii "[Tick] Started PID="
.balign 4
msg_done:   .ascii "[Tick] Done!\n  "

user_program3_end:
