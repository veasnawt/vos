/*
 * Entry stubs for exception vectors 0-31 and the provisional system-call
 * vector 0x80. Each stub normalizes the stack to the layout of struct
 * exc_frame (error code, vector, then 15 GPRs), clears DF and calls
 * exception_handler(frame) on a 16-byte-aligned stack. In 64-bit mode the
 * CPU pushes SS:RSP for every interrupt, with or without a privilege change,
 * so the layout is the same for traps from ring 0 and ring 3 (from ring 3 it
 * is on the TSS.RSP0 stack). The handler never returns: kernel traps panic,
 * user traps resume the kernel through user_return.
 */
.macro EXC_STUB n, has_err
exc_stub_\n:
.if !\has_err
    pushq $0                        /* dummy error code */
.endif
    pushq $\n
    jmp exc_common
.endm

.section .text
.code64

EXC_STUB 0, 0
EXC_STUB 1, 0
EXC_STUB 2, 0
EXC_STUB 3, 0
EXC_STUB 4, 0
EXC_STUB 5, 0
EXC_STUB 6, 0
EXC_STUB 7, 0
EXC_STUB 8, 1
EXC_STUB 9, 0
EXC_STUB 10, 1
EXC_STUB 11, 1
EXC_STUB 12, 1
EXC_STUB 13, 1
EXC_STUB 14, 1
EXC_STUB 15, 0
EXC_STUB 16, 0
EXC_STUB 17, 1
EXC_STUB 18, 0
EXC_STUB 19, 0
EXC_STUB 20, 0
EXC_STUB 21, 1
EXC_STUB 22, 0
EXC_STUB 23, 0
EXC_STUB 24, 0
EXC_STUB 25, 0
EXC_STUB 26, 0
EXC_STUB 27, 0
EXC_STUB 28, 0
EXC_STUB 29, 1
EXC_STUB 30, 1
EXC_STUB 31, 0
EXC_STUB 128, 0                     /* int $0x80 from ring 3 (DPL 3 gate) */
.global exc_stub_128

exc_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    mov %rsp, %rdi                  /* struct exc_frame * */
    mov %rsp, %rbp
    and $-16, %rsp
    call exception_handler
1:  cli
    hlt
    jmp 1b

.section .rodata
.align 8
.global exc_stub_table
exc_stub_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .quad exc_stub_\n
.endr

.section .note.GNU-stack, "", @progbits
