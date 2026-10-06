.globl _start
.text
_start:
    mov $1, %eax            # syscall: write
    mov $1, %edi            # fd 1 (stdout)
    lea msg(%rip), %rsi
    mov $14, %edx           # length
    syscall
    mov $60, %eax           # syscall: exit
    xor %edi, %edi          # status 0
    syscall
.section .rodata
msg: .ascii "hello, loader\n"
