/* Copying from and to user memory (plan section 9.3; copy_to_user since N2.4). */
#ifndef VOS_X86_64_USERCOPY_H
#define VOS_X86_64_USERCOPY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Copy `len` bytes at user address `uptr` in the address space `root` into
 * the kernel buffer `dst`. Returns 0, or -VOS_EFAULT with nothing copied.
 *
 *   - len == 0: returns 0 without examining uptr.
 *   - [uptr, uptr + len) must not overflow and must lie in
 *     [0x400000, 0x0000800000000000): this rejects null and the null guard,
 *     non-canonical addresses and the whole upper (kernel) half.
 *   - every 4 KiB page the range touches must be mapped as a 4 KiB page with
 *     the user bit set at all four levels (vmm_lookup's effective flags) and a
 *     frame below 1 GiB; all pages are checked before the first byte is copied.
 *   - the bytes are then read through the kernel alias of the validated
 *     frames: the user address itself is never dereferenced by the kernel, so
 *     the copy cannot fault and does not depend on SMAP. Each requested byte is
 *     read exactly once; nothing beyond len is read.
 *
 * Callers enforce their own length limits (-VOS_EINVAL) before calling.
 * Assumption (N1, still true in N2.1): one process, one CPU, the kernel runs
 * with interrupts disabled (IRQs only interrupt ring 3), no demand paging
 * or shared mappings, so nothing can change the page tables between the
 * checks and the copy.
 */
int copy_from_user(uint64_t root, void *dst, uint64_t uptr, size_t len);

/*
 * Copy `len` bytes from the kernel buffer `src` to user address `uptr` in the
 * address space `root` (N2.4). Returns 0, or -VOS_EFAULT with nothing
 * written. Same rules as copy_from_user, and every page must also be
 * writable by the user (effective W at every level): code and read-only data
 * pages are refused like unmapped or supervisor pages. The whole range is
 * validated before the first byte is written, and the bytes go through the
 * kernel alias of the validated frames, never through the user address (so
 * the write cannot fault and does not depend on SMAP). len == 0 returns 0
 * without examining uptr. The same single-CPU, IF = 0 argument as above
 * keeps the mappings stable between the check and the copy: the kernel is
 * not preempted inside it, and only the process's own (blocked) thread or
 * its teardown, after the thread is gone, changes its page tables.
 */
int copy_to_user(uint64_t root, uint64_t uptr, const void *src, size_t len);

/* Same full destination checks, without writing; callers must recheck after waits. */
int user_writable(uint64_t root, uint64_t uptr, size_t len);

#endif
