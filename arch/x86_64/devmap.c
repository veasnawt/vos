#include "devmap.h"
#include "console.h"
#include "cpu.h"
#include "lib.h"
#include "memlayout.h"
#include "multiboot2.h"
#include "pmm.h"
#include "vmm.h"
#include "irq.h"

#define MSR_PAT             0x277
#define MSR_MTRRCAP         0xFE
#define MSR_MTRR_DEF_TYPE   0x2FF
#define MSR_MTRR_PHYSBASE0  0x200
#define PAT_UC              0x00
#define PAT_WC              0x01
#define CPUID1_EDX_MTRR     (1u << 12)
#define CPUID1_EDX_PAT      (1u << 16)

struct devmap_entry {
    uint64_t pa, end;                       /* page-rounded physical range */
    uint64_t va;                            /* virtual address of pa */
    uint64_t bits;                          /* memory-type bits of every leaf */
    const char *what;
};

static struct devmap_entry map[DEVMAP_MAX];
static unsigned count;
static uint64_t next_va = DEVMAP_VA_BASE, used_bytes;
static uint64_t pd_pa;                      /* the window's page directory (PMM frame) */
static const struct mb2_tag_mmap *mmap;
static uint32_t mmap_entries;
static unsigned maxphys = 36;
static int ready, pat_wc;
static uint64_t uc_bits = PTE_PCD | PTE_PWT;

/* Scope ownership survives caller errors/abandoned Rust handles. Newly owned
 * PTs occupy fresh PDEs; shared preexisting tables are never freed. The fixture
 * spans two PDEs; the C1 qualification BAR occupies one. No scoped region
 * publishes memory to a device; the BAR region is read-only by contract. */
static struct {
    uint64_t token, generation, previous_va, va, pa, length;
    uint64_t tables[2];
    unsigned indices[2], ntables, slot, kind;
} scope;
static int scope_budget = -1;

static int scope_context(void)
{
    return (read_cr3() & PTE_ADDR) == vmm_kernel_root() &&
           !(read_rflags() & RFLAGS_IF) && !irq_in_handler();
}

void devmap_scope_budget(int n) { scope_budget = n; }
uint64_t devmap_scope_token(void) { return scope.token; }
unsigned devmap_scope_tables(void) { return scope.ntables; }
unsigned devmap_scope_kind(void) { return scope.token ? scope.kind : 0; }

int devmap_scope_extent(uint64_t token, uint64_t *va, uint64_t *length)
{
    if (!va || !length) return DEVMAP_EINVAL;
    *va = *length = 0;
    if (!token || token != scope.token) return DEVMAP_ESTATE;
    *va = scope.va; *length = scope.length;
    return DEVMAP_OK;
}

static uint64_t page_down(uint64_t x)
{
    return x & ~(PAGE_SIZE - 1);
}

const char *devmap_strerror(int err)
{
    static const char *const s[] = {
        "ok", "invalid request", "address overflow", "beyond the physical address width",
        "range intersects RAM or firmware memory", "overlaps another device mapping", "window full",
        "no frame for a page table", "device window not ready",
    };

    return err >= 0 && err <= DEVMAP_ESTATE ? s[err] : "unknown error";
}

unsigned devmap_maxphys(void)
{
    return maxphys;
}

int devmap_wc_available(void)
{
    return pat_wc;
}

unsigned devmap_count(void)
{
    return count;
}

unsigned devmap_table_frames(uint64_t *pa, unsigned max)
{
    const uint64_t *pd;
    unsigned n = 0;

    if (!ready)
        return 0;
    if (n < max)
        pa[n++] = pd_pa;
    pd = phys_to_virt(pd_pa);
    for (unsigned i = 0; i < 512 && n < max; i++)
        if (pd[i] & PTE_P)
            pa[n++] = pd[i] & PTE_ADDR;
    return n;
}

/* The kernel PDPT (PML4[511]). */
static uint64_t *kernel_pdpt(void)
{
    const uint64_t *l4 = phys_to_virt(vmm_kernel_root());

    return phys_to_virt(l4[511] & PTE_ADDR);
}

int devmap_init(const struct boot_info *bi)
{
    uint32_t a, b, c, d, max_ext;
    uint64_t *pdpt;

    if (ready || !bi || !bi->mmap)
        return DEVMAP_ESTATE;
    mmap = bi->mmap;
    mmap_entries = bi->mmap_entries;
    cpuid(0x80000000, 0, &max_ext, &b, &c, &d);
    if (max_ext >= 0x80000008) {
        cpuid(0x80000008, 0, &a, &b, &c, &d);
        if ((a & 0xFF) >= 32 && (a & 0xFF) <= 52)
            maxphys = a & 0xFF;
    }

    /*
     * PAT: keep entries 0-3 (every existing mapping uses them), check that
     * entry 3 is UC, and make entry 4 write-combining. Entries 4-7 are unused:
     * the VMM audit refuses the PAT bit outside this window.
     */
    cpuid(1, 0, &a, &b, &c, &d);
    if (d & CPUID1_EDX_PAT) {
        uint64_t pat = rdmsr(MSR_PAT), want;

        if (((pat >> 24) & 0xFF) != PAT_UC)
            return DEVMAP_ESTATE;           /* PCD|PWT would not mean UC */
        want = (pat & ~(0xFFULL << 32)) | ((uint64_t)PAT_WC << 32);
        wrmsr(MSR_PAT, want);
        if (rdmsr(MSR_PAT) != want || (rdmsr(MSR_PAT) & 0xFFFFFFFFULL) != (pat & 0xFFFFFFFFULL))
            return DEVMAP_ESTATE;
        pat_wc = 1;
    }

    pdpt = kernel_pdpt();
    if (pdpt[DEVMAP_PDPT_INDEX])
        return DEVMAP_ESTATE;               /* the window must be unused */
    if (pmm_alloc_frame(&pd_pa) != PMM_OK)
        return DEVMAP_ENOMEM;
    pdpt[DEVMAP_PDPT_INDEX] = pd_pa | PTE_P | PTE_W;
    ready = 1;
    return DEVMAP_OK;
}

/* Memory-map entry types that are RAM (or may be): anything but "reserved". */
static int ram_type(uint32_t type)
{
    return type != 2;
}

int devmap_check(uint64_t pa, uint64_t len)
{
    uint64_t first, last;

    if (!ready)
        return DEVMAP_ESTATE;
    if (len == 0)
        return DEVMAP_EINVAL;
    if (pa + len < pa || pa + len > UINT64_MAX - PAGE_SIZE)
        return DEVMAP_EOVERFLOW;
    first = page_down(pa);
    last = page_down(pa + len + PAGE_SIZE - 1);
    if (last > (1ULL << maxphys))
        return DEVMAP_ERANGE;
    if (last - first > DEVMAP_MAX_BYTES)
        return DEVMAP_EFULL;
    for (uint32_t i = 0; i < mmap_entries; i++) {
        const struct mb2_mmap_entry *e =
            (const void *)((const uint8_t *)mmap + sizeof(*mmap) + (uint64_t)i * mmap->entry_size);
        uint64_t end = e->base + e->length;

        if (e->length == 0 || !ram_type(e->type))
            continue;
        if (end < e->base)
            end = UINT64_MAX;
        if (first < end && e->base < last)
            return DEVMAP_ERAM;
    }
    for (unsigned i = 0; i < count; i++)
        if (first < map[i].end && map[i].pa < last)
            return DEVMAP_EOVERLAP;
    return DEVMAP_OK;
}

/* The page-table entry slot for a window address, allocating the page table if needed. */
static uint64_t *pte_slot(uint64_t va, int create)
{
    uint64_t *pd = phys_to_virt(pd_pa), i2 = (va >> 21) & 511, i1 = (va >> 12) & 511, pt;

    if (!(pd[i2] & PTE_P)) {
        if (!create || pmm_alloc_frame(&pt) != PMM_OK)
            return NULL;
        pd[i2] = pt | PTE_P | PTE_W;
    }
    return (uint64_t *)phys_to_virt(pd[i2] & PTE_ADDR) + i1;
}

int devmap_map(uint64_t pa, uint64_t len, int attr, const char *what, void **va, int *used)
{
    int err;

    if (scope.token)
        return DEVMAP_ESTATE;              /* preserve exclusive tail ownership */
    if (!va || (attr != DEVMAP_UC && attr != DEVMAP_WC))
        return DEVMAP_EINVAL;
    if ((err = devmap_check(pa, len)))
        return err;

    uint64_t first = page_down(pa), last = page_down(pa + len + PAGE_SIZE - 1), pages = (last - first) / PAGE_SIZE;
    uint64_t bits = attr == DEVMAP_WC && pat_wc ? PTE_PAT4K : uc_bits;

    if (count >= DEVMAP_MAX || used_bytes + (last - first) > DEVMAP_MAX_BYTES ||
        next_va + (pages + 1) * PAGE_SIZE > DEVMAP_VA_END)
        return DEVMAP_EFULL;
    /* Page tables first, so a failure leaves nothing half mapped. */
    for (uint64_t i = 0; i < pages; i++)
        if (!pte_slot(next_va + i * PAGE_SIZE, 1))
            return DEVMAP_ENOMEM;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t *pte = pte_slot(next_va + i * PAGE_SIZE, 0);

        *pte = (first + i * PAGE_SIZE) | PTE_P | PTE_W | PTE_NX | bits;
        invlpg(next_va + i * PAGE_SIZE);
    }
    map[count] = (struct devmap_entry){ first, last, next_va, bits, what };
    count++;
    *va = (void *)(uintptr_t)(next_va + (pa - first));
    if (used)
        *used = bits == PTE_PAT4K ? DEVMAP_WC : DEVMAP_UC;
    next_va += (pages + 1) * PAGE_SIZE;     /* one unmapped guard page after each mapping */
    used_bytes += last - first;
    return DEVMAP_OK;
}

/* Validate the entire teardown before its first mutation. In this serialized
 * context PMM prechecks make subsequent frees infallible absent corruption;
 * nevertheless every return is checked and identifiers are cleared only after
 * successful free. No table is freed while any leaf still references it. */
int devmap_scope_release(uint64_t token)
{
    if (!scope_context() || !token || token != scope.token)
        return DEVMAP_ESTATE;
    uint64_t *pd = phys_to_virt(pd_pa);
    if (count != scope.slot + 1 || map[scope.slot].va != scope.va ||
        map[scope.slot].pa != scope.pa || map[scope.slot].end != scope.pa + scope.length ||
        devmap_audit())
        return DEVMAP_ESTATE;
    for (unsigned t = 0; t < scope.ntables; t++) {
        if (!scope.tables[t]) continue; /* retained after a hypothetical failed free */
        uint64_t entry = pd[scope.indices[t]];
        if (entry && (entry & ~PTE_A) != (scope.tables[t] | PTE_P | PTE_W))
            return DEVMAP_ESTATE;
        if (pmm_free_check_in(pmm_kernel(), scope.tables[t]) != PMM_OK)
            return DEVMAP_ESTATE;
    }
    for (uint64_t v = scope.va; v < scope.va + scope.length; v += PAGE_SIZE) {
        uint64_t *pte = pte_slot(v, 0);
        if (pte) *pte = 0;
        invlpg(v);
    }
    for (unsigned t = 0; t < scope.ntables; t++) {
        if (!scope.tables[t]) continue;
        pd[scope.indices[t]] = 0;
        if (pmm_free_frame(scope.tables[t]) != PMM_OK)
            return DEVMAP_ESTATE; /* token and all unreleased identifiers retained */
        scope.tables[t] = 0;
    }
    used_bytes -= scope.length;
    next_va = scope.previous_va;
    memset(&map[--count], 0, sizeof(map[0]));
    uint64_t generation = scope.generation;
    memset(&scope, 0, sizeof(scope));
    scope.generation = generation;
    return DEVMAP_OK;
}

/* Common tail of both scoped regions, after region-specific validation and
 * devmap_check. `start` selects fresh PDEs above every existing mapping. */
static int scope_install(unsigned kind, uint64_t pa, uint64_t length, uint64_t start,
                         const char *what, uint64_t *token, uint64_t *va)
{
    if (count == DEVMAP_MAX || scope.generation == UINT64_MAX ||
        used_bytes + length > DEVMAP_MAX_BYTES)
        return DEVMAP_EFULL;
    if (start + length + PAGE_SIZE > DEVMAP_VA_END) return DEVMAP_EFULL;
    uint64_t *pd = phys_to_virt(pd_pa);
    unsigned first = (start >> 21) & 511, last = ((start + length - 1) >> 21) & 511;
    for (unsigned i = first; i <= last; i++) if (pd[i]) return DEVMAP_ESTATE;
    scope.previous_va = next_va;
    scope.va = start; scope.pa = pa; scope.length = length; scope.slot = count;
    scope.kind = kind;
    /* Register before acquisition: even a failed rollback retains identifiers. */
    scope.token = ++scope.generation;
    for (unsigned i = first; i <= last; i++) {
        uint64_t frame = 0;
        if (scope_budget == 0 || pmm_alloc_frame(&frame) != PMM_OK) {
            /* No leaves have been installed. Every owned PT is still empty. */
            for (unsigned t = 0; t < scope.ntables; t++) {
                pd[scope.indices[t]] = 0;
                if (pmm_free_frame(scope.tables[t]) != PMM_OK) {
                    *token = scope.token; return DEVMAP_ESTATE;
                }
                scope.tables[t] = 0;
            }
            uint64_t generation = scope.generation;
            memset(&scope, 0, sizeof(scope)); scope.generation = generation;
            return DEVMAP_ENOMEM;
        }
        if (scope_budget > 0) scope_budget--;
        unsigned t = scope.ntables++;
        scope.tables[t] = frame; scope.indices[t] = i;
        pd[i] = frame | PTE_P | PTE_W;
    }
    for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
        *pte_slot(start + off, 0) = (pa + off) | PTE_P | PTE_W | PTE_NX | uc_bits;
        invlpg(start + off);
    }
    map[count++] = (struct devmap_entry){pa, pa + length, start, uc_bits, what};
    next_va = start + length + PAGE_SIZE; used_bytes += length;
    *token = scope.token; *va = start;
    return DEVMAP_OK;
}

int devmap_scope_acquire(unsigned region, uint64_t offset, uint64_t length,
                         int attr, uint64_t *token, uint64_t *va)
{
    if (!token || !va) return DEVMAP_EINVAL;
    *token = *va = 0;
    if (!scope_context() || !ready || scope.token) return DEVMAP_ESTATE;
    if (region != DEVMAP_SCOPE_FIXTURE || attr != DEVMAP_UC || !length ||
        offset % PAGE_SIZE || length % PAGE_SIZE || offset > 16384 || length > 16384 - offset)
        return DEVMAP_EINVAL;
    uint64_t pa = 0x100000000ULL + offset;
    int err = devmap_check(pa, length);
    if (err) return err; /* includes RAM, aliases, MAXPHYADDR and overflow */
    /* Fresh PTs give exact ownership, independent of framebuffer table sharing.
     * At most two 4 KiB tables, even when the 16 KiB fixture straddles a PDE. */
    uint64_t start = ((next_va + 0x1fffff) & ~0x1fffffULL) + 0x1ff000;
    return scope_install(DEVMAP_SCOPE_FIXTURE, pa, length, start,
                         "USB.2-B no-access fixture", token, va);
}

int devmap_scope_acquire_region(const struct devmap_scope_region *r, int attr,
                                uint64_t *token, uint64_t *va)
{
    if (!token || !va) return DEVMAP_EINVAL;
    *token = *va = 0;
    if (!scope_context() || !ready || scope.token) return DEVMAP_ESTATE;
    /* Only the fixed qualification profile extent, naturally aligned as a
     * PCI BAR of that size must be. No arbitrary length or WC mapping. */
    if (!r || r->kind != DEVMAP_SCOPE_XHCI_BAR || attr != DEVMAP_UC ||
        r->length != DEVMAP_XHCI_BAR_BYTES || !r->pa || r->pa % r->length)
        return DEVMAP_EINVAL;
    int err = devmap_check(r->pa, r->length);
    if (err) return err; /* RAM, existing mappings (USB.1), MAXPHYADDR, overflow */
    /* One fresh 2 MiB-aligned PDE: exactly one owned PT, guard page included. */
    uint64_t start = (next_va + 0x1fffff) & ~0x1fffffULL;
    return scope_install(DEVMAP_SCOPE_XHCI_BAR, r->pa, r->length, start,
                         "USB.2-C1 xHCI BAR read-only qualification", token, va);
}

uint64_t devmap_expected_pte(uint64_t va)
{
    for (unsigned i = 0; i < count; i++)
        if (va >= map[i].va && va < map[i].va + (map[i].end - map[i].pa))
            return (map[i].pa + (va - map[i].va)) | PTE_P | PTE_W | PTE_NX | map[i].bits;
    return 0;
}

const char *devmap_audit(void)
{
    const uint64_t *pd;
    uint64_t present = 0, expected = 0;

    if (!ready)
        return count ? "mappings without a window" : NULL;
    if ((kernel_pdpt()[DEVMAP_PDPT_INDEX] & ~(PTE_A)) != (pd_pa | PTE_P | PTE_W))
        return "device window entry changed";
    pd = phys_to_virt(pd_pa);
    for (unsigned i2 = 0; i2 < 512; i2++) {
        const uint64_t *pt;

        if (!pd[i2])
            continue;
        if ((pd[i2] & ~(PTE_ADDR | PTE_A)) != (PTE_P | PTE_W) || pmm_frame_state_in(pmm_kernel(), pd[i2] & PTE_ADDR) != PMM_FRAME_ALLOCATED)
            return "device window table entry with unexpected bits";
        pt = phys_to_virt(pd[i2] & PTE_ADDR);
        for (unsigned i1 = 0; i1 < 512; i1++) {
            uint64_t va = DEVMAP_VA_BASE + ((uint64_t)i2 << 21) + ((uint64_t)i1 << 12);
            uint64_t want = devmap_expected_pte(va);

            if (!pt[i1])
                continue;
            present++;
            if ((pt[i1] & ~(PTE_A | PTE_D)) != want || !want)
                return "device window page differs from the registry";
        }
    }
    for (unsigned i = 0; i < count; i++) {
        if (map[i].pa >= map[i].end || map[i].va < DEVMAP_VA_BASE || map[i].va >= DEVMAP_VA_END ||
            (map[i].bits != uc_bits && map[i].bits != PTE_PAT4K) || (map[i].bits == PTE_PAT4K && !pat_wc))
            return "device mapping record invalid";
        for (unsigned j = 0; j < i; j++)
            if (map[i].pa < map[j].end && map[j].pa < map[i].end)
                return "device mappings overlap";
        expected += (map[i].end - map[i].pa) / PAGE_SIZE;
    }
    if (present != expected)
        return "device window page count differs from the registry";
    if (pat_wc && (((rdmsr(MSR_PAT) >> 32) & 0xFF) != PAT_WC || ((rdmsr(MSR_PAT) >> 24) & 0xFF) != PAT_UC))
        return "PAT entries changed";
    return NULL;
}

int devmap_mtrr_type(uint64_t pa)
{
    uint32_t a, b, c, d;
    uint64_t def, cap;

    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & CPUID1_EDX_MTRR))
        return -1;
    def = rdmsr(MSR_MTRR_DEF_TYPE);
    if (!(def & (1u << 11)))
        return -1;                          /* MTRRs disabled: everything UC */
    cap = rdmsr(MSR_MTRRCAP);
    int type = -2;
    for (unsigned i = 0; i < (cap & 0xFF) && i < 32; i++) {
        uint64_t base = rdmsr(MSR_MTRR_PHYSBASE0 + 2 * i), mask = rdmsr(MSR_MTRR_PHYSBASE0 + 2 * i + 1);
        uint64_t m = mask & ~0xFFFULL & ((1ULL << maxphys) - 1);

        if (!(mask & (1u << 11)) || (pa & m) != (base & m))
            continue;
        int t = (int)(base & 0xFF);
        if (type == -2 || t == 0 || (t == 4 && type == 6))
            type = t;                       /* overlaps: UC wins, then WT over WB */
    }
    return type == -2 ? (int)(def & 0xFF) : type;
}

#ifdef VOS_TEST
static const struct mb2_tag_mmap *boot_mmap;
static uint32_t boot_entries;

void devmap_test_set_map(const struct mb2_tag_mmap *m, uint32_t entries)
{
    if (!boot_mmap) {
        boot_mmap = mmap;
        boot_entries = mmap_entries;
    }
    mmap = m ? m : boot_mmap;
    mmap_entries = m ? entries : boot_entries;
}

uint64_t devmap_test_pte(uint64_t va)
{
    uint64_t *pte = ready && va >= DEVMAP_VA_BASE && va < DEVMAP_VA_END ? pte_slot(va, 0) : NULL;

    return pte ? *pte : 0;
}

void devmap_test_set_pte(uint64_t va, uint64_t v)
{
    uint64_t *pte = ready && va >= DEVMAP_VA_BASE && va < DEVMAP_VA_END ? pte_slot(va, 0) : NULL;

    if (pte) {
        *pte = v;
        invlpg(va);
    }
}

void devmap_test_rollback(unsigned keep)
{
    while (count > keep) {
        struct devmap_entry *e = &map[--count];

        for (uint64_t va = e->va; va < e->va + (e->end - e->pa); va += PAGE_SIZE) {
            uint64_t *pte = pte_slot(va, 0);

            if (pte)
                *pte = 0;
            invlpg(va);
        }
        used_bytes -= e->end - e->pa;
        next_va = e->va;
        memset(e, 0, sizeof(*e));
    }
}
#endif
