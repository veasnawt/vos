/*
 * Kernel W^X map (N2.0).
 *
 * The boot code maps the kernel alias of phys [0, 1 GiB) at KERNEL_VMA with
 * 512 2 MiB pages that are all writable and executable. kmap_init replaces
 * the page directory under PDPT[510] with:
 *
 *   PD[0..1]    -> two page tables, contiguous in .bss, for phys [0, 4 MiB)
 *                  in 4 KiB pages (N3.0; one table for [0, 2 MiB) before):
 *                    kernel .text                 R-X  (the only executable pages)
 *                    .rodata, boot code page(s)   R--  NX
 *                    stack guard pages            not present
 *                    everything else              RW-  NX  (low memory, boot page
 *                                                 tables, .data, .bss, info block,
 *                                                 modules, free frames)
 *   PD[2..511]  -> 2 MiB pages of phys [4 MiB, 1 GiB), RW- NX
 *
 * The kernel image must end below KMAP_SMALL_SPAN (a linker assertion and
 * kmap_init check it): only 4 KiB pages can give its text, read-only data
 * and guard pages their own permissions.
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
 * Right before the switch the text and a guard page are accessed through the
 * old map; right after it, a write to the text and a read of the guard must
 * fault (probes recovered by kmap_probe_fault), which shows the TLB flush
 * took effect. If any post-switch check fails, the old directory is restored. Rollback
 * cannot help when the switch itself makes the running code, its stack or
 * the exception path unreachable: the CPU then faults (or triple-faults and
 * resets) before any check can run. That is why the pre-switch check exists.
 */
#ifndef VOS_X86_64_KMAP_H
#define VOS_X86_64_KMAP_H

#include <stdint.h>

#include "bootinfo.h"

#define KMAP_SMALL_TABLES   2       /* 4 KiB page tables: PD[0..1] */
#define KMAP_SMALL_SPAN     (KMAP_SMALL_TABLES * 0x200000ULL)  /* phys [0, 4 MiB) in 4 KiB pages */
#define KMAP_LARGE_PAGES    (512 - KMAP_SMALL_TABLES)           /* 2 MiB pages: PD[2..511] */
#define KMAP_MAX_GUARDS     32

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
 * the leaf entry (without A/D) for the 4 KiB page at phys pa <
 * KMAP_SMALL_SPAN (0 = not present), and the 2 MiB entry for PD index i in
 * KMAP_SMALL_TABLES..511 (0 for the indexes that hold page tables).
 */
uint64_t kmap_expected_pte(uint64_t pa);
uint64_t kmap_expected_pde(unsigned i);
/* Virtual addresses of all kernel stack guard pages; returns their number. */
unsigned kmap_guard_pages(uint64_t *va, unsigned max);
/* Nonzero if phys pa lies in the kernel .text (the only frames executable in ring 0). */
int kmap_is_text(uint64_t pa);
/* Nonzero if the frames [pa, pa + size) overlap the kernel .text. */
int kmap_text_overlaps(uint64_t pa, uint64_t size);
/* Nonzero if the 4 KiB frame containing phys pa is a stack guard page. */
int kmap_is_guard(uint64_t pa);

/*
 * Check a candidate page directory and its KMAP_SMALL_TABLES page tables,
 * which are contiguous: pt holds KMAP_SMALL_TABLES * 512 entries and pd[t]
 * must point to pt_pa + t * 4 KiB. First the layout itself (every entry
 * against kmap_expected_*), then, independently of that layout, the
 * addresses the running kernel needs (see kmap_init). With live set, translations go through vmm_lookup
 * on the active root instead of the given arrays. Returns 0 if every check
 * passed, else -1 and records the first failure.
 */
int kmap_check(const uint64_t *pd, const uint64_t *pt, uint64_t pt_pa, int live,
               const struct boot_info *bi);
/* Only the second half of kmap_check: the addresses the running kernel needs. */
int kmap_check_running(const uint64_t *pd, const uint64_t *pt, uint64_t pt_pa, int live,
                       const struct boot_info *bi);

struct exc_frame;
/*
 * Called by the exception handler for every ring-0 trap: if it is a #PF of
 * one of kmap_init's armed probe instructions (exactly those two addresses,
 * only while armed), record it, redirect the frame to the probe's return
 * and return 1; otherwise 0, and the fault is handled as a kernel error.
 */
int kmap_probe_fault(struct exc_frame *f);

#ifdef VOS_TEST
/* Test hook: the address of the armed-probe load instruction (to show it is inert later). */
uint64_t kmap_test_probe_insn(void);
/* Test hook: the live directory and tables, and the physical address of the first table. */
void kmap_test_tables(const uint64_t **pd, const uint64_t **pt, uint64_t *pt_pa);
#endif

#endif
