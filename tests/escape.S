.globl _start
.text
_start:
    mov $1, %eax
    mov $5, %edi            # fd 5: not allowed
    lea msg(%rip), %rsi
    mov $2, %edx
    syscall
    mov %eax, %edi
    neg %edi                # EBADF = 9
    mov $60, %eax
    syscall
.section .rodata
msg: .ascii "x\n"
