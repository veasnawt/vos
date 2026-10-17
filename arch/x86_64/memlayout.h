/* Address-layout constants and the boot-time physical-to-virtual mapping. */
#ifndef VOS_X86_64_MEMLAYOUT_H
#define VOS_X86_64_MEMLAYOUT_H

#include <stdint.h>

#define KERNEL_VMA      0xFFFFFFFF80000000ULL
#define BOOT_MAP_LIMIT  0x40000000ULL   /* boot page tables map phys [0, 1 GiB) */
#define PAGE_SIZE       0x1000ULL
#define VGA_TEXT_PHYS   0xB8000ULL

/*
 * Virtual layout (plan section 6.1). The lower half belongs to future user
 * address spaces; the kernel lives only in the upper half.
 */
#define LOWER_HALF_END      0x0000800000000000ULL   /* first non-canonical address */
#define UPPER_HALF_START    0xFFFF800000000000ULL   /* first canonical upper-half address */
#define NULL_GUARD_END      0x0000000000400000ULL   /* [0, 4 MiB) is never mapped */
#define LOWER_MAP_END       0x00007FFFFFFFF000ULL   /* the highest lower-half page is never mapped */
#define USER_IMAGE_START    0x0000000000400000ULL   /* user image (N1.8) */
#define USER_IMAGE_END      0x0000000040000000ULL
#define USER_STACK_GUARD    0x00007FFFFFFFA000ULL   /* unmapped guard page below the stack */
#define USER_STACK_START    0x00007FFFFFFFB000ULL   /* 16 KiB user stack (N1.5) */
#define USER_STACK_END      0x00007FFFFFFFF000ULL
#define KERNEL_ALIAS_END    (KERNEL_VMA + BOOT_MAP_LIMIT)   /* alias of phys [0, 1 GiB) */

/* Valid only for phys < BOOT_MAP_LIMIT. */
static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + KERNEL_VMA);
}

#endif
