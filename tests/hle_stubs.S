# Import stubs: each one loads the address of its own name into r10 and asks
# the runtime to resolve it (syscall 0x4000). Real arguments stay in rdi, rsi, rdx.
.macro IMPORT name
.text
.globl \name
.type \name, @function
\name:
    lea .Lname_\name(%rip), %r10
    mov $0x4000, %eax
    syscall
    ret
.section .rodata
.Lname_\name: .asciz "\name"
.endm

IMPORT cincar_puts
IMPORT cincar_add
IMPORT cincar_missing      # not provided by the runtime: must fail loudly
.section .note.GNU-stack,"",@progbits
