/*
 * Kernel-only device-memory mappings (U1): the framebuffer, and later other
 * MMIO, never ordinary RAM.
 *
 * Window. Kernel PDPT entry DEVMAP_PDPT_INDEX (VA DEVMAP_VA_BASE, just below
 * the kernel alias). The PDPT is shared by every address space, but every
 * entry in the window is supervisor-only, so ring 3 never reaches it. One
 * page directory and its page tables come from the PMM; mappings are 4 KiB
 * pages, present, writable, NX, never user and never global, each followed
 * by an unmapped guard page. At most DEVMAP_MAX mappings and DEVMAP_MAX_BYTES
 * in all. Legacy devmap_map mappings are permanent; the non-access fixture
 * lease below is the sole scoped exception and owns only its fresh tables.
 *
 * What may be mapped. Physical [pa, pa + len) rounded out to pages must
 *   - not overflow, and lie below 2^MAXPHYADDR (CPUID 0x80000008);
 *   - intersect no memory-map entry of type available, ACPI reclaimable,
 *     ACPI NVS, bad or any unknown type: those are RAM, owned by the PMM or
 *     the firmware. Only "reserved" entries and addresses no entry lists
 *     (the MMIO holes) are device memory here;
 *   - not overlap another device mapping (one memory type per frame, no
 *     aliases with different attributes).
 *
 * Memory types. DEVMAP_UC: PCD|PWT, PAT entry 3, which must hold UC.
 * DEVMAP_WC: the PAT bit (entry 4), programmed to write-combining by
 * devmap_init when the CPU has a PAT. Entries 0-3, which every other mapping
 * uses, are verified unchanged, and no other mapping sets the PAT bit (the VMM
 * audit refuses it outside this window). PAT WC combined with any MTRR type
 * gives WC or UC, never a cached type, so the firmware's MTRRs (read here,
 * never written) cannot make device memory cacheable. Without a PAT a WC
 * request is mapped UC.
 */
#ifndef VOS_X86_64_DEVMAP_H
#define VOS_X86_64_DEVMAP_H

#include <stdint.h>

#include "bootinfo.h"

#define DEVMAP_PDPT_INDEX   509
#define DEVMAP_VA_BASE      0xFFFFFFFF40000000ULL                   /* PML4 511, PDPT 509 */
#define DEVMAP_VA_END       (DEVMAP_VA_BASE + 0x40000000ULL)
#define DEVMAP_MAX          8
#define DEVMAP_MAX_BYTES    (64ULL << 20)

#define DEVMAP_UC           0
#define DEVMAP_WC           1

#define DEVMAP_OK           0
#define DEVMAP_EINVAL       1   /* zero length, bad attribute, NULL output */
#define DEVMAP_EOVERFLOW    2   /* pa + len or its page rounding wraps */
#define DEVMAP_ERANGE       3   /* beyond the CPU's physical address width */
#define DEVMAP_ERAM         4   /* intersects RAM (available, ACPI, bad or unknown map entry) */
#define DEVMAP_EOVERLAP     5   /* overlaps an existing device mapping */
#define DEVMAP_EFULL        6   /* too many mappings, too many bytes */
#define DEVMAP_ENOMEM       7   /* no frame for a page table */
#define DEVMAP_ESTATE       8   /* not initialized, or the PAT/window setup failed */

/* Set up the window (after kmap_init) and the PAT. DEVMAP_OK or DEVMAP_ESTATE. */
int devmap_init(const struct boot_info *bi);
/* Validate a request without mapping anything. */
int devmap_check(uint64_t pa, uint64_t len);
/*
 * Map [pa, pa + len) for the kernel with memory type attr (DEVMAP_UC or
 * DEVMAP_WC); *va receives the address of pa itself. *used (if not NULL)
 * receives the type actually used (WC degrades to UC without a PAT).
 */
int devmap_map(uint64_t pa, uint64_t len, int attr, const char *what, void **va, int *used);
const char *devmap_strerror(int err);
/* Physical address bits the CPU supports (36 if it does not say). */
unsigned devmap_maxphys(void);
/* Nonzero if WC mappings are real write-combining (PAT programmed). */
int devmap_wc_available(void);
/* The MTRR memory type of physical address pa (0 UC, 1 WC, 4 WT, 5 WP, 6 WB; -1 if MTRRs are off or absent). */
int devmap_mtrr_type(uint64_t pa);
unsigned devmap_count(void);
/* The PMM frames the window's page tables use (directory first); returns how many were stored. */
#define DEVMAP_MAX_TABLES   (1 + DEVMAP_MAX_BYTES / 0x200000 + DEVMAP_MAX)
unsigned devmap_table_frames(uint64_t *pa, unsigned max);

/* VMM audit support: the expected leaf entry for a window address (0 if nothing is mapped there). */
uint64_t devmap_expected_pte(uint64_t va);
/* Consistency of the registry and the window's page tables; NULL if consistent. */
const char *devmap_audit(void);

/* USB.2-B non-access fixture: a qualified 16 KiB hole at 4 GiB, never
 * dereferenced. This is NOT a PCI BAR/aperture or DMA qualification API.
 * Only this region identifier is accepted; no arbitrary physical input.
 * One exclusive tail lease; kernel CR3, IF=0, non-IRQ thread context.
 * All permanent mappings stay unchanged. Errors retain the owning token.
 * A release succeeds once; stale tokens never authorize another generation.
 */
#define DEVMAP_SCOPE_FIXTURE 1u
int devmap_scope_acquire(unsigned region, uint64_t offset, uint64_t length,
                         int attr, uint64_t *token, uint64_t *va);
int devmap_scope_release(uint64_t token);
uint64_t devmap_scope_token(void);
unsigned devmap_scope_tables(void);

/* Fault budgets apply only to the explicitly invoked fixture acquisition:
 * -1 unlimited; 0 fail before the next allocation. No legacy-map effects. */
void devmap_scope_budget(int allocations);

#ifdef VOS_TEST
/* Remove every mapping made after the first `keep` (self-tests only). */
void devmap_test_rollback(unsigned keep);
/* Validate against another memory map (NULL restores the boot map). */
void devmap_test_set_map(const struct mb2_tag_mmap *m, uint32_t entries);
/* The raw leaf entry at a window address (0 if none). */
uint64_t devmap_test_pte(uint64_t va);
/* Overwrite the leaf entry at a window address (audit tests; restore it afterwards). */
void devmap_test_set_pte(uint64_t va, uint64_t pte);
#endif

#endif
