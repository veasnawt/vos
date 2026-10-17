/*
 * N2.1: every general-purpose register but rsp holds a distinct pattern and
 * DF is set; the loop compares them with copies on the stack forever. It
 * exits 1 if any register changed and 2 if DF was cleared, so an interrupt
 * that does not restore the interrupted state exactly ends the program with
 * a status. Otherwise only the kernel's test tick budget ends it. Called as
 * main by crt0.
 */
#include <vos/syscall.h>

.macro CHECK reg, slot
    cmp (8 * \slot)(%rsp), \reg
    jne 8f
.endm

.section .text
.global main
.type main, @function
main:
    .irp k,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14
    movabs $(0x0102030405060708 * (\k + 1) + 0x1111111111111111), %rax
    push %rax
    .endr
    mov (8 * 14)(%rsp), %rax
    mov (8 * 13)(%rsp), %rbx
    mov (8 * 12)(%rsp), %rcx
    mov (8 * 11)(%rsp), %rdx
    mov (8 * 10)(%rsp), %rsi
    mov (8 * 9)(%rsp), %rdi
    mov (8 * 8)(%rsp), %rbp
    mov (8 * 7)(%rsp), %r8
    mov (8 * 6)(%rsp), %r9
    mov (8 * 5)(%rsp), %r10
    mov (8 * 4)(%rsp), %r11
    mov (8 * 3)(%rsp), %r12
    mov (8 * 2)(%rsp), %r13
    mov (8 * 1)(%rsp), %r14
    mov (8 * 0)(%rsp), %r15
    std
1:  CHECK %rax, 14
    CHECK %rbx, 13
    CHECK %rcx, 12
    CHECK %rdx, 11
    CHECK %rsi, 10
    CHECK %rdi, 9
    CHECK %rbp, 8
    CHECK %r8, 7
    CHECK %r9, 6
    CHECK %r10, 5
    CHECK %r11, 4
    CHECK %r12, 3
    CHECK %r13, 2
    CHECK %r14, 1
    CHECK %r15, 0
    pushfq
    testq $0x400, (%rsp)
    lea 8(%rsp), %rsp               /* lea keeps the flags of the test */
    jnz 1b
    mov $2, %edi
    jmp 9f
8:  mov $1, %edi
9:  cld
    mov $VOS_SYS_EXIT, %eax
    int $0x80
    ud2
.size main, . - main

.section .note.GNU-stack, "", @progbits
