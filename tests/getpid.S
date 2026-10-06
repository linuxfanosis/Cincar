.globl _start
.text
_start:
    mov $39, %eax           # getpid: not implemented by our runtime
    syscall
    mov %eax, %edi
    neg %edi                # exit code = -rax (38 if ENOSYS)
    mov $60, %eax
    syscall
