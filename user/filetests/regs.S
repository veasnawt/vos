#include <vos/file.h>
.macro check reg, value
    movabs $\value, %rax
    cmp %rax, \reg
    jne failed
.endm
.section .text
.global main
.type main,@function
main:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    xor %edi,%edi
    mov $VOS_SYS_FILE_INITIAL,%eax
    int $0x80
    test %rax,%rax
    js failed
    mov %rax,handle(%rip)
    mov $1,%edi
    xor %esi,%esi
    xor %edx,%edx
    mov $VOS_SYS_CHANNEL_RECV,%eax
    int $0x80
    mov %rsp,saved_rsp(%rip)
    movabs $0x123456789abcdef0,%rax
    mov %rax,-128(%rsp)
    movabs $0x1111111111111111,%rbx
    movabs $0x2222222222222222,%rcx
    movabs $0x3333333333333333,%rbp
    movabs $0x4444444444444444,%r8
    movabs $0x5555555555555555,%r9
    movabs $0x6666666666666666,%r10
    movabs $0x7777777777777777,%r11
    movabs $0x8888888888888888,%r12
    movabs $0x9999999999999999,%r13
    movabs $0xaaaaaaaaaaaaaaaa,%r14
    movabs $0xbbbbbbbbbbbbbbbb,%r15
    mov handle(%rip),%rdi
    lea output(%rip),%rsi
    mov $64,%edx
    stc
    pushfq
    popq saved_flags(%rip)
    mov $VOS_SYS_FILE_STAT,%eax
    int $0x80
    pushfq
    popq returned_flags(%rip)
    test %rax,%rax
    jnz failed
    check %rbx,0x1111111111111111
    check %rcx,0x2222222222222222
    check %rbp,0x3333333333333333
    check %r8,0x4444444444444444
    check %r9,0x5555555555555555
    check %r10,0x6666666666666666
    check %r11,0x7777777777777777
    check %r12,0x8888888888888888
    check %r13,0x9999999999999999
    check %r14,0xaaaaaaaaaaaaaaaa
    check %r15,0xbbbbbbbbbbbbbbbb
    check %rdx,64
    mov handle(%rip),%rax
    cmp %rax,%rdi
    jne failed
    lea output(%rip),%rax
    cmp %rax,%rsi
    jne failed
    cmp saved_rsp(%rip),%rsp
    jne failed
    mov saved_flags(%rip),%rax
    cmp returned_flags(%rip),%rax
    jne failed
    check -128(%rsp),0x123456789abcdef0
    xor %eax,%eax
    jmp done
failed:
    mov $1,%eax
done:
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
.size main,.-main
.section .bss
.balign 8
handle: .skip 8
saved_rsp: .skip 8
saved_flags: .skip 8
returned_flags: .skip 8
output: .skip 64
.section .note.GNU-stack,"",@progbits
