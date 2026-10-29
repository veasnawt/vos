/*
 * USB.2-C3a.2: early UEFI firmware memory exclusion.
 *
 * GRUB's tag-6 memory map reports EFI BootServicesCode/Data as available RAM,
 * yet firmware drivers leave DMA engines pointing into it after
 * ExitBootServices. Before pmm_init, the Rust decision (vos_efi_exclusions_v1)
 * classifies the EFI memory map (tag 17) against tag 6 and emits the ranges of
 * available RAM that must never be allocatable; they enter the reservation
 * table, which pmm_init applies with its clear-only protect pass. The EFI map
 * can therefore remove frames but never add any. There is no opt-out and no
 * release path. A UEFI boot without a valid EFI map is refused.
 */
#ifndef VOS_X86_64_EFI_MEMORY_H
#define VOS_X86_64_EFI_MEMORY_H

#include <stdint.h>

#include "bootinfo.h"
#include "resv.h"

/* One object, so a reservation entry's origin is checked by identity. */
extern const char efi_memory_what[];
#define EFI_MEMORY_WHAT         efi_memory_what
/* Validated floor: the desktop/application run peaks at 57 frames (gdbstub
 * watchpoint low-water); 2048 frames (8 MiB) leaves a >35x margin. */
#define EFI_MEMORY_FLOOR_FRAMES 2048

#define EFI_MEMORY_APPLIED      0
#define EFI_MEMORY_NOT_EFI      1   /* legacy BIOS: the e820 map alone applies */
#define EFI_MEMORY_REFUSED      (-1)

/* Layouts asserted in arch/x86_64/rust/efi_exclusion.rs. */
struct efi_memory_input {
    const void *mmap, *efi_mmap;        /* whole tags, header included */
    uint64_t window_lo, window_hi;      /* tracked physical window, page-aligned */
    uint32_t mmap_size, efi_mmap_size;
    uint32_t efi_mmap_tags, efi_firmware, boot_services_active;
    uint32_t capacity;                  /* free reservation-table entries */
};

struct efi_memory_report {
    uint64_t frames;                    /* frames inside the excluded ranges */
    int32_t status;                     /* EFI_MEMORY_* */
    uint32_t error;                     /* nonzero when refused */
    uint32_t descriptors, ranges, coalesced, reserved;
};

int vos_efi_exclusions_v1(const struct efi_memory_input *in,
                          int (*sink)(void *ctx, uint64_t start, uint64_t end),
                          void *ctx, struct efi_memory_report *out);

/* Classify and enter exclusions into `t`; returns report->status. */
int efi_memory_exclude(const struct boot_info *bi, struct resv_table *t,
                       struct efi_memory_report *report);
const char *efi_memory_strerror(uint32_t error);

/* USB.2-C3a.3: 1 if `frame` lies in an exclusion this module entered into `t`
 * and the Rust re-derivation from the EFI map confirms the whole frame is
 * excluded tag-6 RAM; 0 otherwise (including BIOS and any invalid input). */
int vos_efi_firmware_frame_v1(const struct efi_memory_input *in, uint64_t frame);
int efi_memory_firmware_frame(const struct boot_info *bi, const struct resv_table *t, uint64_t frame);

#endif
