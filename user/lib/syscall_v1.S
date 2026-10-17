/*
 * System-call stubs added by ABI v1 (N2.4), in their own object so that ABI
 * v0 executables (hello) stay byte-for-byte unchanged. Same convention as
 * syscall.S: arguments in rdi, rsi, rdx; number in rax; result in rax; the
 * kernel preserves every other register and RFLAGS.
 */
#include <vos/syscall.h>

.section .text
.code64

/* long vos_yield(void) */
.global vos_yield
.type vos_yield, @function
vos_yield:
    mov $VOS_SYS_YIELD, %eax
    int $0x80
    ret
.size vos_yield, . - vos_yield

/* long vos_channel_send(long handle, const void *buf, unsigned long len) */
.global vos_channel_send
.type vos_channel_send, @function
vos_channel_send:
    mov $VOS_SYS_CHANNEL_SEND, %eax
    int $0x80
    ret
.size vos_channel_send, . - vos_channel_send

/* long vos_channel_recv(long handle, void *buf, unsigned long capacity) */
.global vos_channel_recv
.type vos_channel_recv, @function
vos_channel_recv:
    mov $VOS_SYS_CHANNEL_RECV, %eax
    int $0x80
    ret
.size vos_channel_recv, . - vos_channel_recv

/* long vos_handle_close(long handle) */
.global vos_handle_close
.type vos_handle_close, @function
vos_handle_close:
    mov $VOS_SYS_HANDLE_CLOSE, %eax
    int $0x80
    ret
.size vos_handle_close, . - vos_handle_close

.section .note.GNU-stack, "", @progbits
