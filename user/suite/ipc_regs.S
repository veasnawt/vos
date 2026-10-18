/*
 * N2.4 (ABI v1): every register but rax holds a pattern across a
 * channel_send of 8 bytes and a blocking channel_recv of the echo on handle
 * 1 (and a yield); exit 0 if all survived and the echo matches, else 1.
 * Called as main by crt0.
 */
#include <vos/syscall.h>

.macro CHK reg, val
    movabs $\val, %rax
    cmp %rax, \reg
    jne 8f
.endm
.macro LOAD
    movabs $0x1111111111111111, %rbx
    movabs $0x2222222222222222, %rcx
    movabs $0x6666666666666666, %rbp
    movabs $0x7777777777777777, %r8
    movabs $0x8888888888888888, %r9
    movabs $0x9999999999999999, %r10
    movabs $0xAAAAAAAAAAAAAAAA, %r11
    movabs $0xBBBBBBBBBBBBBBBB, %r12
    movabs $0xCCCCCCCCCCCCCCCC, %r13
    movabs $0xDDDDDDDDDDDDDDDD, %r14
    movabs $0xEEEEEEEEEEEEEEEE, %r15
.endm
.macro CHECKALL
    CHK %rbx, 0x1111111111111111
    CHK %rcx, 0x2222222222222222
    CHK %rbp, 0x6666666666666666
    CHK %r8, 0x7777777777777777
    CHK %r9, 0x8888888888888888
    CHK %r10, 0x9999999999999999
    CHK %r11, 0xAAAAAAAAAAAAAAAA
    CHK %r12, 0xBBBBBBBBBBBBBBBB
    CHK %r13, 0xCCCCCCCCCCCCCCCC
    CHK %r14, 0xDDDDDDDDDDDDDDDD
    CHK %r15, 0xEEEEEEEEEEEEEEEE
.endm

.section .text
.global main
.type main, @function
main:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    sub $24, %rsp
    movabs $0x5245475354455354, %rax
    mov %rax, (%rsp)                /* the message */
    LOAD
    mov $1, %edi
    mov %rsp, %rsi
    mov $8, %edx
    mov $VOS_SYS_CHANNEL_SEND, %eax
    int $0x80
    test %rax, %rax
    jnz 8f
    CHECKALL
    mov $VOS_SYS_YIELD, %eax
    int $0x80
    CHECKALL
    mov $1, %edi
    lea 8(%rsp), %rsi
    mov $16, %edx
    mov $VOS_SYS_CHANNEL_RECV, %eax
    int $0x80
    cmp $8, %rax
    jne 8f
    CHECKALL
    CHK %rdi, 1
    mov 8(%rsp), %rax
    cmp (%rsp), %rax
    jne 8f
    xor %eax, %eax
    jmp 9f
8:  mov $1, %eax
9:  add $24, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
.size main, . - main

.section .note.GNU-stack, "", @progbits
