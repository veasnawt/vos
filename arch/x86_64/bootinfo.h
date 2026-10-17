/*
 * Multiboot2 boot information: validated parsing and physical reservations.
 * Both functions return NULL on success or a static error string; neither
 * trusts the loader's data.
 */
#ifndef VOS_X86_64_BOOTINFO_H
#define VOS_X86_64_BOOTINFO_H

#include <stdint.h>

#include "multiboot2.h"
#include "resv.h"

#define BOOT_MAX_MODULES    64      /* the N1.9 isolation suite delivers ~42 */
#define MB2_INFO_MAX        0x10000u    /* sanity bound for the info block */

struct boot_module {
    uint64_t start, end;                /* [start, end), physical, below 1 GiB */
    const char *cmdline;                /* NUL-terminated, points into the info block */
};

struct boot_info {
    uint64_t info_start, info_end;      /* Multiboot2 information block (physical) */
    const char *loader;                 /* NULL if absent */
    const char *cmdline;                /* kernel command line; NULL if absent */
    const struct mb2_tag_mmap *mmap;
    uint32_t mmap_entries;
    struct boot_module modules[BOOT_MAX_MODULES];
    uint32_t module_count;
};

/*
 * Parse the info block read through `info` (a virtual pointer) that lives at
 * physical address `info_phys`. Rejects malformed tags, unterminated strings,
 * empty/reversed modules, modules at or above 1 GiB, more than
 * BOOT_MAX_MODULES modules, and duplicate command-line/memory-map tags. The
 * memory map must have entry_version 0, entry_size >= 24 and a multiple of 8,
 * at least one entry, and no trailing partial entry.
 */
const char *mb2_parse(const void *info, uint64_t info_phys, struct boot_info *bi);

/* Entry i (< bi->mmap_entries) of the validated memory map. */
const struct mb2_mmap_entry *boot_mmap_entry(const struct boot_info *bi, uint32_t i);

/*
 * Add the boot reservations to `t`: physical 0-1 MiB, the kernel image
 * [kstart, kend), the info block and every module. Each non-low range must lie
 * entirely in available RAM and intersect no other memory-map entry type;
 * modules must also not overlap the kernel's page span, the info block or one
 * another. Entries added before an error are kept.
 */
const char *boot_reserve(const struct boot_info *bi, uint64_t kstart, uint64_t kend,
                         struct resv_table *t);

#endif
