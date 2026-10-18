/*
 * Virtual memory self-tests (VOS_TEST only).
 *
 * selftest_vmm runs in every test image after the identity map is gone and
 * restores everything it creates. Expectations are fixed in the test
 * (frame counts, effective permissions) and the results are checked both
 * through vmm_lookup/vmm_audit and through real CPU memory accesses.
 *
 * The selftest_vmm_fault_* functions are isolated test cases: each ends in a
 * CPU page fault, which the kernel turns into a panic (exit status 35).
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "kmap.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "vmm.h"

#define VT      0x0000200000000000ULL   /* test window: PML4 entry 64, unused otherwise */
#define VT2     (VT + PAGE_SIZE)        /* same page table as VT */
#define VT3     (VT + 0x200000)         /* next page table */

static int failures, cases;

static void expect(int ok, const char *what)
{
    cases++;
    if (ok)
        return;
    failures++;
    serial_puts("selftest: FAIL: ");
    serial_puts(what);
    serial_puts("\r\n");
}

static uint64_t alloc(void)
{
    uint64_t f;

    if (pmm_alloc_frame(&f) != PMM_OK)
        panic("selftest: out of frames");
    return f;
}

static void release(uint64_t f)
{
    if (pmm_free_frame(f) != PMM_OK)
        panic("selftest: frame release failed");
}

static uint64_t pattern(uint64_t frame, int i)
{
    return frame ^ ((uint64_t)i * 0x9E3779B97F4A7C15ULL);
}

static void fill(uint64_t frame)
{
    uint64_t *p = phys_to_virt(frame);

    for (int i = 0; i < 512; i++)
        p[i] = pattern(frame, i);
}

/* Read every word of the page at va through the CPU and compare with frame's pattern. */
static int reads_as(uint64_t va, uint64_t frame)
{
    const volatile uint64_t *p = (const volatile uint64_t *)va;

    for (int i = 0; i < 512; i++)
        if (p[i] != pattern(frame, i))
            return 0;
    return 1;
}

static int audit_ok(uint64_t tables, uint64_t leaves, uint64_t user)
{
    struct vmm_audit a;

    return vmm_audit_space(vmm_kernel_root(), 1, &a) == VMM_OK && a.tables_lower == tables &&
           a.leaves_lower == leaves && a.user_leaves == user && a.large_upper == KMAP_LARGE_PAGES;
}

static int mapped_to(uint64_t va, uint64_t pa, unsigned flags)
{
    struct vmm_mapping m;

    return vmm_lookup(vmm_kernel_root(), va, &m) == VMM_OK && m.pa == pa &&
           m.page_size == PAGE_SIZE && m.flags == flags;
}

struct dtr { uint16_t limit; uint64_t base; } __attribute__((packed));

static void test_layout_and_survival(void)
{
    const uint64_t K = vmm_kernel_root();
    const uint64_t *l4 = phys_to_virt(K);
    struct vmm_mapping m;

    expect(K != 0 && (read_cr3() & PTE_ADDR) == K, "kernel space is active");
    expect(audit_ok(0, 0, 0), "audit: upper half is exactly the supervisor alias, lower half empty");
    expect(l4[0] == 0 && vmm_lookup(K, PMM_LOW_LIMIT, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(K, 0, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(K, BOOT_MAP_LIMIT - PAGE_SIZE, &m) == VMM_ENOTMAPPED,
           "identity map absent (PML4[0] clear, low addresses do not translate)");
    uint64_t idp = vmm_test_identity_pdpt();
    expect(idp != 0 && pmm_frame_state_in(pmm_kernel(), idp) == PMM_FRAME_UNAVAILABLE &&
           ((const uint64_t *)phys_to_virt(idp))[0] != 0,
           "boot identity PDPT kept intact, never PMM-owned");

    expect(vmm_lookup(K, KERNEL_VMA + PMM_LOW_LIMIT + 0x123, &m) == VMM_OK &&
           m.pa == PMM_LOW_LIMIT + 0x123 && m.page_size == PAGE_SIZE && m.flags == 0,
           "alias of the boot code page: 4 KiB, supervisor, read-only, NX");
    expect(vmm_lookup(K, KERNEL_VMA + KMAP_SMALL_SPAN - PAGE_SIZE + 0x123, &m) == VMM_OK &&
           m.pa == KMAP_SMALL_SPAN - PAGE_SIZE + 0x123 && m.page_size == PAGE_SIZE && m.flags == VMM_WRITE,
           "alias just below 4 MiB: 4 KiB page, supervisor, writable, NX");
    expect(vmm_lookup(K, KERNEL_VMA + KMAP_SMALL_SPAN + 0x123, &m) == VMM_OK && m.pa == KMAP_SMALL_SPAN + 0x123 &&
           m.page_size == 0x200000 && m.flags == VMM_WRITE &&
           vmm_lookup(K, KERNEL_ALIAS_END - 1, &m) == VMM_OK && m.pa == BOOT_MAP_LIMIT - 1 &&
           m.page_size == 0x200000 && m.flags == VMM_WRITE,
           "alias above 4 MiB: 2 MiB pages, supervisor, writable, NX");
    expect(vmm_lookup(K, KERNEL_ALIAS_END, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(K, UPPER_HALF_START, &m) == VMM_ENOTMAPPED &&
           vmm_lookup(K, 0x0000800000000000ULL, &m) == VMM_ENONCANON,
           "nothing else is mapped; non-canonical lookups refused");

    /* Kernel survival, exercised with real accesses where possible. */
    struct dtr gdtr, idtr;
    uint16_t tr;
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    __asm__ volatile ("sidt %0" : "=m"(idtr));
    __asm__ volatile ("str %0" : "=r"(tr));
    expect(gdtr.base >= KERNEL_VMA && idtr.base >= KERNEL_VMA && tr == GDT_TSS &&
           gdt_tss_rsp0() >= KERNEL_VMA && gdt_tss_ist1() >= KERNEL_VMA &&
           gdt_in_ist1_stack(gdt_tss_ist1() - 8), "GDT, IDT, TR and TSS stacks are higher-half");
    expect((read_cr0() & CR0_WP) && (rdmsr(MSR_EFER) & EFER_NXE), "CR0.WP and EFER.NXE still set");
    volatile uint16_t *vga = phys_to_virt(VGA_TEXT_PHYS);
    uint16_t old = vga[80 * 25 - 1];
    vga[80 * 25 - 1] = 0x0756;
    expect(vga[80 * 25 - 1] == 0x0756, "VGA text memory writable through the alias");
    vga[80 * 25 - 1] = old;
}

static void test_map_unmap(void)
{
    const uint64_t K = vmm_kernel_root();
    const uint64_t *l4 = phys_to_virt(K);
    uint64_t f0 = pmm_free_count(), pa;
    uint64_t d1 = alloc(), d2 = alloc(), d3 = alloc();

    fill(d1);
    fill(d2);
    fill(d3);

    /* A. Mapping */
    expect(vmm_map(K, VT, d1, VMM_WRITE) == VMM_OK, "map an unused page");
    expect(pmm_free_count() == f0 - 3 - 3, "three page tables allocated (PDPT, PD, PT)");
    expect(mapped_to(VT, d1, VMM_WRITE), "lookup: expected frame, writable, NX, supervisor");
    expect(reads_as(VT, d1), "CPU reads the frame through the new mapping");
    *(volatile uint64_t *)(VT + 8) = 0x1122334455667788ULL;
    expect(((uint64_t *)phys_to_virt(d1))[1] == 0x1122334455667788ULL,
           "CPU write through the mapping reaches the frame");
    ((uint64_t *)phys_to_virt(d1))[1] = pattern(d1, 1);
    expect((l4[64] & (PTE_P | PTE_W | PTE_U | PTE_NX)) == (PTE_P | PTE_W | PTE_U),
           "intermediate entries: present, writable, user, executable (leaf decides)");
    expect(audit_ok(3, 1, 0), "audit: 3 lower tables, 1 leaf, 0 user leaves");

    /* Invalid and conflicting requests change nothing. */
    uint64_t free_frame = alloc();
    release(free_frame);
    static const struct { uint64_t va, pa; unsigned flags; int err; const char *what; } bad[] = {
        { VT, 0, VMM_WRITE, VMM_EEXIST, "duplicate mapping refused" },
        { VT + 8, 0, 0, VMM_EALIGN, "misaligned virtual address refused" },
        { VT2, 8, 0, VMM_EALIGN, "misaligned physical address refused" },
        { 0x0000800000000000ULL, 0, 0, VMM_ENONCANON, "first non-canonical address refused" },
        { 0x8000000000000000ULL, 0, 0, VMM_ENONCANON, "non-canonical high address refused" },
        { 0x1000, 0, 0, VMM_ERANGE, "null guard refused" },
        { NULL_GUARD_END - PAGE_SIZE, 0, 0, VMM_ERANGE, "top of the null guard refused" },
        { LOWER_MAP_END, 0, 0, VMM_ERANGE, "highest lower-half page refused" },
        { KERNEL_VMA + 0x200000, 0, 0, VMM_EKERNEL, "kernel alias refused" },
        { UPPER_HALF_START, 0, 0, VMM_EKERNEL, "upper half refused" },
        { 0xFFFFFFFFFFFFF000ULL, 0, 0, VMM_EKERNEL, "last page refused" },
        { VT2, VGA_TEXT_PHYS, 0, VMM_EPHYS, "frame below 1 MiB refused" },
        { VT2, PMM_LIMIT, 0, VMM_EPHYS, "frame at 1 GiB refused" },
        { VT2, 1ULL << 52, 0, VMM_EPHYS, "frame beyond physical addressing refused" },
        { VT2, 0, VMM_WRITE | VMM_EXEC, VMM_EFLAGS, "writable + executable refused" },
        { VT2, 0, VMM_WRITE | VMM_EXEC | VMM_USER, VMM_EFLAGS, "user W+X refused" },
        { VT2, 0, VMM_EXEC, VMM_EFLAGS, "supervisor executable page refused (only kernel text runs in ring 0)" },
        { VT2, 1, 0, VMM_EPHYS, "kernel text frame refused" },
        { VT2, 2, 0, VMM_EPHYS, "free frame refused" },
        { VT2, 0, 0x8, VMM_EFLAGS, "unknown flag refused" },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint64_t f = pmm_free_count();
        uint64_t frame = bad[i].pa ? bad[i].pa : d2;
        if (bad[i].err == VMM_EALIGN && bad[i].pa == 8)
            frame = d2 + 8;
        if (bad[i].pa == 1)
            frame = ((uint64_t)selftest_vmm - KERNEL_VMA) & ~(PAGE_SIZE - 1);
        if (bad[i].pa == 2)
            frame = free_frame;
        int err = vmm_map(K, bad[i].va, frame, bad[i].flags);
        expect(err == bad[i].err && pmm_free_count() == f && mapped_to(VT, d1, VMM_WRITE) &&
               audit_ok(3, 1, 0), bad[i].what);
    }
    expect(vmm_map(0x200001, VT2, d2, 0) == VMM_EROOT && vmm_map(PMM_LIMIT, VT2, d2, 0) == VMM_EROOT &&
           vmm_map(free_frame, VT2, d2, 0) == VMM_EROOT && vmm_map(0, VT2, d2, 0) == VMM_EROOT,
           "invalid roots refused (misaligned, >= 1 GiB, free frame, 0)");

    /* Read-only page in the same table: no new tables. */
    expect(vmm_map(K, VT2, d2, 0) == VMM_OK && pmm_free_count() == f0 - 3 - 3 &&
           mapped_to(VT2, d2, 0) && reads_as(VT2, d2), "read-only page shares the tables");
    /* User page (read-only, NX; nothing runs in ring 3): needs one new PT. */
    expect(vmm_map(K, VT3, d3, VMM_USER) == VMM_OK && pmm_free_count() == f0 - 3 - 4 &&
           mapped_to(VT3, d3, VMM_USER) && audit_ok(4, 3, 1), "user page: effective user, read-only, NX");

    /* B. Unmapping */
    expect(vmm_unmap(K, VT, &pa) == VMM_OK && pa == d1, "unmap returns the data frame");
    expect(pmm_frame_state_in(pmm_kernel(), d1) == PMM_FRAME_ALLOCATED && reads_as(KERNEL_VMA + d1, d1),
           "data frame not freed and not modified by unmap");
    struct vmm_mapping m;
    expect(vmm_lookup(K, VT, &m) == VMM_ENOTMAPPED && mapped_to(VT2, d2, 0),
           "lookup after unmap: absent; neighbour intact");
    expect(pmm_free_count() == f0 - 3 - 4, "tables still in use are kept");
    expect(vmm_unmap(K, VT, &pa) == VMM_ENOTMAPPED && pa == 0 && pmm_free_count() == f0 - 3 - 4,
           "repeated unmap refused without change");

    /* Remap the same address to another frame: the CPU must see the new frame. */
    expect(vmm_map(K, VT, d3, 0) == VMM_OK && reads_as(VT, d3),
           "remapped address reads the new frame (no stale translation)");
    expect(vmm_unmap(K, VT, &pa) == VMM_OK && pa == d3, "remapped page removed");

    expect(vmm_unmap(K, VT3, &pa) == VMM_OK && pa == d3 && pmm_free_count() == f0 - 3 - 3,
           "emptied page table released, shared upper tables kept");
    expect(vmm_unmap(K, VT2, &pa) == VMM_OK && pa == d2 && pmm_free_count() == f0 - 3,
           "last unmap releases PT, PD and PDPT");
    expect(l4[64] == 0 && audit_ok(0, 0, 0), "nothing reachable remains in the lower half");
    expect(vmm_unmap(K, VT2, &pa) == VMM_ENOTMAPPED && vmm_unmap(K, KERNEL_VMA, &pa) == VMM_EKERNEL &&
           vmm_unmap(K, 0x0000800000000000ULL, &pa) == VMM_ENONCANON && audit_ok(0, 0, 0),
           "invalid unmaps refused");

    /* Executable user page: NX clear in the leaf. */
    expect(vmm_map(K, VT, d1, VMM_EXEC | VMM_USER) == VMM_OK && mapped_to(VT, d1, VMM_EXEC | VMM_USER) &&
           vmm_unmap(K, VT, &pa) == VMM_OK, "executable user page represented");

    /* Allocation failure part-way: everything allocated by the call is released. */
    for (int budget = 0; budget < 3; budget++) {
        vmm_test_table_budget(budget);
        int err = vmm_map(K, VT, d1, VMM_WRITE);
        vmm_test_table_budget(-1);
        expect(err == VMM_ENOMEM && pmm_free_count() == f0 - 3 && l4[64] == 0 && audit_ok(0, 0, 0),
               "table allocation failure rolls back completely");
    }
    expect(vmm_map(K, VT, d1, VMM_WRITE) == VMM_OK, "map after rollback");
    vmm_test_table_budget(0);
    expect(vmm_map(K, VT3, d3, 0) == VMM_ENOMEM && pmm_free_count() == f0 - 3 - 3 &&
           mapped_to(VT, d1, VMM_WRITE) && audit_ok(3, 1, 0),
           "failure under existing tables keeps them and the old mapping");
    vmm_test_table_budget(-1);
    expect(vmm_unmap(K, VT, &pa) == VMM_OK && pmm_free_count() == f0 - 3 && audit_ok(0, 0, 0),
           "cleanup after the rollback test");

    release(d1);
    release(d2);
    release(d3);
    expect(pmm_free_count() == f0 && pmm_check_in(pmm_kernel()) == PMM_OK,
           "PMM free count back to its baseline; invariants hold");
}

/* Hand-built hierarchy (never loaded into CR3): large pages, bad entries, permission math. */
static void test_synthetic_tables(void)
{
    uint64_t f0 = pmm_free_count(), pa;
    uint64_t r = alloc(), pdpt = alloc(), pd = alloc(), pt1 = alloc(), pt2 = alloc(), pt3 = alloc();
    uint64_t data = alloc(), loose = alloc();
    uint64_t *R = phys_to_virt(r), *A = phys_to_virt(pdpt), *B = phys_to_virt(pd);
    uint64_t *C1 = phys_to_virt(pt1), *C2 = phys_to_virt(pt2), *C3 = phys_to_virt(pt3);
    struct vmm_mapping m;

    release(loose);                     /* a frame that is free, hence not a valid table */
    R[0] = pdpt | PTE_P | PTE_W | PTE_U;
    A[0] = pd | PTE_P | PTE_W | PTE_U;
    A[1] = 0x40000000ULL | PTE_P | PTE_W | PTE_PS;          /* 1 GiB page at VA 1 GiB */
    B[2] = 0x400000ULL | PTE_P | PTE_W | PTE_PS;            /* 2 MiB page at VA 4 MiB */
    B[3] = 0x7000000000ULL | PTE_P | PTE_W;                 /* table pointer above 1 GiB */
    B[4] = loose | PTE_P | PTE_W;                           /* table pointer to a free frame */
    B[5] = pt1 | PTE_P | PTE_W | PTE_PWT;                   /* unsupported bit */
    B[6] = pt1 | PTE_P | PTE_U;                             /* not writable */
    C1[0] = data | PTE_P | PTE_W | PTE_U | PTE_NX;
    B[7] = pt2 | PTE_P | PTE_W;                             /* not user */
    C2[0] = data | PTE_P | PTE_W | PTE_U | PTE_NX;
    B[8] = pt3 | PTE_P | PTE_W | PTE_U | PTE_NX;            /* NX at the PD level */
    B[9] = pt3 | PTE_P | PTE_W | PTE_U;
    C3[0] = data | PTE_P | PTE_U;                           /* executable leaf */

    uint64_t f1 = pmm_free_count(), b2 = B[2];
    expect(vmm_map(r, 0x401000, data, 0) == VMM_ELARGE && vmm_unmap(r, 0x400000, &pa) == VMM_ELARGE &&
           vmm_map(r, 0x40001000, data, 0) == VMM_ELARGE && B[2] == b2 && pmm_free_count() == f1,
           "2 MiB and 1 GiB pages are never treated as tables");
    expect(vmm_lookup(r, 0x401234, &m) == VMM_OK && m.pa == 0x401234 && m.page_size == 0x200000 &&
           vmm_lookup(r, 0x40000010, &m) == VMM_OK && m.pa == 0x40000010 && m.page_size == 0x40000000,
           "lookup resolves large pages with their offset");
    expect(vmm_map(r, 0x600000, data, 0) == VMM_ECORRUPT && vmm_map(r, 0x800000, data, 0) == VMM_ECORRUPT &&
           vmm_map(r, 0xA00000, data, 0) == VMM_ECORRUPT && pmm_free_count() == f1,
           "pointers above 1 GiB, to free frames, or with unknown bits are not followed");
    expect(vmm_lookup(r, 0xC00000, &m) == VMM_OK && m.flags == VMM_USER,
           "effective W is the AND over all levels");
    expect(vmm_lookup(r, 0xE00000, &m) == VMM_OK && m.flags == VMM_WRITE,
           "effective U is the AND over all levels");
    expect(vmm_lookup(r, 0x1000000, &m) == VMM_OK && m.flags == VMM_USER &&
           vmm_lookup(r, 0x1200000, &m) == VMM_OK && m.flags == (VMM_EXEC | VMM_USER),
           "effective NX is the OR over all levels");
    struct vmm_audit a;
    expect(vmm_audit_space(r, 0, &a) == VMM_ECORRUPT && a.errors >= 4 && a.first_error,
           "audit reports the malformed hierarchy");
    if (a.first_error) {
        serial_puts("selftest: audit of the bad hierarchy: ");
        serial_dec(a.errors);
        serial_puts(" errors, first: ");
        serial_puts(a.first_error);
        serial_puts("\r\n");
    }
    fill(data);                         /* pseudo-random entries */
    expect(vmm_audit_space(data, 0, &a) == VMM_ECORRUPT && a.errors > 0,
           "audit of a garbage table terminates and reports errors");

    /*
     * A page table inside the kernel image (like the boot tables): emptying it
     * must not free it, nor the PMM-owned tables above it.
     */
    static uint64_t image_pt[512] __attribute__((aligned(4096)));
    uint64_t image_pt_pa = (uint64_t)image_pt - KERNEL_VMA;
    memset(B, 0, PAGE_SIZE);
    B[2] = image_pt_pa | PTE_P | PTE_W | PTE_U;         /* VA [4 MiB, 6 MiB) */
    uint64_t f2 = pmm_free_count();
    expect(pmm_frame_state_in(pmm_kernel(), image_pt_pa) == PMM_FRAME_UNAVAILABLE &&
           vmm_map(r, 0x407000, data, 0) == VMM_OK && image_pt[7] != 0 && pmm_free_count() == f2,
           "mapping through a kernel-image table allocates nothing");
    expect(vmm_unmap(r, 0x407000, &pa) == VMM_OK && pa == data && image_pt[7] == 0 &&
           B[2] == (image_pt_pa | PTE_P | PTE_W | PTE_U) && A[0] != 0 && R[0] != 0 &&
           pmm_free_count() == f2, "emptied kernel-image table is kept, and the tables above it");

    memset(R, 0, PAGE_SIZE);
    memset(A, 0, PAGE_SIZE);
    memset(B, 0, PAGE_SIZE);
    release(pt3);
    release(pt2);
    release(pt1);
    release(pd);
    release(pdpt);
    release(r);
    release(data);
    expect(pmm_free_count() == f0, "synthetic hierarchy fully released");
}

static int streq(const char *a, const char *b)
{
    return a && b && strlen(a) == strlen(b) && memcmp(a, b, strlen(a)) == 0;
}

/* One defect per hand-built hierarchy: the audit must name exactly that defect. */
static void test_audit_detects(void)
{
    uint64_t f0 = pmm_free_count();
    uint64_t r = alloc(), l3 = alloc(), l2 = alloc(), l1 = alloc(), data = alloc();
    uint64_t *R = phys_to_virt(r), *L3 = phys_to_virt(l3), *L2 = phys_to_virt(l2);
    uint64_t *L1 = phys_to_virt(l1);
    struct vmm_audit a;

    /* Clean lower-half path: no error, one leaf. */
    R[1] = l3 | PTE_P | PTE_W | PTE_U;
    L3[0] = l2 | PTE_P | PTE_W | PTE_U;
    L2[0] = l1 | PTE_P | PTE_W | PTE_U;
    L1[0] = data | PTE_P | PTE_W | PTE_NX;
    expect(vmm_audit_space(r, 0, &a) == VMM_OK && a.leaves_lower == 1 && a.tables_lower == 3,
           "audit accepts a clean hierarchy");

    L1[0] = data | PTE_P | PTE_W;
    expect(vmm_audit_space(r, 0, &a) == VMM_ECORRUPT && a.errors == 1 &&
           streq(a.first_error, "writable and executable page"), "audit flags a W+X page");

    L1[0] = data | PTE_P | PTE_NX | PTE_G;
    expect(vmm_audit_space(r, 0, &a) == VMM_ECORRUPT && a.errors == 1 &&
           streq(a.first_error, "leaf entry with unsupported bits"), "audit flags unsupported leaf bits");

    L1[0] = PTE_W;
    expect(vmm_audit_space(r, 0, &a) == VMM_ECORRUPT && a.errors == 1 &&
           streq(a.first_error, "non-present entry not cleared"), "audit flags a stale non-present entry");

    L1[0] = 0x80000ULL | PTE_P | PTE_NX;
    expect(vmm_audit_space(r, 0, &a) == VMM_ECORRUPT && a.errors == 1 &&
           streq(a.first_error, "leaf maps a frame outside [1 MiB, 1 GiB)"),
           "audit flags a leaf outside the managed frames");

    /* Upper half: a user leaf under supervisor tables, then a user table entry. */
    R[1] = 0;
    R[300] = l3 | PTE_P | PTE_W;
    L3[0] = l2 | PTE_P | PTE_W;
    L2[0] = l1 | PTE_P | PTE_W;
    L1[0] = data | PTE_P | PTE_U | PTE_NX;
    expect(vmm_audit_space(r, 0, &a) == VMM_ECORRUPT && a.errors == 1 &&
           streq(a.first_error, "user-accessible upper-half page"), "audit flags a user page in the upper half");
    L1[0] = data | PTE_P | PTE_NX;
    L2[0] |= PTE_U;
    expect(vmm_audit_space(r, 0, &a) == VMM_ECORRUPT && a.errors == 1 &&
           streq(a.first_error, "user bit on an upper-half table entry"),
           "audit flags a user table entry in the upper half");
    expect(vmm_audit_space(r, 1, &a) == VMM_ECORRUPT && a.errors >= 2,
           "kernel-half audit flags a foreign upper-half entry and a missing alias");

    memset(R, 0, PAGE_SIZE);
    release(data);
    release(l1);
    release(l2);
    release(l3);
    release(r);
    expect(pmm_free_count() == f0, "audit test hierarchy released");
}

int selftest_vmm(void)
{
    const uint64_t *l4 = phys_to_virt(vmm_kernel_root());
    static uint64_t upper[256];

    failures = cases = 0;
    memcpy(upper, l4 + 256, sizeof(upper));
    test_layout_and_survival();
    test_map_unmap();
    test_synthetic_tables();
    test_audit_detects();
    expect(memcmp(upper, l4 + 256, sizeof(upper)) == 0, "kernel half of the PML4 unchanged");
    serial_puts("selftest: vmm ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

/* ---- isolated fault cases: each must end in a CPU page fault ---- */

static void no_fault(const char *what)
{
    serial_puts("vmm-test: no page fault: ");
    serial_puts(what);
    serial_puts("\r\n");
    panic("test case: expected page fault did not occur");
}

/* K3: the boot identity map is gone. */
void selftest_vmm_fault_lowaccess(void)
{
    serial_puts("vmm-test: reading 0x100000 through the removed identity map\r\n");
    (void)*(volatile uint32_t *)PMM_LOW_LIMIT;
    no_fault("low address still readable");
}

/* A translation used before vmm_unmap must not survive it. */
void selftest_vmm_fault_stale_tlb(void)
{
    uint64_t d = alloc(), pa;

    fill(d);
    if (vmm_map(vmm_kernel_root(), VT, d, VMM_WRITE) != VMM_OK || !reads_as(VT, d))
        panic("test case: setup failed");
    serial_puts("vmm-test: stale-tlb read through VT ok\r\n");
    if (vmm_unmap(vmm_kernel_root(), VT, &pa) != VMM_OK)
        panic("test case: unmap failed");
    (void)*(volatile uint64_t *)VT;
    no_fault("unmapped page still readable");
}

static void __attribute__((noinline)) nx_control(void)
{
    __asm__ volatile ("" : : : "memory");
}

/*
 * Executing from a page without VMM_EXEC must fault (NX). The control is a
 * call into the kernel text: since N2.0 vmm_map refuses supervisor-executable
 * pages, so the kernel text is the only code ring 0 can run.
 */
void selftest_vmm_fault_nx(void)
{
    uint64_t n = alloc();

    *(volatile uint8_t *)phys_to_virt(n) = 0xC3;            /* ret */
    if (vmm_map(vmm_kernel_root(), VT, n, VMM_WRITE) != VMM_OK)
        panic("test case: setup failed");
    nx_control();
    serial_puts("vmm-test: kernel text control returned\r\n");
    ((void (*)(void))VT)();
    no_fault("NX page executed");
}

/* A supervisor write to a read-only page must fault (CR0.WP). */
void selftest_vmm_fault_readonly(void)
{
    uint64_t d = alloc();

    fill(d);
    if (vmm_map(vmm_kernel_root(), VT, d, 0) != VMM_OK || !reads_as(VT, d))
        panic("test case: setup failed");
    serial_puts("vmm-test: read-only page readable\r\n");
    *(volatile uint64_t *)VT = 0;
    no_fault("read-only page written");
}
