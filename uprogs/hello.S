/* hello.bin — prints a greeting and exits */
.global _start
_start:
    adr     x0, msg
    mov     x1, #28
    mov     x8, #0          /* SYS_WRITE */
    svc     #0

    mov     x8, #1          /* SYS_GETPID */
    svc     #0
    mov     x19, x0

    adr     x0, msg_pid
    mov     x1, #5
    mov     x8, #0
    svc     #0

    add     x0, x19, #'0'
    sub     sp, sp, #16
    strb    w0, [sp]
    mov     w1, #'\n'
    strb    w1, [sp, #1]
    mov     x0, sp
    mov     x1, #2
    mov     x8, #0
    svc     #0
    add     sp, sp, #16

    adr     x0, msg_bye
    mov     x1, #9
    mov     x8, #0
    svc     #0

    mov     x0, #0
    mov     x8, #2          /* SYS_EXIT */
    svc     #0
    b       .

.balign 4
msg:     .ascii "Hello from the filesystem!\n  "
.balign 4
msg_pid: .ascii "PID: "
.balign 4
msg_bye: .ascii "Goodbye!\n"
