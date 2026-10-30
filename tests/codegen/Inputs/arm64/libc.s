// Stub libc: write(fd, buf, n) as the Linux arm64 system call.
        .text
        .globl write
write:
        mov     x8, #64
        svc     #0
        ret
