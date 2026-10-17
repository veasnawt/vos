/* Copying from user memory (plan section 9.3). */
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

#endif
