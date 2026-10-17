/*
 * VOS user interface to system-call ABI v0 and v1 (internal, experimental,
 * unstable). ABI v1 (N2.4, docs/abi/syscall-v1.md) adds yield and channels;
 * only executables whose VOS ABI note says 1 (abi_note_v1.o) may use them,
 * an ABI v0 executable gets -VOS_ENOSYS.
 *
 * The values below are ABI constants shared with the kernel by specification
 * (docs/plans/n1-user-space.md, section 8.1), not by including kernel headers:
 * the user tree never includes kernel sources. A build test compares them.
 *
 * Results are returned directly: a non-negative value on success or a
 * negative VOS error (-VOS_E*). There is no errno and these numbers are not
 * Linux numbers.
 */
#ifndef VOS_SYSCALL_H
#define VOS_SYSCALL_H

#define VOS_SYS_EXIT            0
#define VOS_SYS_DEBUG_WRITE     1
#define VOS_SYS_YIELD           2       /* ABI v1 */
#define VOS_SYS_CHANNEL_SEND    3       /* ABI v1 */
#define VOS_SYS_CHANNEL_RECV    4       /* ABI v1 */
#define VOS_SYS_HANDLE_CLOSE    5       /* ABI v1 */

#define VOS_ENOSYS              1       /* unknown system call */
#define VOS_EINVAL              2       /* invalid argument (e.g. length > 256) */
#define VOS_EFAULT              3       /* buffer not readable (or writable) user memory */
#define VOS_EAGAIN              4       /* ABI v1: the peer's queue is full; try again */
#define VOS_EPIPE               5       /* ABI v1: the peer endpoint is closed */
#define VOS_EBADHANDLE          6       /* ABI v1: not a handle of this process */
#define VOS_ENOMEM              7       /* ABI v1: a kernel limit was reached */
#define VOS_EMSGSIZE            8       /* ABI v1: the receive buffer is smaller than the message */
#define VOS_EACCES              9       /* ABI v1: the handle lacks the right for this operation */

#define VOS_DEBUG_WRITE_MAX     256
#define VOS_MSG_MAX             256     /* ABI v1: largest channel message */

#ifndef __ASSEMBLER__
/*
 * Write up to VOS_DEBUG_WRITE_MAX bytes to the kernel's debug console. The
 * kernel prints them as "user[<name>]: ..." lines with non-printable bytes
 * replaced. Returns len, -VOS_EINVAL or -VOS_EFAULT.
 */
long vos_debug_write(const void *buf, unsigned long len);

/* End the process with the low 32 bits of status. Does not return. */
__attribute__((noreturn)) void vos_exit(int status);

/* ABI v1 (syscall_v1.o; see docs/abi/syscall-v1.md for the full contract). */
long vos_yield(void);
/* Queue len (0..VOS_MSG_MAX) bytes to the peer; never blocks. Returns 0 or -VOS_E*. */
long vos_channel_send(long handle, const void *buf, unsigned long len);
/* Take the oldest message into buf (capacity bytes); blocks while none is queued and the peer is
 * open. Returns the message length or -VOS_E*; a message larger than capacity stays queued. */
long vos_channel_recv(long handle, void *buf, unsigned long capacity);
/* Close the handle (and its endpoint). Returns 0 or -VOS_EBADHANDLE. */
long vos_handle_close(long handle);
#endif

#endif
