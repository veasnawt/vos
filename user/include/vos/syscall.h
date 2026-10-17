/*
 * VOS user interface to system-call ABI v0 (internal, experimental, unstable).
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

#define VOS_ENOSYS              1       /* unknown system call */
#define VOS_EINVAL              2       /* invalid argument (e.g. length > 256) */
#define VOS_EFAULT              3       /* buffer not readable user memory */

#define VOS_DEBUG_WRITE_MAX     256

#ifndef __ASSEMBLER__
/*
 * Write up to VOS_DEBUG_WRITE_MAX bytes to the kernel's debug console. The
 * kernel prints them as "user[<name>]: ..." lines with non-printable bytes
 * replaced. Returns len, -VOS_EINVAL or -VOS_EFAULT.
 */
long vos_debug_write(const void *buf, unsigned long len);

/* End the process with the low 32 bits of status. Does not return. */
__attribute__((noreturn)) void vos_exit(int status);
#endif

#endif
