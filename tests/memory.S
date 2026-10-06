.globl _start
.text
_start:
    # brk(0): ask where the heap starts
    mov $12, %eax
    xor %edi, %edi
    syscall
    mov %rax, %r12
    # brk(start + 4096): grow the heap one page; must return the new break
    lea 4096(%r12), %rdi
    mov $12, %eax
    syscall
    lea 4096(%r12), %rcx
    cmp %rcx, %rax
    jne fail
    # the new heap page must be usable
    movb $'O', (%r12)
    movb $'K', 1(%r12)
    movb $10, 2(%r12)
    # mmap(0, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    mov $9, %eax
    xor %edi, %edi
    mov $8192, %esi
    mov $3, %edx
    mov $0x22, %r10d
    mov $-1, %r8
    xor %r9d, %r9d
    syscall
    cmp $-4096, %rax        # values in the top 4096 are errno codes
    ja fail
    mov %rax, %r13
    cmpq $0, (%r13)         # fresh memory must be zero
    jne fail
    movq $0x1234, 4096(%r13)   # second page of the mapping must be writable
    cmpq $0x1234, 4096(%r13)
    jne fail
    # print "OK\n" from the heap
    mov $1, %eax
    mov $1, %edi
    mov %r12, %rsi
    mov $3, %edx
    syscall
    mov $60, %eax
    xor %edi, %edi
    syscall
fail:
    mov $60, %eax
    mov $1, %edi
    syscall
