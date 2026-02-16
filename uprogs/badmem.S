/* badmem.bin — tests memory isolation by accessing invalid addresses */
.global _start
_start:
    /* Print banner */
    adr     x0, msg_start
    mov     x1, #30
    mov     x8, #0
    svc     #0

    /* Test 1: Try to read kernel memory at 0x40000000 (should fault) */
    adr     x0, msg_test1
    mov     x1, #35
    mov     x8, #0
    svc     #0

    ldr     x0, =0x40000000    /* kernel .text — should be unmapped in our TTBR0 */
    ldr     x1, [x0]           /* THIS SHOULD FAULT */

    /* If we get here, isolation is broken */
    adr     x0, msg_fail
    mov     x1, #25
    mov     x8, #0
    svc     #0

    mov     x0, #1
    mov     x8, #2
    svc     #0
    b       .

.balign 4
msg_start: .ascii "[badmem] Memory isolation test\n"
.balign 4
msg_test1: .ascii "[badmem] Reading kernel @ 0x40000000..."
.balign 4
msg_fail:  .ascii "[badmem] BUG: no fault!\n  "
