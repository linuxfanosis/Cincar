.globl _start
.text
_start:
    xor %eax, %eax
    mov (%rax), %eax        # null dereference
