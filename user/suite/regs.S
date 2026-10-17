/*
 * Own entry point (no crt0): check the ABI v0 entry state directly. Exit 0
 * if every general-purpose register is zero, RFLAGS is 0x2, RSP is
 * 0x7FFFFFFFEFF0 with 16 zero bytes above it and DS/ES/FS/GS are null; else
 * the number of the first failed check.
 */
#include <vos/syscall.h>

.section .text
.global _start
_start:
    pushfq
    or %rbx, %rax
    or %rcx, %rax
    or %rdx, %rax
    or %rsi, %rax
    or %rdi, %rax
    or %rbp, %rax
    or %r8, %rax
    or %r9, %rax
    or %r10, %rax
    or %r11, %rax
    or %r12, %rax
    or %r13, %rax
    or %r14, %rax
    or %r15, %rax
    mov $1, %edi
    test %rax, %rax
    jnz 9f
    popq %rcx
    mov $2, %edi
    cmp $2, %rcx
    jne 9f
    mov $3, %edi
    movabs $0x7FFFFFFFEFF0, %rdx
    cmp %rdx, %rsp
    jne 9f
    mov $4, %edi
    cmpq $0, (%rsp)
    jne 9f
    cmpq $0, 8(%rsp)
    jne 9f
    mov $5, %edi
    mov %ds, %ax
    mov %es, %cx
    or %cx, %ax
    mov %fs, %cx
    or %cx, %ax
    mov %gs, %cx
    or %cx, %ax
    test %ax, %ax
    jnz 9f
    xor %edi, %edi
9:  mov $VOS_SYS_EXIT, %eax
    int $0x80
    ud2

/* The RW segment must not be empty (see user.ld). */
.section .data
.quad 0

.section .note.GNU-stack, "", @progbits
