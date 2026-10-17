/*
 * System-call stubs for ABI v0: the only place in user space where int $0x80
 * appears. Each stub is an ordinary System V function: its arguments already
 * sit in rdi, rsi (rdx, r10, r8, r9 for later calls; a stub with a 4th
 * argument must move rcx to r10), the number goes in rax and the result
 * comes back in rax. The kernel preserves every other register and RFLAGS,
 * which is stricter than what the compiler assumes for a call, and never
 * writes to the user stack.
 */
#include <vos/syscall.h>

.section .text
.code64

/* long vos_debug_write(const void *buf, unsigned long len) */
.global vos_debug_write
.type vos_debug_write, @function
vos_debug_write:
    mov $VOS_SYS_DEBUG_WRITE, %eax
    int $0x80
    ret
.size vos_debug_write, . - vos_debug_write

/* void vos_exit(int status) -- does not return */
.global vos_exit
.type vos_exit, @function
vos_exit:
    mov $VOS_SYS_EXIT, %eax
    int $0x80
    ud2                             /* exit never returns; trap if it ever did */
.size vos_exit, . - vos_exit

.section .note.GNU-stack, "", @progbits
