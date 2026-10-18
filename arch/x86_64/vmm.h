/*
 * Virtual memory manager: 4-level x86_64 page tables, 4 KiB mappings.
 *
 * Address spaces are identified by the physical address of their PML4
 * ("root"). Every page-table frame is reached through the kernel alias of
 * physical [0, 1 GiB), so table frames must lie below 1 GiB.
 *
 * Mapping rules enforced by vmm_map:
 *   - va must be canonical, 4 KiB aligned, in the lower half, outside the
 *     null guard [0, 4 MiB) and below the highest lower-half page. The upper
 *     half (kernel alias and everything else) is never modified.
 *   - pa must be 4 KiB aligned and a frame currently allocated from the
 *     physical allocator, so inside [1 MiB, 1 GiB) and never a frame of the
 *     kernel image, a module or the boot information.
 *   - flags: VMM_WRITE, VMM_EXEC, VMM_USER; writable + executable is refused,
 *     and so is VMM_EXEC without VMM_USER: only the kernel text (kmap.c) is
 *     executable in ring 0. A page without VMM_EXEC gets NX.
 *   - an existing mapping, or a large page in the way, is never overwritten.
 * Intermediate entries created here are present + writable (+ user in the
 * lower half); the leaf alone decides the effective permissions.
 *
 * Ownership: vmm_unmap never frees the mapped data frame; it returns it to the
 * caller. It frees an intermediate table only when the table became empty,
 * lies in the lower half, and its frame is PMM-allocated (boot-time tables are
 * inside the kernel image, so they are never freed).
 */
#ifndef VOS_X86_64_VMM_H
#define VOS_X86_64_VMM_H

#include <stdint.h>

/* Request flags (also used for effective permissions). */
#define VMM_WRITE       0x1u
#define VMM_EXEC        0x2u
#define VMM_USER        0x4u
#define VMM_FLAGS_ALL   (VMM_WRITE | VMM_EXEC | VMM_USER)

/* Hardware entry bits. */
#define PTE_P           (1ULL << 0)
#define PTE_W           (1ULL << 1)
#define PTE_U           (1ULL << 2)
#define PTE_PWT         (1ULL << 3)
#define PTE_PCD         (1ULL << 4)
#define PTE_A           (1ULL << 5)
#define PTE_D           (1ULL << 6)
#define PTE_PS          (1ULL << 7)
#define PTE_G           (1ULL << 8)
#define PTE_PAT4K       (1ULL << 7)     /* PAT bit of a 4 KiB leaf (PS position); U1: device window only */
#define PTE_NX          (1ULL << 63)
#define PTE_ADDR        0x000FFFFFFFFFF000ULL

/* Results. */
#define VMM_OK          0
#define VMM_EALIGN      1   /* va or pa not 4 KiB aligned */
#define VMM_ENONCANON   2   /* va not canonical */
#define VMM_ERANGE      3   /* va in the null guard or the top lower-half page */
#define VMM_EKERNEL     4   /* va in the upper half: never changed by vmm_map/unmap */
#define VMM_EPHYS       5   /* pa not a PMM-allocated frame in [1 MiB, 1 GiB) */
#define VMM_EFLAGS      6   /* unknown flag, writable + executable, or supervisor executable */
#define VMM_EEXIST      7   /* va already mapped */
#define VMM_ENOTMAPPED  8   /* va not mapped */
#define VMM_ELARGE      9   /* a 1 GiB / 2 MiB page covers va */
#define VMM_ENOMEM      10  /* no frame for a page table (nothing changed) */
#define VMM_ECORRUPT    11  /* malformed table entry; not followed */
#define VMM_EROOT       12  /* root is not a valid PML4 frame */
#define VMM_ESTATE      13  /* precondition failed (see diagnostic) */

struct vmm_mapping {
    uint64_t pa;                /* physical address of va (offset included) */
    uint64_t page_size;         /* 4 KiB, 2 MiB or 1 GiB */
    unsigned flags;             /* effective VMM_WRITE / VMM_EXEC / VMM_USER */
};

struct vmm_audit {
    uint64_t tables_lower;      /* page tables reachable in the lower half */
    uint64_t tables_upper;
    uint64_t leaves_lower;      /* 4 KiB mappings in the lower half */
    uint64_t user_leaves;       /* effective user-accessible leaves */
    uint64_t large_upper;       /* 2 MiB kernel alias pages */
    uint64_t leaves_upper;      /* 4 KiB upper-half mappings (the kernel map's page table) */
    uint64_t exec_supervisor;   /* leaves executable in ring 0 that are not user pages */
    uint64_t device_leaves;     /* U1: 4 KiB leaves in the device window (devmap.h) */
    uint64_t errors;
    const char *first_error;
    uint64_t first_error_va;
};

/* Adopt the boot PML4 (CR3) as the kernel address space and verify its shape. */
int vmm_init(void);
uint64_t vmm_kernel_root(void);
/*
 * Remove the boot identity map (PML4[0]) after verifying that RIP, RSP, the
 * GDT, IDT and TSS (and its RSP0/IST1 stacks) all use higher-half addresses
 * and that no other lower-half entry exists; then reload CR3 and confirm the
 * low half no longer translates while the kernel alias still does. The boot
 * tables themselves stay in place (they are part of the kernel image).
 */
int vmm_remove_identity(void);

int vmm_map(uint64_t root, uint64_t va, uint64_t pa, unsigned flags);
int vmm_unmap(uint64_t root, uint64_t va, uint64_t *pa);
int vmm_lookup(uint64_t root, uint64_t va, struct vmm_mapping *m);

/*
 * Walk the whole hierarchy under `root` without following any malformed
 * entry. In every address space no leaf may be writable and executable
 * (effective permissions), no non-user leaf may be executable unless it maps
 * kernel text, no mapping of kernel text may be writable, no leaf may cover
 * a stack guard page, and lower-half leaves must map frames in [1 MiB,
 * 1 GiB) outside the kernel image. With kernel_half set, the upper half must
 * be exactly the W^X kernel map (kmap.h): PML4[511] -> PDPT[510] -> PD[0] a
 * page table matching kmap_expected_pte, PD[1..511] matching
 * kmap_expected_pde, nothing else. Returns VMM_OK if no error was found.
 */
int vmm_audit_space(uint64_t root, int kernel_half, struct vmm_audit *a);

const char *vmm_strerror(int err);
const char *vmm_diagnostic(void);       /* detail for the last VMM_ESTATE */

#ifdef VOS_TEST
/* Test hook: allow n more page-table allocations, then fail (-1 = unlimited). */
void vmm_test_table_budget(int n);
/* Test hook: the boot identity PDPT that PML4[0] pointed to before removal. */
uint64_t vmm_test_identity_pdpt(void);
#endif

#endif
