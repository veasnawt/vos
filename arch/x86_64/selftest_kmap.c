/*
 * Kernel W^X map and protected stacks (N2.0), VOS_TEST only.
 *
 * selftest_kmap runs in every test image after kmap_init and restores all it
 * changes:
 *   - the live kernel half, page by page through vmm_lookup and the audit,
 *     and with real CPU reads of the text and read-only data;
 *   - the verifier on copies of the live tables with one defect each (text
 *     writable, data executable, guard mapped, a required page missing, ...):
 *     every defect must be refused with the expected diagnostic;
 *   - the audit on hand-built hierarchies that try to bypass W^X through
 *     another mapping of kernel frames;
 *   - the IST assignment and the guard pages below every kernel stack.
 *
 * selftest_kmap_case is the isolated fault cases: each ends in a CPU
 * exception and a kernel panic (exit status 35). Before the access it prints
 * "fault-test: expect key=value ...", which scripts/check-fault.sh compares
 * with the CPU-reported "exc:" line.
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "kmap.h"
#include "lib.h"
#include "memlayout.h"
#include "panic.h"
#include "pmm.h"
#include "thread.h"
#include "vmm.h"

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[], __kernel_phys_end[];
extern char boot_kstack_guard[], kernel_stack[], kernel_stack_top[];

#define LARGE_2M    0x200000ULL
#define SMALL_PAGES (KMAP_SMALL_SPAN / PAGE_SIZE)       /* entries in the kernel page tables */

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

static int streq(const char *a, const char *b)
{
    return a && b && strlen(a) == strlen(b) && memcmp(a, b, strlen(a)) == 0;
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

static unsigned idx(const void *kernel_va)
{
    return (unsigned)(((uint64_t)kernel_va - KERNEL_VMA) >> 12);
}

static unsigned text_pages(void)
{
    return (unsigned)((__text_end - __text_start) / PAGE_SIZE);
}

/* ---- the live kernel half ---- */

static void test_live(const struct boot_info *bi)
{
    const uint64_t K = vmm_kernel_root();
    const uint64_t *pd, *pt;
    uint64_t pt_pa, guards[KMAP_MAX_GUARDS];
    unsigned ng = kmap_guard_pages(guards, KMAP_MAX_GUARDS);
    struct vmm_mapping m;
    struct vmm_audit a;

    kmap_test_tables(&pd, &pt, &pt_pa);
    expect(kmap_active(), "kmap is active");
    const uint64_t *l4 = phys_to_virt(K), *pdpt = phys_to_virt(l4[511] & PTE_ADDR);
    expect((pdpt[510] & PTE_ADDR) == (uint64_t)pd - KERNEL_VMA && (pd[0] & PTE_ADDR) == pt_pa &&
           (pd[1] & PTE_ADDR) == pt_pa + PAGE_SIZE && !(pd[1] & (PTE_PS | PTE_U)) && (pd[2] & PTE_PS) &&
           KMAP_SMALL_TABLES == 2 && KMAP_LARGE_PAGES == 510,
           "PML4[511] -> PDPT[510] -> kernel directory -> two kernel page tables, then 2 MiB pages");
    expect(kmap_check(pd, pt, pt_pa, 1, bi) == 0, "live tables pass every kmap check through the walker");

    /* Every 4 KiB page of phys [0, 4 MiB) and every 2 MiB page above, through the walker. */
    unsigned wrong = 0, exec = 0, absent = 0, wx = 0, above = 0;
    for (uint64_t pa = 0; pa < KMAP_SMALL_SPAN; pa += PAGE_SIZE) {
        int r = vmm_lookup(K, KERNEL_VMA + pa + 0x7FF, &m);
        uint64_t want = kmap_expected_pte(pa);
        unsigned fl = ((want & PTE_W) ? VMM_WRITE : 0) | ((want & PTE_NX) ? 0 : VMM_EXEC);

        if (!want) {
            absent++;
            wrong += r != VMM_ENOTMAPPED;
            continue;
        }
        wrong += r != VMM_OK || m.pa != pa + 0x7FF || m.page_size != PAGE_SIZE || m.flags != fl;
        exec += (m.flags & VMM_EXEC) != 0;
        wx += (m.flags & VMM_EXEC) && (m.flags & VMM_WRITE);
        wrong += (m.flags & VMM_EXEC) && !kmap_is_text(pa);
        above += pa >= (uint64_t)__kernel_phys_end && m.flags == VMM_WRITE && m.page_size == PAGE_SIZE;
    }
    for (uint64_t pa = KMAP_SMALL_SPAN; pa < BOOT_MAP_LIMIT; pa += LARGE_2M) {
        for (uint64_t off = 0; off < LARGE_2M; off += LARGE_2M - 8) {
            int r = vmm_lookup(K, KERNEL_VMA + pa + off, &m);

            wrong += r != VMM_OK || m.pa != pa + off || m.page_size != LARGE_2M || m.flags != VMM_WRITE;
        }
    }
    expect(wrong == 0, "the whole alias of phys [0, 1 GiB) translates to its own frames with W^X permissions");
    expect((uint64_t)__kernel_phys_end <= KMAP_SMALL_SPAN &&
           above == (KMAP_SMALL_SPAN - (uint64_t)__kernel_phys_end) / PAGE_SIZE,
           "phys [image end, 4 MiB): 4 KiB pages, supervisor, RW NX");
    expect(exec == text_pages() && wx == 0, "ring-0 executable pages are exactly the kernel text, none writable");
    expect(absent == ng && ng == 5 + THREAD_STACKS,
           "the guard pages (boot, entry, 3 IST, every thread stack) are the only untranslated pages");
    expect(vmm_audit_space(K, 1, &a) == VMM_OK && a.exec_supervisor == text_pages() &&
           a.large_upper == KMAP_LARGE_PAGES && a.leaves_upper == SMALL_PAGES - ng,
           "audit: kernel half matches the W^X layout");

    /* The retired boot directory (still linked from the unreachable identity PDPT) is empty. */
    const uint64_t *idp = phys_to_virt(vmm_test_identity_pdpt());
    const uint64_t *old = phys_to_virt(idp[0] & PTE_ADDR);
    int empty = 1;
    for (int i = 0; i < 512; i++)
        empty &= old[i] == 0;
    expect(old != pd && empty, "boot page directory retired (all entries zero)");

    /* Real CPU reads: every text and read-only page. Writes are the isolated cases. */
    uint64_t sum = 0;
    for (const char *p = __text_start; p < __text_end; p += PAGE_SIZE)
        sum += *(volatile const uint8_t *)p;
    for (const char *p = __rodata_start; p < __rodata_end; p += PAGE_SIZE)
        sum += *(volatile const uint8_t *)p;
    expect(sum != 0xFFFFFFFFFFFFFFFFULL, "text and rodata readable by the CPU");
}

/*
 * A free frame in the 4 KiB-mapped area above the image (N3.0: phys
 * [2 MiB, 4 MiB) moved from a 2 MiB page to the second page table). The PMM
 * hands out the lowest free frame first; frames below the target are held
 * and released again. Returns 0 if none was found.
 */
static uint64_t small_span_frame(void)
{
    static uint64_t held[1024];
    uint64_t f, found = 0;
    unsigned n = 0;

    while (n < 1024 && pmm_alloc_frame(&f) == PMM_OK) {
        held[n++] = f;
        if (f >= LARGE_2M && f >= (uint64_t)__kernel_phys_end && f < KMAP_SMALL_SPAN) {
            found = f;
            n--;
            break;
        }
        if (f >= KMAP_SMALL_SPAN)
            break;
    }
    while (n)
        release(held[--n]);
    return found;
}

static void test_small_span(void)
{
    uint64_t f0 = pmm_free_count(), f = small_span_frame();
    volatile uint64_t *p = phys_to_virt(f);
    struct vmm_mapping m;

    expect(f != 0 && pmm_frame_state_in(pmm_kernel(), f) == PMM_FRAME_ALLOCATED,
           "a free frame in phys [2 MiB, 4 MiB) above the image is allocatable");
    if (!f)
        return;
    expect(vmm_lookup(vmm_kernel_root(), KERNEL_VMA + f + 8, &m) == VMM_OK && m.pa == f + 8 &&
           m.page_size == PAGE_SIZE && m.flags == VMM_WRITE,
           "its alias: one 4 KiB page, supervisor, writable, NX");
    p[0] = 0x4E332E3020363434ULL;
    p[511] = ~p[0];
    expect(p[0] == 0x4E332E3020363434ULL && p[511] == ~0x4E332E3020363434ULL,
           "CPU writes and reads back through the new 4 KiB alias");
    memset((void *)p, 0, PAGE_SIZE);
    release(f);
    expect(pmm_free_count() == f0, "small-span test frame released");
}

/* ---- the verifier on copies with one defect each ---- */

/* The copied page tables must be contiguous like the live ones: static, in .bss. */
static uint64_t vpt[KMAP_SMALL_TABLES * 512] __attribute__((aligned(4096)));
static uint64_t *cpd, *cpt, cpd_pa, cpt_pa;
static const uint64_t *lpd, *lpt;

static void reset(void)
{
    memcpy(cpt, lpt, sizeof(vpt));
    memcpy(cpd, lpd, PAGE_SIZE);
    for (unsigned t = 0; t < KMAP_SMALL_TABLES; t++)
        cpd[t] = (cpt_pa + t * PAGE_SIZE) | (lpd[t] & ~PTE_ADDR);
}

/* The audit (kernel half) on a root whose PML4[511] -> PDPT[510] leads to the copy. */
static uint64_t copy_root, copy_pdpt;

static int copy_audit_says(const char *first)
{
    struct vmm_audit a;
    int r = vmm_audit_space(copy_root, 1, &a);

    if (!first)
        return r == VMM_OK && a.large_upper == KMAP_LARGE_PAGES;
    if (r == VMM_OK || !streq(a.first_error, first)) {
        serial_puts("selftest: audit said: ");
        serial_puts(r == VMM_OK ? "ok" : a.first_error);
        serial_puts("\r\n");
        return 0;
    }
    return 1;
}

static int refused(int running_only, const struct boot_info *bi, const char *what, const char *problem)
{
    int r = running_only ? kmap_check_running(cpd, cpt, cpt_pa, 0, bi) : kmap_check(cpd, cpt, cpt_pa, 0, bi);
    const struct kmap_fail *f = kmap_failure();
    int ok = r != 0 && (!what || streq(f->what, what)) && streq(f->problem, problem);

    if (!ok) {
        serial_puts("selftest: kmap verifier said: ");
        serial_puts(r ? f->what : "accepted");
        serial_puts(r ? ": " : "");
        serial_puts(r ? f->problem : "");
        serial_puts("\r\n");
    }
    return ok;
}

static void test_verifier(const struct boot_info *bi)
{
    const struct pmm *k = pmm_kernel();
    uint64_t lpt_pa, rsp, f0 = pmm_free_count();
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    unsigned t = idx(__text_start) + 1, r = idx(__rodata_start), s, g = idx(boot_kstack_guard);

    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    s = idx((void *)rsp);
    kmap_test_tables(&lpd, &lpt, &lpt_pa);
    cpd_pa = alloc();
    cpt_pa = (uint64_t)vpt - KERNEL_VMA;
    cpd = phys_to_virt(cpd_pa);
    cpt = vpt;

    reset();
    expect(kmap_check(cpd, cpt, cpt_pa, 0, bi) == 0, "verifier accepts an exact copy of the live tables");

    /* The layout check. */
    reset();
    cpt[t] |= PTE_W;
    expect(refused(0, bi, "kernel page", "writable and executable"), "verifier refuses writable text");
    reset();
    cpt[r] |= PTE_W;
    expect(refused(0, bi, "read-only kernel data", "writable"), "verifier refuses writable rodata");
    reset();
    cpt[s] &= ~PTE_NX;
    cpt[s] &= ~PTE_W;
    expect(refused(0, bi, "kernel page", "executable outside the kernel text"),
           "verifier refuses executable data (the stack, even read-only)");
    reset();
    cpt[g] = ((uint64_t)g << 12) | PTE_P | PTE_W | PTE_NX;
    expect(refused(0, bi, "stack guard page", "mapped"), "verifier refuses a mapped guard page");
    reset();
    cpt[s] = 0;
    expect(refused(0, bi, "kernel page", "missing"), "verifier refuses a missing stack page");
    reset();
    cpt[s] = ((uint64_t)(s + 1) << 12) | PTE_P | PTE_W | PTE_NX;
    expect(refused(0, bi, "kernel page", "maps the wrong frame"), "verifier refuses a page on the wrong frame");
    reset();
    cpd[7] += LARGE_2M;
    expect(refused(0, bi, "2 MiB alias page", "maps the wrong frame"), "verifier refuses a shifted 2 MiB page");
    reset();
    cpd[7] &= ~PTE_NX;
    expect(refused(0, bi, "2 MiB alias page", "executable"), "verifier refuses an executable 2 MiB page");
    reset();
    cpd[0] |= PTE_U;
    expect(refused(0, bi, "PD[0]", "does not point to the kernel page table as a supervisor table"),
           "verifier refuses a user-accessible kernel table");
    reset();
    cpd[0] = cpd_pa | PTE_P | PTE_W;
    expect(refused(0, bi, "PD[0]", "does not point to the kernel page table as a supervisor table"),
           "verifier refuses a directory that links another table");

    /* N3.0: the second page table, phys [2 MiB, 4 MiB). hi is its last page, above the image. */
    unsigned hi = SMALL_PAGES - 1;
    expect(((uint64_t)__kernel_phys_end >> 12) <= hi, "the last 4 KiB-mapped page lies above the image");
    reset();
    cpt[hi] &= ~PTE_NX;
    expect(refused(0, bi, "kernel page", "writable and executable"),
           "verifier refuses a writable, executable page in the second table");
    reset();
    cpt[hi] &= ~(PTE_NX | PTE_W);
    expect(refused(0, bi, "kernel page", "executable outside the kernel text"),
           "verifier refuses a read-only executable page in the second table");
    reset();
    cpt[hi] |= PTE_U;
    expect(refused(0, bi, "kernel page", "unexpected flags"),
           "verifier refuses a user-accessible page in the second table");
    reset();
    cpt[hi] = 0;
    expect(refused(0, bi, "kernel page", "missing"), "verifier refuses a missing page in the second table");
    reset();
    cpt[hi] += PAGE_SIZE;
    expect(refused(0, bi, "kernel page", "maps the wrong frame"),
           "verifier refuses a page of the second table on the wrong frame");
    reset();
    cpd[1] |= PTE_U;
    expect(refused(0, bi, "PD[1]", "does not point to the kernel page table as a supervisor table"),
           "verifier refuses a user-accessible second table");
    reset();
    cpd[1] = cpt_pa | PTE_P | PTE_W;
    expect(refused(0, bi, "PD[1]", "does not point to the kernel page table as a supervisor table"),
           "verifier refuses PD[1] linking the first table again");
    reset();
    cpd[1] = LARGE_2M | PTE_P | PTE_W | PTE_PS | PTE_NX;
    expect(refused(0, bi, "PD[1]", "does not point to the kernel page table as a supervisor table"),
           "verifier refuses the old layout (PD[1] a 2 MiB page)");
    reset();
    cpd[2] += LARGE_2M;
    expect(refused(0, bi, "2 MiB alias page", "maps the wrong frame"),
           "verifier refuses a shifted first 2 MiB page (PD[2])");
    reset();
    cpd[2] = (cpt_pa + PAGE_SIZE) | PTE_P | PTE_W;
    expect(refused(0, bi, "2 MiB alias page", "not a large page"),
           "verifier refuses a page table under PD[2]");

    /* The running-kernel check alone: what would vanish or break at the switch. */
    reset();
    cpt[s] = cpt[s - 1] = 0;            /* the checker's own frame may lie one page lower */
    expect(refused(1, bi, "current stack", "not mapped"), "running check: current stack must stay mapped");
    reset();
    cpt[t] |= PTE_NX;
    expect(refused(1, bi, "kernel text", "not executable"), "running check: text must stay executable");
    reset();
    cpt[t] |= PTE_W;
    expect(refused(1, bi, "kernel text", "writable code"), "running check: text must not become writable");
    reset();
    cpt[idx((void *)gdtr.base)] &= ~PTE_W;
    expect(refused(1, bi, "GDT", "not writable"), "running check: GDT must stay writable");
    reset();
    cpt[idx(phys_to_virt(read_cr3() & PTE_ADDR))] = 0;
    expect(refused(1, bi, "PML4 (CR3)", "not mapped"), "running check: the active PML4 must stay mapped");
    reset();
    cpt[idx((const char *)k->avail + 4 * PAGE_SIZE)] &= ~PTE_W;
    expect(refused(1, bi, "avail bitmap", "not writable"), "running check: allocator bitmaps must stay writable");
    reset();
    cpt[g] = ((uint64_t)g << 12) | PTE_P | PTE_W | PTE_NX;
    expect(refused(1, bi, "stack guard page", "reachable"), "running check: guard pages must stay unreachable");
    reset();
    cpt[hi] = 0;
    expect(refused(1, bi, "kernel alias above the image", "not mapped"),
           "running check: the frames above the image must stay mapped");
    reset();
    cpt[hi] &= ~PTE_W;
    expect(refused(1, bi, "kernel alias above the image", "not writable"),
           "running check: the frames above the image must stay writable");
    reset();
    cpt[hi] &= ~PTE_NX;
    expect(refused(1, bi, "kernel alias above the image", "executable"),
           "running check: the frames above the image must not become executable");
    reset();
    cpd[1] = cpt_pa | PTE_P | PTE_W;    /* phys [2, 4) MiB would translate to [0, 2) MiB */
    expect(refused(1, bi, 0, "not mapped"), "running check: only the given second table is followed");
    reset();
    cpd[9] += LARGE_2M;                 /* a module may lie there and be reported first */
    expect(refused(1, bi, 0, "translates to the wrong frame"),
           "running check: the alias must keep phys_to_virt valid");
    uint64_t *info = bi->info_start < KMAP_SMALL_SPAN ? &cpt[bi->info_start >> 12] : &cpd[bi->info_start >> 21];
    reset();
    *info &= ~PTE_NX;
    expect(refused(1, bi, 0, "executable"), "running check: boot information must not become executable");
    reset();
    *info = 0;
    expect(refused(1, bi, 0, "not mapped"), "running check: boot information must stay mapped");

    /* The page-table audit on the same copies, reached as a kernel half. */
    uint64_t *R, *L3;
    copy_root = alloc();
    copy_pdpt = alloc();
    R = phys_to_virt(copy_root);
    L3 = phys_to_virt(copy_pdpt);
    R[511] = copy_pdpt | PTE_P | PTE_W;
    L3[510] = cpd_pa | PTE_P | PTE_W;
    reset();
    expect(copy_audit_says(0), "audit accepts an exact copy of the kernel half");
    reset();
    cpt[hi] &= ~PTE_NX;
    expect(copy_audit_says("writable and executable page"),
           "audit refuses a writable, executable page in the second table");
    reset();
    cpt[hi] &= ~(PTE_NX | PTE_W);
    expect(copy_audit_says("supervisor-executable page outside the kernel text"),
           "audit refuses a read-only executable page in the second table");
    reset();
    cpt[hi] |= PTE_U;
    expect(copy_audit_says("user-accessible upper-half page"), "audit refuses a user page in the second table");
    reset();
    cpt[hi] = 0;
    expect(copy_audit_says("kernel page missing from the W^X layout"),
           "audit refuses a missing page in the second table");
    reset();
    cpt[hi] += PAGE_SIZE;
    expect(copy_audit_says("kernel page differs from the W^X layout"),
           "audit refuses a page of the second table on the wrong frame");
    reset();
    cpt[hi] = (uint64_t)(__text_start - KERNEL_VMA) | PTE_P;
    expect(copy_audit_says("kernel page differs from the W^X layout"),
           "audit refuses a second (read-only, executable) alias of the text in the second table");
    reset();
    cpd[1] = LARGE_2M | PTE_P | PTE_W | PTE_PS | PTE_NX;
    expect(copy_audit_says("kernel alias page differs from the W^X layout"),
           "audit refuses the old layout (PD[1] a 2 MiB page)");
    reset();
    cpd[2] = (cpt_pa + PAGE_SIZE) | PTE_P | PTE_W;
    expect(copy_audit_says("4 KiB table under the 2 MiB kernel alias"), "audit refuses a page table under PD[2]");
    memset(R, 0, PAGE_SIZE);
    memset(L3, 0, PAGE_SIZE);
    release(copy_pdpt);
    release(copy_root);

    memset(vpt, 0, sizeof(vpt));
    release(cpd_pa);
    expect(pmm_free_count() == f0, "verifier test frames released");
}

/* ---- the audit against other mappings of kernel frames ---- */

static int audit_says(uint64_t root, const char *first)
{
    struct vmm_audit a;
    int r = vmm_audit_space(root, 0, &a);

    if (r == VMM_OK || !streq(a.first_error, first)) {
        serial_puts("selftest: audit said: ");
        serial_puts(r == VMM_OK ? "ok" : a.first_error);
        serial_puts("\r\n");
        return 0;
    }
    return 1;
}

static void test_alias_bypass(void)
{
    uint64_t f0 = pmm_free_count();
    uint64_t r = alloc(), l3 = alloc(), l2 = alloc(), l1 = alloc(), data = alloc();
    uint64_t *R = phys_to_virt(r), *L3 = phys_to_virt(l3), *L2 = phys_to_virt(l2), *L1 = phys_to_virt(l1);
    uint64_t text = (uint64_t)__text_start - KERNEL_VMA, guard = (uint64_t)boot_kstack_guard - KERNEL_VMA;
    struct vmm_audit a;

    /* Upper half (a second supervisor alias). */
    R[300] = l3 | PTE_P | PTE_W;
    L3[0] = l2 | PTE_P | PTE_W;
    L2[0] = l1 | PTE_P | PTE_W;
    L1[0] = text | PTE_P;
    expect(vmm_audit_space(r, 0, &a) == VMM_OK && a.exec_supervisor == 1,
           "audit accepts a read-only executable alias of kernel text");
    L1[0] = text | PTE_P | PTE_W | PTE_NX;
    expect(audit_says(r, "writable mapping of kernel text"), "audit refuses a writable alias of kernel text");
    L1[0] = data | PTE_P;
    expect(audit_says(r, "supervisor-executable page outside the kernel text"),
           "audit refuses a ring-0-executable data page");
    L1[0] = guard | PTE_P | PTE_NX;
    expect(audit_says(r, "stack guard page mapped"), "audit refuses a read-only alias of a guard page");
    L1[0] = 0;
    L2[0] = 0 | PTE_P | PTE_W | PTE_PS | PTE_NX;
    expect(audit_says(r, "writable mapping of kernel text"), "audit refuses a 2 MiB alias covering the text");
    L2[0] = 0 | PTE_P | PTE_PS | PTE_NX;
    expect(audit_says(r, "stack guard page mapped"), "audit refuses a 2 MiB alias covering a guard page");
    L2[0] = 0x200000 | PTE_P | PTE_PS;
    expect(audit_says(r, "supervisor-executable page outside the kernel text"),
           "audit refuses an executable 2 MiB page");

    /* Lower half: no mapping of any kernel-image frame, executable or not. */
    R[300] = 0;
    R[1] = l3 | PTE_P | PTE_W | PTE_U;
    L3[0] = l2 | PTE_P | PTE_W | PTE_U;
    L2[0] = l1 | PTE_P | PTE_W | PTE_U;
    L1[0] = text | PTE_P | PTE_U;
    expect(audit_says(r, "lower-half leaf maps a kernel image frame"), "audit refuses a user alias of kernel text");
    L1[0] = data | PTE_P;
    L2[0] &= ~PTE_U;
    expect(audit_says(r, "supervisor-executable page outside the kernel text"),
           "audit refuses a supervisor-executable lower-half page");

    memset(R, 0, PAGE_SIZE);
    memset(L3, 0, PAGE_SIZE);
    memset(L2, 0, PAGE_SIZE);
    memset(L1, 0, PAGE_SIZE);
    release(data);
    release(l1);
    release(l2);
    release(l3);
    release(r);
    expect(pmm_free_count() == f0, "alias test hierarchy released");
}

/* ---- IST and stack guards ---- */

static int unmapped(uint64_t va)
{
    struct vmm_mapping m;

    return vmm_lookup(vmm_kernel_root(), va, &m) == VMM_ENOTMAPPED && kmap_is_guard(va - KERNEL_VMA);
}

static int rw_nx(uint64_t va)
{
    struct vmm_mapping m;

    return vmm_lookup(vmm_kernel_root(), va, &m) == VMM_OK && m.flags == VMM_WRITE;
}

static void test_stacks(void)
{
    static const char *const ist_name[4] = { 0, "ist1", "ist2", "ist3" };
    unsigned others = 0;
    uint64_t rsp, rsp0 = gdt_tss_rsp0();

    for (unsigned v = 0; v < IDT_ENTRIES; v++)
        if (v != 2 && v != 8 && v != 18)
            others += idt_gate_ist(v) != 0;
    expect(idt_gate_ist(8) == IST_DOUBLE_FAULT && idt_gate_ist(2) == IST_NMI &&
           idt_gate_ist(18) == IST_MACHINE_CHECK && others == 0,
           "IDT: #DF on IST1, NMI on IST2, #MC on IST3, every other gate on no IST");
    for (unsigned n = 1; n <= 3; n++) {
        uint64_t top = gdt_tss_ist(n);

        expect(top % 16 == 0 && streq(gdt_stack_name(top - 8), ist_name[n]) &&
               streq(gdt_stack_name(top - 4096), ist_name[n]) && rw_nx(top - 8) && rw_nx(top - 4096) &&
               unmapped(top - 4096 - 4096) && unmapped(top - 4096 - 8),
               "IST stack: 4 KiB, RW NX, directly above an unmapped guard page");
    }
    expect(gdt_tss_ist(1) != gdt_tss_ist(2) && gdt_tss_ist(2) != gdt_tss_ist(3) &&
           gdt_tss_ist(1) != gdt_tss_ist(3) && gdt_tss_ist(4) == 0, "three distinct IST stacks, IST4-7 unused");
    expect(streq(gdt_stack_name(rsp0 - 8), "entry") && streq(gdt_stack_name(rsp0 - 16384), "entry") &&
           rw_nx(rsp0 - 16384) && unmapped(rsp0 - 16384 - 8) && unmapped(rsp0 - 16384 - 4096),
           "RSP0 entry stack: 16 KiB above an unmapped guard page");
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    expect(rsp > (uint64_t)kernel_stack && rsp <= (uint64_t)kernel_stack_top &&
           (uint64_t)kernel_stack - PAGE_SIZE == (uint64_t)boot_kstack_guard &&
           unmapped((uint64_t)boot_kstack_guard) && unmapped((uint64_t)kernel_stack - 8) &&
           rw_nx((uint64_t)kernel_stack), "boot kernel stack: running on it, guard page below unmapped");
    expect(gdt_stack_canaries_intact(), "stack canaries intact");
}

int selftest_kmap(const struct boot_info *bi)
{
    failures = cases = 0;
    test_live(bi);
    test_small_span();
    test_verifier(bi);
    test_alias_bypass();
    test_stacks();
    serial_puts("selftest: kmap ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

/* ---- isolated fault cases ---- */

static const uint64_t rodata_target = 0x524F44415441ULL;        /* "RODATA", in .rodata */
static uint8_t data_code[16] = { 0xC3 };                        /* ret, in .data */

static void expect_fault(const char *fields, uint64_t addr, const char *more)
{
    serial_puts("fault-test: expect ");
    serial_puts(fields);
    serial_hex(addr);
    serial_puts(more);
    serial_puts("\r\n");
}

static void __attribute__((noinline)) not_reached(const char *what)
{
    serial_puts("fault-test: no fault: ");
    serial_puts(what);
    serial_puts("\r\n");
    panic("test case: expected CPU exception did not occur");
}

#ifdef VOS_TEST_CASE_kstack_overflow
static uint64_t __attribute__((noinline)) recurse(uint64_t n)
{
    volatile uint8_t buf[256];

    buf[n & 255] = (uint8_t)n;
    if (n == ~0ULL)
        return 0;
    return recurse(n + 1) + buf[n & 255];
}
#endif

#if defined(VOS_TEST_CASE_wx_text_write) || defined(VOS_TEST_CASE_wx_rodata_write) || \
    defined(VOS_TEST_CASE_wx_data_exec) || defined(VOS_TEST_CASE_wx_alias_exec) || \
    defined(VOS_TEST_CASE_wx_small_alias_exec) || \
    defined(VOS_TEST_CASE_wx_guard_read) || defined(VOS_TEST_CASE_kstack_overflow) || \
    defined(VOS_TEST_CASE_nmi_gate) || defined(VOS_TEST_CASE_mc_gate) || defined(VOS_TEST_CASE_nmi_real)
#define KMAP_CASE 1
#endif

void selftest_kmap_case(int self_tests_failed)
{
#ifdef KMAP_CASE
    if (self_tests_failed) {
        serial_puts("fault-test: skipped, a self-test failed\r\n");
        return;
    }
#endif
    (void)self_tests_failed;
#ifdef VOS_TEST_CASE_wx_text_write
    uint8_t *p = (uint8_t *)selftest_kmap;

    serial_puts("fault-test: kernel write to its own text\r\n");
    expect_fault("vec=14 err=0x3 cr2=", (uint64_t)p, " stack=kernel");
    *(volatile uint8_t *)p = 0xCC;
    not_reached("text written");
#endif
#ifdef VOS_TEST_CASE_wx_rodata_write
    serial_puts("fault-test: kernel write to .rodata\r\n");
    expect_fault("vec=14 err=0x3 cr2=", (uint64_t)&rodata_target, " stack=kernel");
    *(volatile uint64_t *)&rodata_target = 0;
    not_reached("rodata written");
#endif
#ifdef VOS_TEST_CASE_wx_data_exec
    serial_puts("fault-test: kernel executes writable .data\r\n");
    expect_fault("vec=14 err=0x11 rip=", (uint64_t)data_code, " stack=kernel");
    ((void (*)(void))(uintptr_t)data_code)();
    not_reached(".data executed");
#endif
#ifdef VOS_TEST_CASE_wx_alias_exec
    /* A frame that is free in the allocator, above 4 MiB: reached through a 2 MiB alias page. */
    static uint64_t held[1024];
    uint64_t f;
    unsigned n = 0;
    struct vmm_mapping m;

    do {
        if (n == 1024 || pmm_alloc_frame(&f) != PMM_OK)
            panic("test case: setup failed");
        held[n++] = f;
    } while (f < KMAP_SMALL_SPAN);
    for (unsigned i = 0; i < n; i++)
        release(held[i]);
    if (pmm_frame_state_in(pmm_kernel(), f) != PMM_FRAME_FREE ||
        vmm_lookup(vmm_kernel_root(), KERNEL_VMA + f, &m) != VMM_OK || m.page_size != LARGE_2M ||
        m.flags != VMM_WRITE)
        panic("test case: setup failed");
    serial_puts("fault-test: kernel executes free frame ");
    serial_hex(f);
    serial_puts(" through the alias (2 MiB page, writable, NX)\r\n");
    expect_fault("vec=14 err=0x11 rip=", KERNEL_VMA + f, " stack=kernel");
    ((void (*)(void))phys_to_virt(f))();
    not_reached("alias executed");
#endif
#ifdef VOS_TEST_CASE_wx_small_alias_exec
    /* N3.0: a frame in phys [2 MiB, 4 MiB) above the image, reached through a 4 KiB page of the second table. */
    uint64_t sf = small_span_frame();
    struct vmm_mapping sm;

    if (!sf || vmm_lookup(vmm_kernel_root(), KERNEL_VMA + sf, &sm) != VMM_OK || sm.page_size != PAGE_SIZE ||
        sm.flags != VMM_WRITE)
        panic("test case: setup failed");
    *(volatile uint8_t *)phys_to_virt(sf) = 0xC3;       /* ret: an executable page would return */
    serial_puts("fault-test: kernel executes frame ");
    serial_hex(sf);
    serial_puts(" through the alias (4 KiB page of the second table, writable, NX)\r\n");
    expect_fault("vec=14 err=0x11 rip=", KERNEL_VMA + sf, " stack=kernel");
    ((void (*)(void))phys_to_virt(sf))();
    not_reached("small-span alias executed");
#endif
#ifdef VOS_TEST_CASE_wx_guard_read
    uint64_t g[KMAP_MAX_GUARDS];

    if (kmap_guard_pages(g, KMAP_MAX_GUARDS) < 2)
        panic("test case: setup failed");
    serial_puts("fault-test: kernel reads the guard page below the RSP0 entry stack\r\n");
    expect_fault("vec=14 err=0x0 cr2=", g[1] + 0x800, " stack=kernel");
    (void)*(volatile uint64_t *)(g[1] + 0x800);
    not_reached("guard page read");
#endif
#ifdef VOS_TEST_CASE_kstack_overflow
    serial_puts("fault-test: unbounded recursion on the boot kernel stack\r\n");
    expect_fault("vec=8 err=0x0 cr2page=", (uint64_t)boot_kstack_guard, " stack=ist1");
    recurse(0);
    not_reached("stack overflow");
#endif
#ifdef VOS_TEST_CASE_nmi_gate
    serial_puts("fault-test: int $2 (software, gate and IST2 stack only; not a hardware NMI)\r\n");
    expect_fault("vec=2 err=- cs=", GDT_KERNEL_CS, " stack=ist2");
    __asm__ volatile ("int $2");
    not_reached("int $2 returned");
#endif
#ifdef VOS_TEST_CASE_mc_gate
    serial_puts("fault-test: int $18 (software, gate and IST3 stack only; not a machine check)\r\n");
    expect_fault("vec=18 err=- cs=", GDT_KERNEL_CS, " stack=ist3");
    __asm__ volatile ("int $18");
    not_reached("int $18 returned");
#endif
#ifdef VOS_TEST_CASE_nmi_real
    serial_puts("fault-test: interrupts disabled; waiting for an NMI from the QEMU monitor\r\n");
    expect_fault("vec=2 err=- cs=", GDT_KERNEL_CS, " stack=ist2");
    for (;;)
        __asm__ volatile ("cli; hlt");
#endif
    (void)expect_fault;
    (void)not_reached;
    (void)rodata_target;
    (void)data_code;
}
