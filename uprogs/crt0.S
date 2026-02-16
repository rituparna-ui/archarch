/* Minimal C runtime — calls main(), then exit(0) */
.global _start
.extern main
_start:
    bl      main
    mov     x0, #0
    mov     x8, #2      /* SYS_EXIT */
    svc     #0
    b       .
