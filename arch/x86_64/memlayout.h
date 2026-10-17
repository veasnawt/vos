/* Address-layout constants and the boot-time physical-to-virtual mapping. */
#ifndef VOS_X86_64_MEMLAYOUT_H
#define VOS_X86_64_MEMLAYOUT_H

#include <stdint.h>

#define KERNEL_VMA      0xFFFFFFFF80000000ULL
#define BOOT_MAP_LIMIT  0x40000000ULL   /* boot page tables map phys [0, 1 GiB) */
#define PAGE_SIZE       0x1000ULL
#define VGA_TEXT_PHYS   0xB8000ULL

/* Valid only for phys < BOOT_MAP_LIMIT. */
static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + KERNEL_VMA);
}

#endif
