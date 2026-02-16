/* hello.bin — prints a greeting using fd-based write(STDOUT, ...) */
.global _start
_start:
    /* write(1, msg, 28) — fd 1 = stdout */
    mov     x0, #1
    adr     x1, msg
    mov     x2, #28
    mov     x8, #1          /* SYS_WRITE */
    svc     #0

    /* getpid() */
    mov     x8, #4          /* SYS_GETPID */
    svc     #0
    mov     x19, x0

    /* write(1, "PID: ", 5) */
    mov     x0, #1
    adr     x1, msg_pid
    mov     x2, #5
    mov     x8, #1
    svc     #0

    /* print pid digit */
    add     x20, x19, #'0'
    sub     sp, sp, #16
    strb    w20, [sp]
    mov     w21, #'\n'
    strb    w21, [sp, #1]
    mov     x0, #1
    mov     x1, sp
    mov     x2, #2
    mov     x8, #1
    svc     #0
    add     sp, sp, #16

    /* write(1, "Goodbye!\n", 9) */
    mov     x0, #1
    adr     x1, msg_bye
    mov     x2, #9
    mov     x8, #1
    svc     #0

    /* exit(0) */
    mov     x0, #0
    mov     x8, #5          /* SYS_EXIT */
    svc     #0
    b       .

.balign 4
msg:     .ascii "Hello from the filesystem!\n  "
.balign 4
msg_pid: .ascii "PID: "
.balign 4
msg_bye: .ascii "Goodbye!\n"
