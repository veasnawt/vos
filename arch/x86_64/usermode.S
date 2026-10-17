/*
 * Ring 0 <-> ring 3 transitions for the single synchronous process.
 *
 * int enter_user(uint64_t rip, uint64_t rsp, uint64_t cr3, uint64_t *kernel_rsp)
 *   Saves the callee-saved registers on the current (kernel) stack, stores
 *   that RSP in *kernel_rsp, switches to the user address space and IRETQs
 *   to CS 0x23 / SS 0x1B with RFLAGS 0x202 (IF = 1, IOPL = 0, DF = 0; since
 *   N2.1 hardware interrupts reach ring 3) and every general-purpose register
 *   zero. It "returns" only through user_return. The kernel itself keeps
 *   IF = 0 up to the IRETQ, which loads IF = 1 atomically with the switch.
 *
 * void user_return(uint64_t *kernel_rsp, int result)   (does not return)
 *   Called on the TSS.RSP0 stack by the trap handler, after it has switched
 *   back to the kernel CR3. Restores the stack saved by enter_user, reloads
 *   the kernel data selectors (a CPL 3 -> 0 transition leaves SS null), and
 *   returns `result` from enter_user. The RSP0 stack contents are abandoned.
 */
.set USER_CS,       0x23
.set USER_DS,       0x1B
.set KERNEL_DS,     0x10
.set USER_RFLAGS,   0x202

.section .text
.code64

.global enter_user
.type enter_user, @function
enter_user:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    mov %rsp, (%rcx)                /* kernel context for user_return */
    mov %rdx, %cr3                  /* kernel half is shared, so execution continues */

    pushq $USER_DS                  /* SS */
    pushq %rsi                      /* RSP */
    pushq $USER_RFLAGS              /* RFLAGS */
    pushq $USER_CS                  /* CS */
    pushq %rdi                      /* RIP */

    xor %eax, %eax                  /* null data selectors: nothing kernel-defined leaks */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    xor %ebx, %ebx
    xor %ecx, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    xor %ebp, %ebp
    xor %r8d, %r8d
    xor %r9d, %r9d
    xor %r10d, %r10d
    xor %r11d, %r11d
    xor %r12d, %r12d
    xor %r13d, %r13d
    xor %r14d, %r14d
    xor %r15d, %r15d
    iretq
.size enter_user, . - enter_user

.global user_return
.type user_return, @function
user_return:
    mov (%rdi), %rsp
    movq $0, (%rdi)                 /* the saved context can be used once */
    mov $KERNEL_DS, %ecx
    mov %cx, %ss
    mov %cx, %ds
    mov %cx, %es
    mov %esi, %eax
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size user_return, . - user_return

.section .note.GNU-stack, "", @progbits
