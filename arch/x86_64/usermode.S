/*
 * Entry to ring 3 for a process's thread (N2.3).
 *
 * void user_enter(uint64_t rip, uint64_t rsp)   (does not return)
 *   Called on the process's own thread and kernel stack, with IF = 0, CR3 =
 *   the process's root and TSS.RSP0 = the top of this kernel stack (both set
 *   by the scheduler). IRETQs to CS 0x23 / SS 0x1B with RFLAGS 0x202 (IF = 1,
 *   IOPL = 0, DF = 0) and every general-purpose register zero; DS, ES, FS and
 *   GS are null. The kernel IF stays 0 up to the IRETQ, which loads IF = 1
 *   atomically with the switch. The thread never comes back here: every later
 *   kernel entry is an interrupt on top of this kernel stack (the frames below
 *   it are abandoned), and the thread ends in thread_exit.
 */
.set USER_CS,       0x23
.set USER_DS,       0x1B
.set USER_RFLAGS,   0x202

.section .text
.code64

.global user_enter
.type user_enter, @function
user_enter:
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
.size user_enter, . - user_enter

.section .note.GNU-stack, "", @progbits
