/*
 * VOS system-call ABI v0 (internal, experimental, unstable; plan section 8.1).
 *
 *   Entry: int $0x80 from ring 3. Number in rax; arguments in rdi, rsi, rdx,
 *   r10, r8, r9 (unused ones are ignored); result in rax: >= 0 success,
 *   negative = -VOS_E* (VOS numbers, not Linux numbers).
 *   Every other general-purpose register and RFLAGS are preserved; the user
 *   stack (including the 128-byte red zone) is never touched by the kernel.
 *
 *   0  exit(status)              rdi low 32 bits, signed; does not return
 *   1  debug_write(buf, len)     len <= 256; bytes written, -VOS_EINVAL, -VOS_EFAULT
 *   *  anything else             -VOS_ENOSYS; the process continues
 *
 *   x87, MMX and SSE are unavailable in ring 3 (#NM / #UD).
 */
#ifndef VOS_X86_64_SYSCALL_H
#define VOS_X86_64_SYSCALL_H

#include <stdint.h>

#define SYS_EXIT            0
#define SYS_DEBUG_WRITE     1
#define SYS_YIELD           2   /* ABI v1 (N2.4), docs/abi/syscall-v1.md */
#define SYS_CHANNEL_SEND    3   /* ABI v1 */
#define SYS_CHANNEL_RECV    4   /* ABI v1 */
#define SYS_HANDLE_CLOSE    5   /* ABI v1 */
#define SYS_FILE_OPEN_AT    6   /* ABI v2 */
#define SYS_FILE_READ_AT    7
#define SYS_FILE_STAT       8
#define SYS_DIRECTORY_NEXT  9
#define SYS_FILE_INITIAL   10
#define SYS_CONSOLE_READ   11   /* ABI v3, explicit console grant */
#define SYS_CONSOLE_WRITE  12
#define SYS_CONSOLE_CLEAR  13

#define VOS_ENOSYS          1
#define VOS_EINVAL          2
#define VOS_EFAULT          3
#define VOS_EAGAIN          4   /* ABI v1 */
#define VOS_EPIPE           5
#define VOS_EBADHANDLE      6
#define VOS_ENOMEM          7
#define VOS_EMSGSIZE        8
#define VOS_EACCES          9
#define VOS_ENOENT         10
#define VOS_ENOTDIR        11
#define VOS_EISDIR         12
#define VOS_ENAMETOOLONG   13
#define VOS_ELOOP          14
#define VOS_EIO            15
#define VOS_EUNAVAIL       16
#define VOS_ESTALE         17
#define VOS_ETIMEDOUT      18
#define VOS_EINTR          19
#define VOS_EUNSUPP        20

#define DEBUG_WRITE_MAX     256
#define MSG_MAX             256 /* ABI v1: largest channel message */

#ifdef VOS_TEST
/* Test builds only, never part of the ABI. */
#define SYS_TEST_KERNEL_AC  0x7E5A  /* returns 1 if RFLAGS.AC is set inside the kernel */
#define SYS_TEST_TOUCH_USER 0x7E57  /* the kernel reads the byte at rdi directly: a simulated bug */
#define SYS_TEST_KERNEL_IF  0x7E1F  /* returns 1 if RFLAGS.IF is set inside the kernel (must be 0) */
#endif

struct process;
struct exc_frame;

/*
 * Write untrusted bytes as lines "user[<name>]: <text>\r\n" through put().
 * '\n' ends a line; every other byte outside printable ASCII (0x20-0x7E),
 * including '\r' and escape sequences, becomes '?'. Every line, also the
 * first one after an embedded newline, carries the kernel-generated prefix,
 * and the output always ends at a line end. len 0 writes nothing.
 */
void debug_emit(const char *name, const char *data, uint64_t len, void (*put)(char));

/* Handle every system call except exit; returns the value for the user's rax. */
int64_t syscall_dispatch(struct process *p, struct exc_frame *f);

#endif
