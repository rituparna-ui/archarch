/*
 * Tiny user-mode program.
 *
 * This runs at EL0. It can only interact with the kernel via SVC.
 * Position-independent — will be copied to allocated pages.
 *
 * Syscall convention:
 *   x8 = syscall number
 *   x0-x5 = arguments
 *   svc #0
 *   x0 = return value
 *
 * Syscalls:
 *   0 = write(buf, len)
 *   1 = getpid()
 *   2 = exit(code)
 */

.global user_program_start
.global user_program_end

user_program_start:

    /* --- write("Hello from EL0 user space!\n") --- */
    adr     x0, msg_hello
    mov     x1, #27             /* length of string */
    mov     x8, #0              /* SYS_WRITE */
    svc     #0

    /* --- getpid() --- */
    mov     x8, #1              /* SYS_GETPID */
    svc     #0
    mov     x19, x0             /* save pid in callee-saved reg */

    /* --- write("My PID is: ") --- */
    adr     x0, msg_pid
    mov     x1, #11
    mov     x8, #0
    svc     #0

    /* --- print pid digit (simple: assume single digit 0-9) --- */
    add     x20, x19, #'0'     /* convert to ASCII */
    sub     sp, sp, #16
    strb    w20, [sp]
    mov     x0, sp
    mov     x1, #1
    mov     x8, #0              /* SYS_WRITE */
    svc     #0
    add     sp, sp, #16

    /* --- write newline --- */
    adr     x0, msg_nl
    mov     x1, #1
    mov     x8, #0
    svc     #0

    /* --- write("Counting: ") --- */
    adr     x0, msg_count
    mov     x1, #10
    mov     x8, #0
    svc     #0

    /* --- count from 1 to 5, printing each --- */
    mov     x19, #1
count_loop:
    cmp     x19, #6
    b.ge    count_done

    /* Convert digit to ASCII and print */
    add     x20, x19, #'0'
    sub     sp, sp, #16
    strb    w20, [sp]
    mov     w21, #' '
    strb    w21, [sp, #1]
    mov     x0, sp
    mov     x1, #2
    mov     x8, #0
    svc     #0
    add     sp, sp, #16

    add     x19, x19, #1
    b       count_loop

count_done:
    /* --- write newline --- */
    adr     x0, msg_nl
    mov     x1, #1
    mov     x8, #0
    svc     #0

    /* --- write("Goodbye from user space!\n") --- */
    adr     x0, msg_bye
    mov     x1, #25
    mov     x8, #0
    svc     #0

    /* --- exit(0) --- */
    mov     x0, #0
    mov     x8, #2              /* SYS_EXIT */
    svc     #0

    /* Should never reach here */
    b       .

/* String data — embedded in the code section for position independence */
.balign 4
msg_hello:  .ascii "Hello from EL0 user space!\n"
.balign 4
msg_pid:    .ascii "My PID is: "
.balign 4
msg_nl:     .ascii "\n"
.balign 4
msg_count:  .ascii "Counting: "
.balign 4
msg_bye:    .ascii "Goodbye from user space!\n\000"

user_program_end:
