/* badmem.bin — tests memory isolation (fd-based syscalls) */
.global _start
_start:
    mov     x0, #1; adr x1, msg_start; mov x2, #30; mov x8, #1; svc #0
    mov     x0, #1; adr x1, msg_test1; mov x2, #35; mov x8, #1; svc #0

    mov     x0, #0x10000000
    ldr     x1, [x0]           /* SHOULD FAULT */

    mov     x0, #1; adr x1, msg_fail; mov x2, #25; mov x8, #1; svc #0
    mov     x0, #1; mov x8, #5; svc #0
    b       .

.balign 4
msg_start: .ascii "[badmem] Memory isolation test\n"
.balign 4
msg_test1: .ascii "[badmem] Reading unmapped @ 0x10000000.."
.balign 4
msg_fail:  .ascii "[badmem] BUG: no fault!\n  "
