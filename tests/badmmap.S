.globl _start
.text
_start:
    # mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd=3, 0): file-backed, unsupported
    mov $9, %eax
    xor %edi, %edi
    mov $4096, %esi
    mov $1, %edx
    mov $2, %r10d
    mov $3, %r8d
    xor %r9d, %r9d
    syscall
    mov %eax, %edi
    neg %edi                # EINVAL = 22
    mov $60, %eax
    syscall
