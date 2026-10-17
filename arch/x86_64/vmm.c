#include "vmm.h"
#include "cpu.h"
#include "gdt.h"
#include "kmap.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"

extern char __kernel_phys_start[], __kernel_phys_end[];

#define LEVELS          4
#define IDX(va, level)  (((va) >> (12 + 9 * (level))) & 511)  /* level 3 = PML4, 0 = PT */
#define NONLEAF_BITS    (PTE_P | PTE_W | PTE_U | PTE_A | PTE_ADDR)
#define LEAF_BITS       (PTE_P | PTE_W | PTE_U | PTE_A | PTE_D | PTE_NX | PTE_ADDR)
#define ALIAS_BITS      (PTE_P | PTE_W | PTE_A | PTE_D | PTE_PS | PTE_NX | PTE_ADDR)
#define LARGE_2M        0x200000ULL
#define CR4_PGE         (1ULL << 7)

static uint64_t kernel_root;
static const char *diag = "";

static int canonical(uint64_t va)
{
    return va < LOWER_HALF_END || va >= UPPER_HALF_START;
}

static int in_kernel_image(uint64_t pa)
{
    return pa >= (uint64_t)__kernel_phys_start && pa < (uint64_t)__kernel_phys_end;
}

static int pmm_owned(uint64_t pa)
{
    return !in_kernel_image(pa) && pmm_frame_state_in(pmm_kernel(), pa) == PMM_FRAME_ALLOCATED;
}

/*
 * The only way this file dereferences a table: the frame must be aligned,
 * below 1 GiB (reachable through the alias), and either a boot table inside
 * the kernel image or a PMM-allocated frame. Anything else is never touched.
 */
static uint64_t *table(uint64_t pa)
{
    if (pa % PAGE_SIZE != 0 || pa >= BOOT_MAP_LIMIT)
        return NULL;
    if (!in_kernel_image(pa) && !pmm_owned(pa))
        return NULL;
    return phys_to_virt(pa);
}

static int active(uint64_t root)
{
    return (read_cr3() & PTE_ADDR) == root;
}

/* A present non-leaf entry at `level` (3..1) that may be followed. */
static int check_nonleaf(uint64_t e, int level)
{
    if (e & PTE_PS)
        return level == 3 ? VMM_ECORRUPT : VMM_ELARGE;  /* PS is reserved in a PML4E */
    if (e & ~NONLEAF_BITS)
        return VMM_ECORRUPT;
    return VMM_OK;
}

/* Validation shared by map and unmap. */
static int check_va(uint64_t va)
{
    if (!canonical(va))
        return VMM_ENONCANON;
    if (va >= UPPER_HALF_START)
        return VMM_EKERNEL;
    if (va % PAGE_SIZE != 0)
        return VMM_EALIGN;
    if (va < NULL_GUARD_END || va >= LOWER_MAP_END)
        return VMM_ERANGE;
    return VMM_OK;
}

#ifdef VOS_TEST
static int table_budget = -1;
static uint64_t removed_identity_pdpt;

uint64_t vmm_test_identity_pdpt(void)
{
    return removed_identity_pdpt;
}

void vmm_test_table_budget(int n)
{
    table_budget = n;
}
#endif

static int table_alloc(uint64_t *pa)
{
#ifdef VOS_TEST
    if (table_budget == 0) {
        *pa = 0;
        return VMM_ENOMEM;
    }
    if (table_budget > 0)
        table_budget--;
#endif
    return pmm_alloc_frame(pa) == PMM_OK ? VMM_OK : VMM_ENOMEM;     /* zeroed */
}

static void table_free(uint64_t pa)
{
    if (pmm_free_frame(pa) != PMM_OK)
        panic("vmm: page-table frame release failed");
}

int vmm_map(uint64_t root, uint64_t va, uint64_t pa, unsigned flags)
{
    uint64_t *t;
    int err, level;

    if ((err = check_va(va)) != VMM_OK)
        return err;
    if (pa % PAGE_SIZE != 0)
        return VMM_EALIGN;
    if (pa < PMM_LOW_LIMIT || pa >= PMM_LIMIT || !pmm_owned(pa))
        return VMM_EPHYS;
    if ((flags & ~VMM_FLAGS_ALL) || ((flags & VMM_WRITE) && (flags & VMM_EXEC)) ||
        ((flags & VMM_EXEC) && !(flags & VMM_USER)))
        return VMM_EFLAGS;
    if (!(t = table(root)))
        return VMM_EROOT;

    /* Pass 1, read-only: find the first missing level and refuse every conflict. */
    for (level = 3; level > 0; level--) {
        uint64_t e = t[IDX(va, level)];

        if (!(e & PTE_P))
            break;
        if ((err = check_nonleaf(e, level)) != VMM_OK)
            return err;
        if (!(t = table(e & PTE_ADDR)))
            return VMM_ECORRUPT;
    }
    if (level == 0 && (t[IDX(va, 0)] & PTE_P))
        return VMM_EEXIST;

    /* Pass 2: create the missing tables; on failure undo every one of them. */
    uint64_t new_tables[3], *slots[3];
    int n = 0;

    for (; level > 0; level--) {
        uint64_t f;

        if (table_alloc(&f) != VMM_OK) {
            for (int i = n - 1; i >= 0; i--)
                *slots[i] = 0;
            if (active(root))
                invlpg(va);
            for (int i = n - 1; i >= 0; i--)
                table_free(new_tables[i]);
            return VMM_ENOMEM;
        }
        slots[n] = &t[IDX(va, level)];
        new_tables[n++] = f;
        *slots[n - 1] = f | PTE_P | PTE_W | PTE_U;       /* lower half only */
        t = phys_to_virt(f);
    }

    t[IDX(va, 0)] = pa | PTE_P | ((flags & VMM_WRITE) ? PTE_W : 0) |
                    ((flags & VMM_USER) ? PTE_U : 0) | ((flags & VMM_EXEC) ? 0 : PTE_NX);
    if (active(root))
        invlpg(va);
    return VMM_OK;
}

static int all_zero(const uint64_t *t)
{
    for (int i = 0; i < 512; i++)
        if (t[i])
            return 0;
    return 1;
}

int vmm_unmap(uint64_t root, uint64_t va, uint64_t *pa)
{
    uint64_t *tables[LEVELS], table_pa[LEVELS];
    int err;

    if (pa)
        *pa = 0;
    if ((err = check_va(va)) != VMM_OK)
        return err;
    if (!(tables[3] = table(root)))
        return VMM_EROOT;
    table_pa[3] = root;

    for (int level = 3; level > 0; level--) {
        uint64_t e = tables[level][IDX(va, level)];

        if (!(e & PTE_P))
            return VMM_ENOTMAPPED;
        if ((err = check_nonleaf(e, level)) != VMM_OK)
            return err;
        if (!(tables[level - 1] = table(e & PTE_ADDR)))
            return VMM_ECORRUPT;
        table_pa[level - 1] = e & PTE_ADDR;
    }
    uint64_t leaf = tables[0][IDX(va, 0)];
    if (!(leaf & PTE_P))
        return VMM_ENOTMAPPED;
    if (leaf & ~LEAF_BITS)
        return VMM_ECORRUPT;

    /*
     * Clear the leaf, then detach tables that became empty (PT, PD, PDPT; never
     * the root, never a boot table). Flush before the frames go back to the
     * allocator: INVLPG also drops paging-structure cache entries.
     */
    uint64_t release[3];
    int n = 0;

    tables[0][IDX(va, 0)] = 0;
    for (int level = 0; level < 3; level++) {
        if (!all_zero(tables[level]) || !pmm_owned(table_pa[level]))
            break;
        tables[level + 1][IDX(va, level + 1)] = 0;
        release[n++] = table_pa[level];
    }
    if (active(root))
        invlpg(va);
    for (int i = 0; i < n; i++)
        table_free(release[i]);
    if (pa)
        *pa = leaf & PTE_ADDR;
    return VMM_OK;
}

int vmm_lookup(uint64_t root, uint64_t va, struct vmm_mapping *m)
{
    unsigned w = 1, u = 1, nx = 0;
    uint64_t *t;

    memset(m, 0, sizeof(*m));
    if (!canonical(va))
        return VMM_ENONCANON;
    if (!(t = table(root)))
        return VMM_EROOT;

    for (int level = 3; level >= 0; level--) {
        uint64_t e = t[IDX(va, level)];

        if (!(e & PTE_P))
            return VMM_ENOTMAPPED;
        w &= (e & PTE_W) != 0;
        u &= (e & PTE_U) != 0;
        nx |= (e & PTE_NX) != 0;
        if (level == 0 || (e & PTE_PS)) {
            if (level == 3)
                return VMM_ECORRUPT;
            uint64_t size = 1ULL << (12 + 9 * level);
            m->pa = (e & PTE_ADDR & ~(size - 1)) + (va & (size - 1));
            m->page_size = size;
            m->flags = (w ? VMM_WRITE : 0) | (nx ? 0 : VMM_EXEC) | (u ? VMM_USER : 0);
            return VMM_OK;
        }
        if (!(t = table(e & PTE_ADDR)))
            return VMM_ECORRUPT;
    }
    return VMM_ECORRUPT;                /* not reached */
}

/* ---- audit ---- */

static void bad(struct vmm_audit *a, const char *why, uint64_t va)
{
    if (a->errors++ == 0) {
        a->first_error = why;
        a->first_error_va = va;
    }
}

static uint64_t sign_extend(uint64_t va)
{
    return (va & (1ULL << 47)) ? (va | 0xFFFF000000000000ULL) : va;
}

/* Checks for a present non-leaf entry; returns the next table or NULL. */
static uint64_t *audit_nonleaf(struct vmm_audit *a, uint64_t e, int upper, uint64_t va)
{
    uint64_t *next;

    if (e & ~NONLEAF_BITS) {
        bad(a, "non-leaf entry with unsupported bits", va);
        return NULL;
    }
    if (upper && (e & PTE_U))
        bad(a, "user bit on an upper-half table entry", va);
    if (!(next = table(e & PTE_ADDR))) {
        bad(a, "entry points outside valid table frames", va);
        return NULL;
    }
    if (upper)
        a->tables_upper++;
    else
        a->tables_lower++;
    return next;
}

/* Nonzero if the frames [pa, pa + size) include a kernel stack guard page. */
static int covers_guard(uint64_t pa, uint64_t size)
{
    uint64_t g[KMAP_MAX_GUARDS];
    unsigned n = kmap_guard_pages(g, KMAP_MAX_GUARDS);

    for (unsigned i = 0; i < n; i++)
        if (g[i] - KERNEL_VMA >= pa && g[i] - KERNEL_VMA - pa < size)
            return 1;
    return 0;
}

static int covers_text(uint64_t pa, uint64_t size)
{
    for (uint64_t p = pa; p - pa < size; p += PAGE_SIZE)
        if (kmap_is_text(p))
            return 1;
    return 0;
}

/*
 * Rules for every present leaf in any address space, on the effective
 * permissions (W and U ANDed over the levels, NX ORed): never writable and
 * executable; executable in ring 0 only on kernel text frames (user pages
 * excepted: with SMEP off the CPU also runs them in ring 0); kernel text
 * never writable through any mapping; guard pages never mapped.
 */
static void audit_leaf(struct vmm_audit *a, uint64_t eff, uint64_t pa, uint64_t size, uint64_t va)
{
    int w = (eff & PTE_W) != 0, x = !(eff & PTE_NX), u = (eff & PTE_U) != 0;

    if (w && x)
        bad(a, "writable and executable page", va);
    else if (x && !u && (size != PAGE_SIZE || !kmap_is_text(pa)))
        bad(a, "supervisor-executable page outside the kernel text", va);
    else if (w && covers_text(pa, size))
        bad(a, "writable mapping of kernel text", va);
    else if (covers_guard(pa, size))
        bad(a, "stack guard page mapped", va);
    if (x && !u)
        a->exec_supervisor++;
}

int vmm_audit_space(uint64_t root, int kernel_half, struct vmm_audit *a)
{
    uint64_t *l4;

    memset(a, 0, sizeof(*a));
    if (!(l4 = table(root))) {
        bad(a, "root is not a valid table frame", 0);
        return VMM_EROOT;
    }
    for (uint64_t i4 = 0; i4 < 512; i4++) {
        uint64_t e4 = l4[i4], va4 = sign_extend(i4 << 39);
        int upper = i4 >= 256;
        uint64_t *l3;

        if (!(e4 & PTE_P)) {
            if (e4)
                bad(a, "non-present entry not cleared", va4);
            continue;
        }
        if (kernel_half && upper && i4 != 511)
            bad(a, "unexpected upper-half PML4 entry", va4);
        if (e4 & PTE_PS) {
            bad(a, "PS set in a PML4 entry", va4);
            continue;
        }
        if (!(l3 = audit_nonleaf(a, e4, upper, va4)))
            continue;
        for (uint64_t i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = l3[i3], va3 = va4 + (i3 << 30);
            int kmap_pd = kernel_half && i4 == 511 && i3 == 510;    /* the W^X kernel map */
            uint64_t *l2;

            if (!(e3 & PTE_P)) {
                if (e3)
                    bad(a, "non-present entry not cleared", va3);
                continue;
            }
            if (kernel_half && upper && i3 != 510)
                bad(a, "unexpected kernel PDPT entry", va3);
            if (e3 & PTE_PS) {
                bad(a, "1 GiB page", va3);
                continue;
            }
            if (!(l2 = audit_nonleaf(a, e3, upper, va3)))
                continue;
            for (uint64_t i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = l2[i2], va2 = va3 + (i2 << 21);
                uint64_t *l1;

                if (!(e2 & PTE_P)) {
                    if (e2)
                        bad(a, "non-present entry not cleared", va2);
                    continue;
                }
                if (e2 & PTE_PS) {
                    /* Only the kernel alias may use 2 MiB pages, supervisor only. */
                    if (!upper)
                        bad(a, "2 MiB page in the lower half", va2);
                    else if ((e2 & ~ALIAS_BITS) || (e2 & PTE_ADDR & (LARGE_2M - 1)))
                        bad(a, "kernel alias page with unsupported bits", va2);
                    else if (kmap_pd && (e2 & ~(PTE_A | PTE_D)) != kmap_expected_pde(i2))
                        bad(a, "kernel alias page differs from the W^X layout", va2);
                    audit_leaf(a, (e4 & e3 & e2 & (PTE_W | PTE_U)) | ((e4 | e3 | e2) & PTE_NX),
                               e2 & PTE_ADDR & ~(LARGE_2M - 1), LARGE_2M, va2);
                    a->large_upper++;
                    continue;
                }
                if (kmap_pd && i2 != 0)
                    bad(a, "4 KiB table under the 2 MiB kernel alias", va2);
                if (!(l1 = audit_nonleaf(a, e2, upper, va2)))
                    continue;
                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = l1[i1], va1 = va2 + (i1 << 12);
                    uint64_t eff = (e4 & e3 & e2 & e1 & (PTE_W | PTE_U)) | ((e4 | e3 | e2 | e1) & PTE_NX);
                    int kmap_pt = kmap_pd && i2 == 0;

                    if (!(e1 & PTE_P)) {
                        if (e1)
                            bad(a, "non-present entry not cleared", va1);
                        else if (kmap_pt && kmap_expected_pte(i1 << 12))
                            bad(a, "kernel page missing from the W^X layout", va1);
                        continue;
                    }
                    if (e1 & ~LEAF_BITS)
                        bad(a, "leaf entry with unsupported bits", va1);
                    audit_leaf(a, eff, e1 & PTE_ADDR, PAGE_SIZE, va1);
                    if (upper) {
                        if (e1 & PTE_U)
                            bad(a, "user-accessible upper-half page", va1);
                        if (kmap_pt && (e1 & ~(PTE_A | PTE_D)) != kmap_expected_pte(i1 << 12))
                            bad(a, "kernel page differs from the W^X layout", va1);
                        a->leaves_upper++;
                        continue;
                    }
                    a->leaves_lower++;
                    if ((e1 & PTE_ADDR) < PMM_LOW_LIMIT || (e1 & PTE_ADDR) >= PMM_LIMIT)
                        bad(a, "leaf maps a frame outside [1 MiB, 1 GiB)", va1);
                    else if (in_kernel_image(e1 & PTE_ADDR))
                        bad(a, "lower-half leaf maps a kernel image frame", va1);
                    if (eff & PTE_U)
                        a->user_leaves++;
                }
            }
        }
    }
    if (kernel_half) {
        uint64_t small = 0;

        for (uint64_t pa = 0; pa < KMAP_SMALL_SPAN; pa += PAGE_SIZE)
            small += kmap_expected_pte(pa) != 0;
        if (!(l4[511] & PTE_P) || a->large_upper != KMAP_LARGE_PAGES || a->leaves_upper != small)
            bad(a, "kernel alias incomplete", KERNEL_VMA);
    }
    return a->errors ? VMM_ECORRUPT : VMM_OK;
}

/* ---- kernel address space ---- */

int vmm_init(void)
{
    struct vmm_mapping m;
    uint64_t cr3 = read_cr3();

    if (!(rdmsr(MSR_EFER) & EFER_NXE) || !(read_cr0() & CR0_WP)) {
        diag = "EFER.NXE or CR0.WP is not enabled";
        return VMM_ESTATE;
    }
    if ((cr3 & ~PTE_ADDR) || !in_kernel_image(cr3)) {
        diag = "CR3 is not the boot PML4 inside the kernel image";
        return VMM_ESTATE;
    }
    kernel_root = cr3;
    if (vmm_lookup(kernel_root, KERNEL_VMA, &m) != VMM_OK || m.pa != 0 ||
        m.page_size != LARGE_2M || (m.flags & VMM_USER) ||
        vmm_lookup(kernel_root, KERNEL_ALIAS_END - 1, &m) != VMM_OK ||
        m.pa != BOOT_MAP_LIMIT - 1 || (m.flags & VMM_USER)) {
        kernel_root = 0;
        diag = "kernel alias of phys [0, 1 GiB) is not intact";
        return VMM_ESTATE;
    }
    return VMM_OK;
}

struct dtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

int vmm_remove_identity(void)
{
    struct vmm_mapping m;
    struct dtr gdtr, idtr;
    uint64_t rip, rsp;
    uint16_t tr;

    __asm__ volatile ("lea 0(%%rip), %0" : "=r"(rip));
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    __asm__ volatile ("sidt %0" : "=m"(idtr));
    __asm__ volatile ("str %0" : "=r"(tr));

    diag = "";
    if (!kernel_root || (read_cr3() & PTE_ADDR) != kernel_root)
        diag = "not running on the kernel address space";
    else if (rip < KERNEL_VMA)
        diag = "still executing below the higher half";
    else if (rsp < KERNEL_VMA)
        diag = "stack below the higher half";
    else if (gdtr.base < KERNEL_VMA || idtr.base < KERNEL_VMA)
        diag = "GDT or IDT below the higher half";
    else if (tr != GDT_TSS || gdt_tss_base() < KERNEL_VMA || gdt_tss_rsp0() < KERNEL_VMA ||
             gdt_tss_ist1() < KERNEL_VMA)
        diag = "TSS or its stacks below the higher half";
    if (*diag)
        return VMM_ESTATE;

    uint64_t *l4 = phys_to_virt(kernel_root);
    for (int i = 1; i < 256; i++) {
        if (l4[i]) {
            diag = "unexpected lower-half PML4 entry";
            return VMM_ESTATE;
        }
    }
    if (!(l4[0] & PTE_P) || (l4[0] & PTE_PS)) {
        diag = "identity map not present as expected";
        return VMM_ESTATE;
    }

#ifdef VOS_TEST
    removed_identity_pdpt = l4[0] & PTE_ADDR;
#endif
    l4[0] = 0;
    uint64_t cr4 = read_cr4();
    if (cr4 & CR4_PGE) {                /* global entries survive a CR3 reload */
        write_cr4(cr4 & ~CR4_PGE);
        write_cr4(cr4);
    }
    write_cr3(read_cr3());              /* drop every cached translation */

    if (vmm_lookup(kernel_root, PMM_LOW_LIMIT, &m) != VMM_ENOTMAPPED ||
        vmm_lookup(kernel_root, 0, &m) != VMM_ENOTMAPPED ||
        vmm_lookup(kernel_root, BOOT_MAP_LIMIT - 1, &m) != VMM_ENOTMAPPED ||
        vmm_lookup(kernel_root, KERNEL_VMA + PMM_LOW_LIMIT, &m) != VMM_OK ||
        m.pa != PMM_LOW_LIMIT) {
        diag = "page tables do not match the expected state after removal";
        return VMM_ESTATE;
    }
    return VMM_OK;
}

uint64_t vmm_kernel_root(void)
{
    return kernel_root;
}

const char *vmm_diagnostic(void)
{
    return diag;
}

const char *vmm_strerror(int err)
{
    static const char *const names[] = {
        "ok", "unaligned address", "non-canonical address", "address in a guard range",
        "kernel address range", "physical frame not allocated from [1 MiB, 1 GiB)",
        "invalid flags", "already mapped", "not mapped", "large page in the way",
        "out of page-table frames", "malformed page table", "invalid root", "precondition failed",
    };

    return err >= 0 && err < (int)(sizeof(names) / sizeof(names[0])) ? names[err] : "?";
}
