#include "kmap.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "lib.h"
#include "memlayout.h"
#include "pmm.h"
#include "vmm.h"

/* Linker symbols: boot symbols are physical, the others higher-half virtual. */
extern char __kernel_phys_start[], __kernel_phys_end[], __boot_data_start[];
extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[], __data_start[];
extern char boot_kstack_guard[], kernel_stack[], kernel_stack_top[];

#define LARGE_2M        0x200000ULL
#define AD_BITS         (PTE_A | PTE_D)
#define PDE_TABLE       (PTE_P | PTE_W)                         /* supervisor, no NX: the leaf decides */
#define PTE_TEXT        PTE_P                                   /* R-X */
#define PTE_RO          (PTE_P | PTE_NX)                        /* R-- */
#define PTE_RW          (PTE_P | PTE_W | PTE_NX)                /* RW- */
#define PDE_RW_2M       (PTE_P | PTE_W | PTE_PS | PTE_NX)       /* RW- */

static uint64_t kpd[512] __attribute__((aligned(4096)));
static uint64_t kpt[512] __attribute__((aligned(4096)));
static struct kmap_fail fail;
static int active;
static volatile uint64_t probe;         /* written through the new map after the switch */

static uint64_t kva(const void *p)
{
    return (uint64_t)p;
}

static uint64_t kphys(const void *p)
{
    return (uint64_t)p - KERNEL_VMA;
}

unsigned kmap_guard_pages(uint64_t *va, unsigned max)
{
    unsigned n = 0;

    if (n < max)
        va[n++] = kva(boot_kstack_guard);
    return n + gdt_guard_pages(va + n, max - n);
}

int kmap_is_guard(uint64_t pa)
{
    uint64_t g[KMAP_MAX_GUARDS];
    unsigned n = kmap_guard_pages(g, KMAP_MAX_GUARDS);

    pa &= ~(PAGE_SIZE - 1);
    for (unsigned i = 0; i < n; i++)
        if (pa == g[i] - KERNEL_VMA)
            return 1;
    return 0;
}

int kmap_is_text(uint64_t pa)
{
    return pa >= kphys(__text_start) && pa < kphys(__text_end);
}

static int is_readonly_data(uint64_t pa)
{
    return (pa >= kphys(__rodata_start) && pa < kphys(__rodata_end)) ||
           (pa >= (uint64_t)__kernel_phys_start && pa < (uint64_t)__boot_data_start);
}

uint64_t kmap_expected_pte(uint64_t pa)
{
    pa &= ~(PAGE_SIZE - 1);
    if (pa >= KMAP_SMALL_SPAN || kmap_is_guard(pa))
        return 0;
    if (kmap_is_text(pa))
        return pa | PTE_TEXT;
    if (is_readonly_data(pa))
        return pa | PTE_RO;
    return pa | PTE_RW;
}

uint64_t kmap_expected_pde(unsigned i)
{
    return i >= 1 && i < 512 ? (uint64_t)i * LARGE_2M | PDE_RW_2M : 0;
}

/* ---- checks ---- */

static int failed(const char *what, const char *problem, uint64_t va)
{
    fail.what = what;
    fail.problem = problem;
    fail.va = va;
    return -1;
}

struct xlate {
    const uint64_t *pd, *pt;
    uint64_t pt_pa;
    int live;
};

/* Translate a kernel-alias address through the candidate (or live) tables. 1 if present. */
static int translate(const struct xlate *x, uint64_t va, uint64_t *pa, unsigned *flags)
{
    if (x->live) {
        struct vmm_mapping m;

        if (vmm_lookup(read_cr3() & PTE_ADDR, va, &m) != VMM_OK)
            return 0;
        *pa = m.pa;
        *flags = m.flags;
        return 1;
    }
    if (va < KERNEL_VMA || va >= KERNEL_ALIAS_END)
        return 0;

    /* PML4[511] and PDPT[510] are present, writable, supervisor, without NX (checked by kmap_init). */
    uint64_t off = va - KERNEL_VMA, e = x->pd[off >> 21];
    unsigned w, nx;

    if (!(e & PTE_P))
        return 0;
    w = (e & PTE_W) != 0;
    nx = (e & PTE_NX) != 0;
    if (e & PTE_PS) {
        *pa = (e & PTE_ADDR & ~(LARGE_2M - 1)) + (off & (LARGE_2M - 1));
    } else {
        if ((e & PTE_ADDR) != x->pt_pa)
            return 0;
        e = x->pt[(off >> 12) & 511];
        if (!(e & PTE_P))
            return 0;
        w &= (e & PTE_W) != 0;
        nx |= (e & PTE_NX) != 0;
        *pa = (e & PTE_ADDR) + (off & (PAGE_SIZE - 1));
    }
    *flags = (w ? VMM_WRITE : 0) | (nx ? 0 : VMM_EXEC) | ((e & PTE_U) ? VMM_USER : 0);
    return 1;
}

#define NEED_CODE   1       /* present, executable, not writable */
#define NEED_READ   2       /* present, not executable */
#define NEED_RW     3       /* present, writable, not executable */

/* Every page of [start, end) must translate to its own frame with the required permissions. */
static int require(const struct xlate *x, uint64_t start, uint64_t end, int need, const char *what)
{
    for (uint64_t va = start & ~(PAGE_SIZE - 1); va < end; va += PAGE_SIZE) {
        uint64_t pa;
        unsigned fl;

        if (need == NEED_RW && kmap_is_guard(va - KERNEL_VMA))
            continue;       /* ranges such as .bss contain the guards; checked separately */
        if (!translate(x, va, &pa, &fl))
            return failed(what, "not mapped", va);
        if (pa != va - KERNEL_VMA)
            return failed(what, "translates to the wrong frame", va);
        if (fl & VMM_USER)
            return failed(what, "user-accessible", va);
        if (need == NEED_CODE && !(fl & VMM_EXEC))
            return failed(what, "not executable", va);
        if (need == NEED_CODE && (fl & VMM_WRITE))
            return failed(what, "writable code", va);
        if (need != NEED_CODE && (fl & VMM_EXEC))
            return failed(what, "executable", va);
        if (need == NEED_RW && !(fl & VMM_WRITE))
            return failed(what, "not writable", va);
    }
    return 0;
}

struct dtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* The handler address of IDT gate v (0 if the gate is not present). */
static uint64_t gate_handler(const struct dtr *idtr, unsigned v)
{
    const uint8_t *g = (const uint8_t *)idtr->base + v * 16;

    if ((uint64_t)v * 16 + 15 > idtr->limit || !(g[5] & 0x80))
        return 0;
    return (uint64_t)(g[0] | g[1] << 8) | (uint64_t)(g[6] | g[7] << 8) << 16 |
           (uint64_t)(g[8] | g[9] << 8 | g[10] << 16 | (uint32_t)g[11] << 24) << 32;
}

/* Structure of the directory and table themselves, against the intended layout. */
static int check_structure(const uint64_t *pd, const uint64_t *pt, uint64_t pt_pa)
{
    if ((pd[0] & ~PTE_A) != (pt_pa | PDE_TABLE))
        return failed("PD[0]", "does not point to the kernel page table as a supervisor table", KERNEL_VMA);
    for (unsigned i = 1; i < 512; i++) {
        uint64_t e = pd[i] & ~AD_BITS, va = KERNEL_VMA + i * LARGE_2M;

        if (!(e & PTE_P))
            return failed("2 MiB alias page", "missing", va);
        if (!(e & PTE_PS))
            return failed("2 MiB alias page", "not a large page", va);
        if ((e & PTE_ADDR) != i * LARGE_2M)
            return failed("2 MiB alias page", "maps the wrong frame", va);
        if (!(e & PTE_NX))
            return failed("2 MiB alias page", "executable", va);
        if (e != kmap_expected_pde(i))
            return failed("2 MiB alias page", "unexpected flags", va);
    }
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = pt[i] & ~AD_BITS, pa = (uint64_t)i * PAGE_SIZE, want = kmap_expected_pte(pa);
        uint64_t va = KERNEL_VMA + pa;

        if (!want && e)
            return failed("stack guard page", "mapped", va);
        if (!want)
            continue;
        if (!(e & PTE_P))
            return failed("kernel page", "missing", va);
        if ((e & PTE_ADDR) != pa)
            return failed("kernel page", "maps the wrong frame", va);
        if ((e & PTE_W) && !(e & PTE_NX))
            return failed("kernel page", "writable and executable", va);
        if (!(e & PTE_NX) && !kmap_is_text(pa))
            return failed("kernel page", "executable outside the kernel text", va);
        if ((e & PTE_W) && !(want & PTE_W))
            return failed(kmap_is_text(pa) ? "kernel text" : "read-only kernel data", "writable", va);
        if (e != want)
            return failed("kernel page", "unexpected flags", va);
    }
    return 0;
}

int kmap_check(const uint64_t *pd, const uint64_t *pt, uint64_t pt_pa, int live,
               const struct boot_info *bi)
{
    struct xlate x = { pd, pt, pt_pa, live };
    const struct pmm *k = pmm_kernel();
    struct dtr gdtr, idtr;
    uint64_t rip, rsp, guards[KMAP_MAX_GUARDS], pa;
    unsigned ng, fl;

    fail.what = fail.problem = 0;
    fail.va = 0;
    if (check_structure(pd, pt, pt_pa))
        return -1;

    /* Guards: no translation at all (the 2 MiB pages cannot cover them: checked above). */
    ng = kmap_guard_pages(guards, KMAP_MAX_GUARDS);
    for (unsigned i = 0; i < ng; i++)
        if (translate(&x, guards[i], &pa, &fl))
            return failed("stack guard page", "reachable", guards[i]);

    /* Code: the whole text, the running function and its caller, every IDT handler. */
    __asm__ volatile ("lea 0(%%rip), %0" : "=r"(rip));
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    __asm__ volatile ("sidt %0" : "=m"(idtr));
    if (require(&x, kva(__text_start), kva(__text_end), NEED_CODE, "kernel text") ||
        require(&x, rip, rip + 1, NEED_CODE, "running code") ||
        require(&x, (uint64_t)__builtin_return_address(0), (uint64_t)__builtin_return_address(0) + 1,
                NEED_CODE, "caller"))
        return -1;
    for (unsigned v = 0; v < IDT_ENTRIES; v++) {
        uint64_t h = gate_handler(&idtr, v);

        if (h && require(&x, h, h + 1, NEED_CODE, "IDT handler"))
            return -1;
    }

    /* Read-only data. */
    if (require(&x, kva(__rodata_start), kva(__rodata_end), NEED_READ, "kernel rodata") ||
        require(&x, KERNEL_VMA + (uint64_t)__kernel_phys_start, KERNEL_VMA + (uint64_t)__boot_data_start,
                NEED_READ, "boot code"))
        return -1;

    /* Writable state the kernel uses from now on. */
    const uint64_t *l4 = phys_to_virt(read_cr3() & PTE_ADDR);
    uint64_t pdpt = l4[511] & PTE_ADDR;
    if (require(&x, rsp, rsp + 1, NEED_RW, "current stack") ||
        require(&x, kva(kernel_stack), kva(kernel_stack_top), NEED_RW, "boot kernel stack") ||
        require(&x, gdt_tss_rsp0() - 16384, gdt_tss_rsp0(), NEED_RW, "RSP0 entry stack") ||
        require(&x, gdtr.base, gdtr.base + gdtr.limit + 1, NEED_RW, "GDT") ||
        require(&x, idtr.base, idtr.base + idtr.limit + 1, NEED_RW, "IDT") ||
        require(&x, gdt_tss_base(), gdt_tss_base() + 104, NEED_RW, "TSS") ||
        require(&x, KERNEL_VMA + (read_cr3() & PTE_ADDR), KERNEL_VMA + (read_cr3() & PTE_ADDR) + 1,
                NEED_RW, "PML4 (CR3)") ||
        require(&x, KERNEL_VMA + pdpt, KERNEL_VMA + pdpt + 1, NEED_RW, "kernel PDPT") ||
        require(&x, kva(kpd), kva(kpd) + PAGE_SIZE, NEED_RW, "kernel page directory") ||
        require(&x, kva(kpt), kva(kpt) + PAGE_SIZE, NEED_RW, "kernel page table") ||
        require(&x, kva(k), kva(k) + sizeof(*k), NEED_RW, "allocator state") ||
        require(&x, kva(k->avail), kva(k->avail) + PMM_WORDS(k->frames) * 8, NEED_RW, "avail bitmap") ||
        require(&x, kva(k->alloc), kva(k->alloc) + PMM_WORDS(k->frames) * 8, NEED_RW, "alloc bitmap") ||
        require(&x, kva(__data_start), KERNEL_VMA + (uint64_t)__kernel_phys_end, NEED_RW, "kernel data") ||
        require(&x, KERNEL_VMA + (uint64_t)__boot_data_start, kva(__text_start), NEED_RW,
                "boot data and page tables") ||
        require(&x, KERNEL_VMA + VGA_TEXT_PHYS, KERNEL_VMA + VGA_TEXT_PHYS + 4000, NEED_RW, "VGA text"))
        return -1;
    for (unsigned i = 1; i <= 3; i++)
        if (require(&x, gdt_tss_ist(i) - 4096, gdt_tss_ist(i), NEED_RW, "IST stack"))
            return -1;

    /* Boot information and modules: read through the alias, never executed. */
    if (bi) {
        if (require(&x, KERNEL_VMA + bi->info_start, KERNEL_VMA + bi->info_end, NEED_READ, "boot information"))
            return -1;
        for (uint32_t i = 0; i < bi->module_count; i++)
            if (require(&x, KERNEL_VMA + bi->modules[i].start, KERNEL_VMA + bi->modules[i].end, NEED_READ,
                        "boot module"))
                return -1;
    }

    /* The whole alias keeps translating to phys [0, 1 GiB), one probe per 2 MiB page. */
    for (unsigned i = 1; i < 512; i++)
        if (require(&x, KERNEL_VMA + i * LARGE_2M, KERNEL_VMA + i * LARGE_2M + 1, NEED_RW, "kernel alias"))
            return -1;
    return 0;
}

/* ---- the transition ---- */

static void flush_all(void)
{
    uint64_t cr4 = read_cr4();

    if (cr4 & (1ULL << 7)) {            /* CR4.PGE: global entries survive a CR3 reload */
        write_cr4(cr4 & ~(1ULL << 7));
        write_cr4(cr4);
    }
    write_cr3(read_cr3());
}

static void count(unsigned *text, unsigned *ro, unsigned *rw, unsigned *guard)
{
    *text = *ro = *rw = *guard = 0;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = kpt[i];

        if (!(e & PTE_P))
            ++*guard;
        else if (!(e & PTE_NX))
            ++*text;
        else if (e & PTE_W)
            ++*rw;
        else
            ++*ro;
    }
}

int kmap_init(const struct boot_info *bi)
{
    uint64_t *l4 = phys_to_virt(read_cr3() & PTE_ADDR);
    uint64_t *pdpt, old;

    fail.what = fail.problem = 0;
    if (active)
        return failed("kmap", "already active", 0);
    if ((l4[511] & (PTE_P | PTE_W | PTE_U | PTE_PS | PTE_NX)) != (PTE_P | PTE_W))
        return failed("PML4[511]", "not a present, writable, supervisor table entry", KERNEL_VMA);
    pdpt = phys_to_virt(l4[511] & PTE_ADDR);
    old = pdpt[510];
    if ((old & (PTE_P | PTE_W | PTE_U | PTE_PS | PTE_NX)) != (PTE_P | PTE_W))
        return failed("PDPT[510]", "not a present, writable, supervisor table entry", KERNEL_VMA);
    if ((uint64_t)__kernel_phys_end > KMAP_SMALL_SPAN)     /* also a linker assertion */
        return failed("kernel image", "extends beyond 2 MiB", 0);

    /* 1. Build. */
    for (unsigned i = 0; i < 512; i++)
        kpt[i] = kmap_expected_pte((uint64_t)i * PAGE_SIZE);
    kpd[0] = kphys(kpt) | PDE_TABLE;
    for (unsigned i = 1; i < 512; i++)
        kpd[i] = kmap_expected_pde(i);

    /* 2. Check the candidate against what the running kernel needs. */
    if (kmap_check(kpd, kpt, kphys(kpt), 0, bi))
        return -1;

    /* 3. Switch: one entry, then drop every cached translation and paging-structure entry. */
    pdpt[510] = kphys(kpd) | PDE_TABLE;
    flush_all();

    /*
     * 4. Check the live tables through the walker (this code, its stack and the
     * write below already run on the new map); roll back on failure.
     */
    const uint64_t *lpd = phys_to_virt(pdpt[510] & PTE_ADDR);
    const uint64_t *lpt = phys_to_virt(lpd[0] & PTE_ADDR);
    probe = 0x6B6D6170;                 /* "kmap": a write through the new map */
    if (kmap_check(lpd, lpt, lpd[0] & PTE_ADDR, 1, bi) || probe != 0x6B6D6170) {
        pdpt[510] = old;
        flush_all();
        if (!fail.problem)
            failed("post-switch probe", "failed", (uint64_t)&probe);
        return -1;
    }

    /* 5. Retire the boot directory: unreachable now, and never again W+X if re-linked. */
    memset(phys_to_virt(old & PTE_ADDR), 0, PAGE_SIZE);
    active = 1;

    unsigned text, ro, rw, guard;
    count(&text, &ro, &rw, &guard);
    serial_puts("kmap: phys 0-2 MiB as 4 KiB pages: ");
    serial_dec(text);
    serial_puts(" text R-X, ");
    serial_dec(ro);
    serial_puts(" R NX, ");
    serial_dec(rw);
    serial_puts(" RW NX, ");
    serial_dec(guard);
    serial_puts(" guard pages unmapped; ");
    serial_dec(KMAP_LARGE_PAGES);
    serial_puts(" x 2 MiB RW NX\r\n");
    serial_puts("kmap: text ");
    serial_hex(kva(__text_start));
    serial_puts("-");
    serial_hex(kva(__text_end));
    serial_puts(", rodata ");
    serial_hex(kva(__rodata_start));
    serial_puts("-");
    serial_hex(kva(__rodata_end));
    serial_puts("\r\n");
    serial_puts("kmap: PDPT[510] ");
    serial_hex(old & PTE_ADDR);
    serial_puts(" -> ");
    serial_hex(kphys(kpd));
    serial_puts(", checked before and after the switch; boot directory retired\r\n");
    return 0;
}

const struct kmap_fail *kmap_failure(void)
{
    return &fail;
}

int kmap_active(void)
{
    return active;
}

#ifdef VOS_TEST
void kmap_test_tables(const uint64_t **pd, const uint64_t **pt, uint64_t *pt_pa)
{
    *pd = kpd;
    *pt = kpt;
    *pt_pa = kphys(kpt);
}
#endif
