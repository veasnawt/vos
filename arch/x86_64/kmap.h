/*
 * Kernel W^X map (N2.0).
 *
 * The boot code maps the kernel alias of phys [0, 1 GiB) at KERNEL_VMA with
 * 512 2 MiB pages that are all writable and executable. kmap_init replaces
 * the page directory under PDPT[510] with:
 *
 *   PD[0]       -> one page table for phys [0, 2 MiB), 4 KiB pages:
 *                    kernel .text                 R-X  (the only executable pages)
 *                    .rodata, boot code page(s)   R--  NX
 *                    stack guard pages            not present
 *                    everything else              RW-  NX  (low memory, boot page
 *                                                 tables, .data, .bss, info block,
 *                                                 modules)
 *   PD[1..511]  -> 2 MiB pages of phys [2 MiB, 1 GiB), RW- NX
 *
 * phys_to_virt() stays valid for every frame below 1 GiB except the guard
 * pages, which have no mapping at all. User address spaces copy PML4[511] by
 * value, so they share this page directory and see the same kernel half.
 *
 * The transition is controlled: the new tables are built and checked against
 * the addresses the running kernel needs (code, IDT handlers, stacks, GDT,
 * IDT, TSS, page tables, allocator bitmaps, boot information, modules)
 * before PDPT[510] is switched and every non-global translation is flushed;
 * the same checks then run on the live tables through the page-table walker.
 * If the post-switch check fails, the old directory is restored. Rollback
 * cannot help when the switch itself makes the running code, its stack or
 * the exception path unreachable: the CPU then faults (or triple-faults and
 * resets) before any check can run. That is why the pre-switch check exists.
 */
#ifndef VOS_X86_64_KMAP_H
#define VOS_X86_64_KMAP_H

#include <stdint.h>

#include "bootinfo.h"

#define KMAP_LARGE_PAGES    511     /* 2 MiB pages: PD[1..511] */
#define KMAP_SMALL_SPAN     0x200000ULL     /* PD[0] covers phys [0, 2 MiB) with 4 KiB pages */
#define KMAP_MAX_GUARDS     8

/* Result of a check: the first failure found. */
struct kmap_fail {
    const char *what;               /* the object checked, e.g. "kernel stack" */
    const char *problem;            /* e.g. "not writable" */
    uint64_t va;
};

/* Build, check, switch, check again, retire the boot directory. 0 or -1 (see kmap_failure). */
int kmap_init(const struct boot_info *bi);
const struct kmap_fail *kmap_failure(void);
int kmap_active(void);

/*
 * The intended layout, from the linker symbols and the guard list only:
 * the leaf entry (without A/D) for the 4 KiB page at phys pa < 2 MiB (0 =
 * not present), and the 2 MiB entry for PD index i in 1..511.
 */
uint64_t kmap_expected_pte(uint64_t pa);
uint64_t kmap_expected_pde(unsigned i);
/* Virtual addresses of all kernel stack guard pages; returns their number. */
unsigned kmap_guard_pages(uint64_t *va, unsigned max);
/* Nonzero if phys pa lies in the kernel .text (the only frames executable in ring 0). */
int kmap_is_text(uint64_t pa);
/* Nonzero if the 4 KiB frame containing phys pa is a stack guard page. */
int kmap_is_guard(uint64_t pa);

/*
 * Check a candidate page directory and page table (pd[0] must point to
 * pt_pa). With live set, translations go through vmm_lookup on the active
 * root instead of the given arrays. Returns 0 if every check passed, else -1
 * and records the first failure.
 */
int kmap_check(const uint64_t *pd, const uint64_t *pt, uint64_t pt_pa, int live,
               const struct boot_info *bi);

#ifdef VOS_TEST
/* Test hook: the live directory and table, and the physical address of the table. */
void kmap_test_tables(const uint64_t **pd, const uint64_t **pt, uint64_t *pt_pa);
#endif

#endif
