.globl _start
.text
_start:
    mov $60, %eax
    mov $7, %edi
    syscall
