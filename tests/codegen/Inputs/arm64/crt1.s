// Stub C runtime start for the arm64 link test: calls main, then exit(status).
        .text
        .globl _start
_start:
        bl      main
        mov     x8, #94
        svc     #0
