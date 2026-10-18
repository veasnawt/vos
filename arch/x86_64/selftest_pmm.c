/*
 * Physical allocator self-tests (VOS_TEST only).
 *
 * Expected results come from two sources that do not share code with pmm.c:
 * hand-computed frame counts for each synthetic map, and oracle_free(), a
 * per-frame rule ("wholly inside one available entry, at or above 1 MiB,
 * below the limit, touched by no other entry type and no reservation")
 * that is compared with the allocator's bitmap frame by frame.
 */
#include "selftest.h"
#include "devmap.h"
#include "console.h"
#include "cpu.h"
#include "lib.h"
#include "memlayout.h"
#include "pmm.h"
#include "virtio_blk.h"
#include "vmm.h"

extern char __kernel_phys_start[], __kernel_phys_end[];

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

static int report(const char *name)
{
    serial_puts("selftest: ");
    serial_puts(name);
    serial_puts(" ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

/* ---- independent oracle ---- */

struct pme { uint64_t base, len; uint32_t type; };

static int oracle_free(const struct pme *m, int n, const struct resv_table *t,
                       uint64_t f, uint64_t limit)
{
    uint64_t fe = f + PAGE_SIZE;
    int in_ram = 0;

    if (f < 0x100000 || fe > limit)
        return 0;
    for (int i = 0; i < n; i++) {
        if (m[i].len == 0)
            continue;
        int overflows = m[i].len > UINT64_MAX - m[i].base;
        if (m[i].type == MB2_MMAP_AVAILABLE) {
            if (!overflows && m[i].base <= f && fe <= m[i].base + m[i].len)
                in_ram = 1;
        } else {
            uint64_t last = overflows ? UINT64_MAX : m[i].base + m[i].len;
            if (f < last && m[i].base < fe)
                return 0;
        }
    }
    for (size_t i = 0; t && i < t->count; i++)
        if (f < t->r[i].end && t->r[i].start < fe)
            return 0;
    return in_ram;
}

/* Compare every tracked frame with the oracle; returns the mismatch count. */
/*
 * Compare every frame with the oracle. The nheld frames in held were
 * allocated on purpose before the test (N3.1: the virtio-blk DMA pool): they
 * must be ALLOCATED, and are not counted as free.
 */
static uint64_t oracle_compare(const struct pmm *p, const struct pme *m, int n,
                               const struct resv_table *t, uint64_t *expected_free,
                               const uint64_t *held, unsigned nheld)
{
    uint64_t bad = 0, want = 0, limit = p->frames * PAGE_SIZE;

    for (uint64_t f = 0; f < limit; f += PAGE_SIZE) {
        int o = oracle_free(m, n, t, f, limit);
        int is_free = pmm_frame_state_in(p, f) == PMM_FRAME_FREE;

        for (unsigned i = 0; i < nheld; i++)
            if (held[i] == f) {
                bad += !o || pmm_frame_state_in(p, f) != PMM_FRAME_ALLOCATED;
                o = 0;
            }
        want += o;
        if (o != is_free) {
            if (bad++ == 0) {
                serial_puts("selftest: oracle mismatch at ");
                serial_hex(f);
                serial_puts(o ? " (oracle: free)\r\n" : " (oracle: unavailable)\r\n");
            }
        }
    }
    *expected_free = want;
    return bad;
}

/* ---- synthetic maps, built directly (the parser is tested separately) ---- */

#define T_FRAMES    16384ULL            /* synthetic instances track 64 MiB */
#define T_LIMIT     (T_FRAMES * PAGE_SIZE)

static struct {
    struct mb2_tag_mmap hdr;
    struct mb2_mmap_entry e[16];
} smap __attribute__((aligned(8)));

static struct boot_info sbi;
static uint64_t t_avail[PMM_WORDS(T_FRAMES)], t_alloc[PMM_WORDS(T_FRAMES)];
static uint64_t t_snap[PMM_WORDS(T_FRAMES)];

static void set_map(const struct pme *m, int n, uint32_t version, uint32_t entry_size)
{
    memset(&smap, 0, sizeof(smap));
    memset(&sbi, 0, sizeof(sbi));
    smap.hdr.tag.type = MB2_TAG_MMAP;
    smap.hdr.tag.size = sizeof(smap.hdr) + n * sizeof(smap.e[0]);
    smap.hdr.entry_size = entry_size;
    smap.hdr.entry_version = version;
    for (int i = 0; i < n; i++) {
        smap.e[i].base = m[i].base;
        smap.e[i].length = m[i].len;
        smap.e[i].type = m[i].type;
    }
    sbi.mmap = &smap.hdr;
    sbi.mmap_entries = n;
}

/*
 * Initialize a 64 MiB instance from `m` and `t`; check the result against
 * both the hand-computed count and the oracle.
 */
static void map_case(struct pmm *p, const struct pme *m, int n, const struct resv_table *t,
                     uint64_t want, const char *what)
{
    uint64_t oracle_want = 0;

    set_map(m, n, 0, sizeof(struct mb2_mmap_entry));
    int err = pmm_init_in(p, t_avail, t_alloc, T_FRAMES, &sbi, t);
    uint64_t bad = err == PMM_OK ? oracle_compare(p, m, n, t, &oracle_want, 0, 0) : 1;

    expect(err == PMM_OK && p->free == want && p->usable == want && oracle_want == want &&
           bad == 0 && pmm_check_in(p) == PMM_OK, what);
    if (err != PMM_OK || p->free != want || oracle_want != want) {
        serial_puts("selftest:   got ");
        serial_dec(p->free);
        serial_puts(", oracle ");
        serial_dec(oracle_want);
        serial_puts(", hand-computed ");
        serial_dec(want);
        serial_puts("\r\n");
    }
}

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

static void test_init(void)
{
    struct pmm p;
    struct resv_table t;

    resv_init(&t);

    /* All frames start unavailable: maps without usable RAM above 1 MiB yield nothing. */
    static const struct pme only_reserved[] = { { 0, T_LIMIT, 2 } };
    map_case(&p, only_reserved, N(only_reserved), &t, 0, "only reserved memory: 0 frames");
    static const struct pme only_low[] = { { 0, 0x9FC00, 1 }, { 0xF0000, 0x10000, 1 } };
    map_case(&p, only_low, N(only_low), &t, 0, "RAM only below 1 MiB: 0 frames");

    static const struct pme one[] = { { 0, 0x2000000, 1 } };
    map_case(&p, one, N(one), &t, 7936, "available [0, 32 MiB): 7936 frames above 1 MiB");
    static const struct pme straddle[] = { { 0x80000, 0x100000, 1 } };
    map_case(&p, straddle, N(straddle), &t, 128, "entry straddling 1 MiB: only the upper part");

    static const struct pme unaligned[] = { { 0x100800, 0x3000, 1 } };
    map_case(&p, unaligned, N(unaligned), &t, 2, "unaligned available entry rounds inward");

    static const struct pme partial_res[] = { { 0x200000, 0x100000, 1 }, { 0x280800, 0x100, 2 } };
    map_case(&p, partial_res, N(partial_res), &t, 255, "partial reserved page removes the frame");
    memcpy(t_snap, t_avail, sizeof(t_snap));
    static const struct pme partial_res_rev[] = { { 0x280800, 0x100, 2 }, { 0x200000, 0x100000, 1 } };
    map_case(&p, partial_res_rev, N(partial_res_rev), &t, 255, "same entries in reverse order");
    expect(memcmp(t_snap, t_avail, sizeof(t_snap)) == 0, "entry order does not change the bitmap");

    static const struct pme dup[] = { { 0x200000, 0x100000, 1 }, { 0x200000, 0x100000, 1 } };
    map_case(&p, dup, N(dup), &t, 256, "duplicate available entries count once");

    static const struct pme zero[] = {
        { 0x200000, 0x100000, 1 }, { 0x250000, 0, 2 }, { 0x500000, 0, 1 },
    };
    map_case(&p, zero, N(zero), &t, 256, "zero-length entries have no effect");

    static const struct pme ovf[] = {
        { 0xFFFFFFFFFFFFF000ULL, 0x2000, 1 }, { 0x100000, 0x300000, 1 },
        { 0x300000, 0xFFFFFFFFFFFFF000ULL, 2 },
    };
    map_case(&p, ovf, N(ovf), &t, 512, "overflowing reserved entry protects to 2^64");

    static const struct pme types[] = {
        { 0x200000, 0x100000, 1 }, { 0x210000, 0x1000, 3 }, { 0x220000, 0x10, 4 },
        { 0x230FFF, 2, 5 }, { 0x240000, 0x1000, 99 },
    };
    map_case(&p, types, N(types), &t, 251, "ACPI, NVS, bad and unknown types are unusable");

    static const struct pme high[] = {
        { 0x3F00000, 0x4100000, 1 }, { 0x5000000, 0x1000000, 1 },
    };
    map_case(&p, high, N(high), &t, 256, "RAM at or above the limit is ignored");
    expect(p.ignored_bytes == 0x5000000, "ignored RAM above the limit is counted");

    static const struct pme unsorted[] = {
        { 0x550000, 0x1000, 3 }, { 0x500000, 0x100000, 1 }, { 0x180000, 0x10000, 2 },
        { 0x100000, 0x300000, 1 },
    };
    map_case(&p, unsorted, N(unsorted), &t, 1007, "unsorted, overlapping entries: unusable wins");

    /* Reservations: inside available RAM, overlapping each other, unaligned. */
    static const struct pme ram[] = { { 0x200000, 0x100000, 1 } };
    resv_add(&t, 0x250000, 0x252000, "a");
    resv_add(&t, 0x251000, 0x254000, "b (overlaps a)");
    resv_add(&t, 0x260800, 0x260900, "c (unaligned)");
    map_case(&p, ram, N(ram), &t, 251, "reservations are removed as a union, rounded outward");
    resv_init(&t);

    /* Rejected inputs leave the instance unusable. */
    set_map(ram, 1, 1, sizeof(struct mb2_mmap_entry));
    expect(pmm_init_in(&p, t_avail, t_alloc, T_FRAMES, &sbi, &t) == PMM_EMMAP &&
           p.state == PMM_FAILED, "memory map version 1 refused");
    uint64_t a;
    expect(pmm_alloc_in(&p, &a) == PMM_ENOTREADY && a == 0, "failed instance allocates nothing");
    set_map(ram, 1, 0, 16);
    expect(pmm_init_in(&p, t_avail, t_alloc, T_FRAMES, &sbi, &t) == PMM_EMMAP,
           "entry size 16 refused");
    struct boot_info empty = { 0 };
    expect(pmm_init_in(&p, t_avail, t_alloc, T_FRAMES, &empty, &t) == PMM_EMMAP,
           "missing memory map refused");
    set_map(ram, 1, 0, sizeof(struct mb2_mmap_entry));
    expect(pmm_init_in(&p, t_avail, t_alloc, 100, &sbi, &t) == PMM_EINVAL &&
           pmm_init_in(&p, t_avail, t_alloc, 0, &sbi, &t) == PMM_EINVAL &&
           pmm_init_in(&p, t_avail, t_alloc, PMM_MAX_FRAMES + 64, &sbi, &t) == PMM_EINVAL,
           "invalid frame counts refused");
}

/* Snapshot of everything a failed operation must not change. */
struct snap { uint64_t free, usable, hint; int state; };

static struct snap take(const struct pmm *p)
{
    struct snap s = { p->free, p->usable, p->hint, p->state };
    return s;
}

static int same(const struct pmm *p, struct snap s)
{
    return p->free == s.free && p->usable == s.usable && p->hint == s.hint && p->state == s.state;
}

static void test_alloc_free(void)
{
    struct pmm p, uninit = { 0 };
    struct resv_table t;
    uint64_t a, b, c, d;

    expect(pmm_alloc_in(&uninit, &a) == PMM_ENOTREADY && a == 0 &&
           pmm_free_in(&uninit, 0x200000) == PMM_ENOTREADY, "uninitialized instance refuses");

    resv_init(&t);
    static const struct pme three[] = { { 0x200000, 0x3000, 1 } };
    map_case(&p, three, N(three), &t, 3, "three-frame map");

    expect(pmm_alloc_in(&p, &a) == PMM_OK && pmm_alloc_in(&p, &b) == PMM_OK &&
           pmm_alloc_in(&p, &c) == PMM_OK, "three allocations succeed");
    expect(a == 0x200000 && b == 0x201000 && c == 0x202000, "first-fit, aligned, distinct");
    expect(p.free == 0 && pmm_check_in(&p) == PMM_OK, "free count reaches 0");

    memcpy(t_snap, t_alloc, sizeof(t_snap));
    struct snap s = take(&p);
    expect(pmm_alloc_in(&p, &d) == PMM_ENOMEM && d == 0, "exhausted: ENOMEM and address 0");
    expect(pmm_alloc_in(&p, &d) == PMM_ENOMEM && d == 0, "exhausted again: still ENOMEM");
    expect(p.free == 0 && p.usable == s.usable && p.state == PMM_READY &&
           memcmp(t_snap, t_alloc, sizeof(t_snap)) == 0 && pmm_check_in(&p) == PMM_OK,
           "exhaustion changes no frame state");

    expect(pmm_free_in(&p, b) == PMM_OK && p.free == 1, "valid free");
    expect(pmm_alloc_in(&p, &d) == PMM_OK && d == b, "freed frame is reused (hint moved back)");
    expect(pmm_free_in(&p, b) == PMM_OK, "free it again after reallocation");

    /* Invalid frees: each must fail and change nothing. */
    static const struct { uint64_t addr; int err; const char *what; } bad[] = {
        { 0x201001, PMM_EALIGN, "unaligned free rejected" },
        { 0x200800, PMM_EALIGN, "mid-frame free rejected" },
        { T_LIMIT, PMM_ERANGE, "free at the limit rejected" },
        { 0xFFFFFFFFFFFFF000ULL, PMM_ERANGE, "free near 2^64 rejected" },
        { 0, PMM_EPROTECTED, "free of frame 0 rejected" },
        { 0x100000, PMM_EPROTECTED, "free of a never-usable frame rejected" },
        { 0x203000, PMM_EPROTECTED, "free just past the RAM rejected" },
        { 0x201000, PMM_ENOTALLOC, "double free rejected" },
    };
    for (unsigned i = 0; i < N(bad); i++) {
        memcpy(t_snap, t_alloc, sizeof(t_snap));
        s = take(&p);
        int err = pmm_free_in(&p, bad[i].addr);
        expect(err == bad[i].err && same(&p, s) &&
               memcmp(t_snap, t_alloc, sizeof(t_snap)) == 0 && pmm_check_in(&p) == PMM_OK,
               bad[i].what);
    }

    expect(pmm_free_in(&p, a) == PMM_OK && pmm_free_in(&p, c) == PMM_OK && p.free == 3 &&
           pmm_check_in(&p) == PMM_OK, "all frames returned");
}

int selftest_pmm_synthetic(void)
{
    failures = cases = 0;
    test_init();
    test_alloc_free();
    return report("pmm-synthetic");
}

/* ---- the kernel allocator, on the real memory map ---- */

#define MAX_REAL_ENTRIES 64
#define N_ALLOC 1000

static struct pme real_map[MAX_REAL_ENTRIES];
static int real_n;
static uint64_t alloc_snapshot[PMM_WORDS(PMM_MAX_FRAMES)];
static uint64_t frames[N_ALLOC];

/* Independent of the reservation table: kernel, info, modules, low 1 MiB, 1 GiB. */
static int hits_known_protected(const struct boot_info *bi, uint64_t f)
{
    uint64_t fe = f + PAGE_SIZE;
    uint64_t ks = (uint64_t)__kernel_phys_start & ~(PAGE_SIZE - 1);
    uint64_t ke = (uint64_t)__kernel_phys_end;

    if (f < 0x100000 || fe > PMM_LIMIT)
        return 1;
    if (f < ke && ks < fe)
        return 1;
    if (f < bi->info_end && bi->info_start < fe)
        return 1;
    for (uint32_t i = 0; i < bi->module_count; i++)
        if (f < bi->modules[i].end && bi->modules[i].start < fe)
            return 1;
    return 0;
}

static int all_bytes(uint64_t phys, uint8_t v)
{
    const volatile uint8_t *b = phys_to_virt(phys);

    for (uint64_t i = 0; i < PAGE_SIZE; i++)
        if (b[i] != v)
            return 0;
    return 1;
}

/* A protected frame is unavailable and cannot be freed (and the attempt changes nothing). */
static void expect_protected(const struct pmm *k, uint64_t phys, const char *what)
{
    phys &= ~(PAGE_SIZE - 1);
    struct snap s = take(k);
    int state = pmm_frame_state_in(k, phys);
    int err = pmm_free_frame(phys);

    expect(state == PMM_FRAME_UNAVAILABLE && (err == PMM_EPROTECTED || err == PMM_ERANGE) &&
           same(k, s), what);
}

struct dtr { uint16_t limit; uint64_t base; } __attribute__((packed));

int selftest_pmm_kernel(const struct boot_info *bi, const struct resv_table *t)
{
    const struct pmm *k = pmm_kernel();
    uint64_t want = 0, a, b;

    failures = cases = 0;
    expect(k->state == PMM_READY && k->frames == PMM_MAX_FRAMES && pmm_check_in(k) == PMM_OK,
           "kernel allocator ready, 262144 frames, invariants hold");
    expect(pmm_init(bi, t) == PMM_EINVAL && k->state == PMM_READY, "second pmm_init refused");

    /* Whole-bitmap comparison with the oracle on the real map and reservations. */
    real_n = bi->mmap_entries;
    expect(real_n <= MAX_REAL_ENTRIES, "memory map fits the test copy");
    if (real_n > MAX_REAL_ENTRIES)
        real_n = MAX_REAL_ENTRIES;
    for (int i = 0; i < real_n; i++) {
        const struct mb2_mmap_entry *e = boot_mmap_entry(bi, i);
        real_map[i].base = e->base;
        real_map[i].len = e->length;
        real_map[i].type = e->type;
    }
    /*
     * Frames held since boot: the virtio-blk DMA pool while a device holds it
     * (N3.1), the block cache while a device is registered (N3.2), and the
     * device window's page tables (U1).
     */
    const struct vblk_info *vi = vblk_info();
    static uint64_t held[VBLK_POOL_FRAMES + BLK_CACHE_LINES + DEVMAP_MAX_TABLES];
    unsigned nheld = 0;
    for (unsigned i = 0; vi->pool_held && i < VBLK_POOL_FRAMES; i++)
        held[nheld++] = vi->pool[i];
    nheld += blk_cache_frames(held + nheld, BLK_CACHE_LINES);
    nheld += devmap_table_frames(held + nheld, DEVMAP_MAX_TABLES);
    uint64_t bad = oracle_compare(k, real_map, real_n, t, &want, held, nheld);
    expect(bad == 0 && want == k->free && k->free == k->usable - nheld,
           "every frame matches the oracle; free count equals the oracle's count");
    uint64_t ignored = 0;
    for (int i = 0; i < real_n; i++) {
        const struct pme *m = &real_map[i];
        if (m->type == MB2_MMAP_AVAILABLE && m->len <= UINT64_MAX - m->base &&
            m->base + m->len > PMM_LIMIT)
            ignored += m->base + m->len - (m->base > PMM_LIMIT ? m->base : PMM_LIMIT);
    }
    expect(k->ignored_bytes == ignored, "RAM above 1 GiB is reported as ignored");
    expect(pmm_frame_state_in(k, PMM_LIMIT) == PMM_FRAME_UNAVAILABLE &&
           pmm_frame_state_in(k, PMM_LIMIT - PAGE_SIZE) != PMM_FRAME_ALLOCATED,
           "nothing at or above 1 GiB is tracked");

    /* Boot-critical frames, located independently of the reservation table. */
    uint64_t cr3 = read_cr3() & ~(PAGE_SIZE - 1);
    const uint64_t *pml4 = phys_to_virt(cr3);
    uint64_t pdpt_hi = pml4[511] & 0x000FFFFFFFFFF000ULL;
    uint64_t pdpt_lo = vmm_test_identity_pdpt();        /* PML4[0] is cleared by now */
    uint64_t pd = ((const uint64_t *)phys_to_virt(pdpt_hi))[510] & 0x000FFFFFFFFFF000ULL;
    uint64_t rsp;
    struct dtr gdtr, idtr;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    __asm__ volatile ("sidt %0" : "=m"(idtr));

    expect_protected(k, 0, "frame 0 protected");
    expect_protected(k, 0xB8000, "VGA text frame protected");
    expect_protected(k, (uint64_t)__kernel_phys_start, "first kernel frame protected");
    expect_protected(k, (uint64_t)__kernel_phys_end - 1, "last kernel frame protected");
    expect_protected(k, cr3, "boot PML4 (CR3) protected");
    expect_protected(k, pdpt_hi, "boot higher-half PDPT protected");
    expect(pdpt_lo != 0, "boot identity PDPT known");
    expect_protected(k, pdpt_lo, "boot identity PDPT protected");
    expect_protected(k, pd, "kernel page directory protected");
    expect_protected(k, ((const uint64_t *)phys_to_virt(pd))[0] & 0x000FFFFFFFFFF000ULL,
                     "kernel page table (phys 0-2 MiB) protected");
    expect_protected(k, ((const uint64_t *)phys_to_virt(pd))[1] & 0x000FFFFFFFFFF000ULL,
                     "kernel page table (phys 2-4 MiB) protected");
    expect_protected(k, rsp - KERNEL_VMA, "current kernel stack protected");
    expect_protected(k, gdtr.base - KERNEL_VMA, "GDT protected");
    expect_protected(k, idtr.base - KERNEL_VMA, "IDT protected");
    expect_protected(k, (uint64_t)k->avail - KERNEL_VMA, "avail bitmap protected");
    expect_protected(k, (uint64_t)k->avail - KERNEL_VMA + sizeof(alloc_snapshot) - 1,
                     "avail bitmap end protected");
    expect_protected(k, (uint64_t)k->alloc - KERNEL_VMA, "alloc bitmap protected");
    expect_protected(k, (uint64_t)k->alloc - KERNEL_VMA + sizeof(alloc_snapshot) - 1,
                     "alloc bitmap end protected");
    expect_protected(k, (uint64_t)k - KERNEL_VMA, "allocator state protected");
    expect_protected(k, bi->info_start, "multiboot2 info start protected");
    expect_protected(k, bi->info_end - 1, "multiboot2 info end protected");
    for (uint32_t i = 0; i < bi->module_count; i++) {
        expect_protected(k, bi->modules[i].start, "module start protected");
        expect_protected(k, bi->modules[i].end - 1, "module end protected");
    }

    memcpy(alloc_snapshot, k->alloc, sizeof(alloc_snapshot));
    uint64_t free0 = k->free;

    /* One frame: aligned, permitted, zeroed. Then reuse proves zeroing over poison. */
    expect(pmm_alloc_frame(&a) == PMM_OK && a % PAGE_SIZE == 0 && a >= 0x100000 &&
           a < PMM_LIMIT && !hits_known_protected(bi, a) &&
           oracle_free(real_map, real_n, t, a, PMM_LIMIT), "allocation is aligned and permitted");
    expect(all_bytes(a, 0), "new frame is zeroed");
    expect(pmm_frame_state_in(k, a) == PMM_FRAME_ALLOCATED && k->free == free0 - 1,
           "frame marked allocated, free count -1");
    serial_puts("selftest: first allocation at ");
    serial_hex(a);
    serial_puts("\r\n");
    memset(phys_to_virt(a), 0x5A, PAGE_SIZE);
    expect(pmm_free_frame(a) == PMM_OK && k->free == free0, "frame released");
    expect(all_bytes(a, PMM_POISON), "released frame is poisoned");
    expect(pmm_alloc_frame(&b) == PMM_OK && b == a && all_bytes(b, 0),
           "reallocated frame is zeroed again (not left poisoned)");

    /* Double free and other invalid frees on the real allocator. */
    expect(pmm_free_frame(b) == PMM_OK, "free before double free");
    static const struct { uint64_t off; int err; const char *what; } bad_free[] = {
        { 0, PMM_ENOTALLOC, "kernel: double free rejected" },
        { 1, PMM_EALIGN, "kernel: unaligned free rejected" },
        { 0x800, PMM_EALIGN, "kernel: mid-frame free rejected" },
    };
    for (unsigned i = 0; i < N(bad_free); i++) {
        struct snap s = take(k);
        int err = pmm_free_frame(b + bad_free[i].off);
        expect(err == bad_free[i].err && same(k, s) &&
               memcmp(alloc_snapshot, k->alloc, sizeof(alloc_snapshot)) == 0, bad_free[i].what);
    }
    struct snap s = take(k);
    expect(pmm_free_frame(PMM_LIMIT) == PMM_ERANGE && pmm_free_frame(0xFFFFFFFFFFFFF000ULL) ==
           PMM_ERANGE && same(k, s), "kernel: out-of-range frees rejected");

    /* Two live allocations differ. */
    expect(pmm_alloc_frame(&a) == PMM_OK && pmm_alloc_frame(&b) == PMM_OK && a != b,
           "two live allocations differ");
    expect(pmm_free_frame(a) == PMM_OK && pmm_free_frame(b) == PMM_OK, "both released");

    /* 1000 live allocations: unique, permitted, zeroed. */
    int ok_alloc = 1, ok_perm = 1, ok_zero = 1, ok_unique = 1;
    for (int i = 0; i < N_ALLOC; i++) {
        if (pmm_alloc_frame(&frames[i]) != PMM_OK) {
            ok_alloc = 0;
            break;
        }
        uint64_t f = frames[i];
        if (f % PAGE_SIZE || hits_known_protected(bi, f) ||
            !oracle_free(real_map, real_n, t, f, PMM_LIMIT))
            ok_perm = 0;
        if (!all_bytes(f, 0))
            ok_zero = 0;
        memset(phys_to_virt(f), 0xC3, PAGE_SIZE);       /* dirty it for the next round */
    }
    for (int i = 0; ok_alloc && i < N_ALLOC; i++)
        for (int j = i + 1; j < N_ALLOC; j++)
            if (frames[i] == frames[j])
                ok_unique = 0;
    expect(ok_alloc, "1000 allocations succeed");
    expect(ok_perm, "1000 allocations avoid every protected or unusable frame");
    expect(ok_zero, "1000 allocations are zeroed");
    expect(ok_unique, "1000 allocations are unique");
    expect(k->free == free0 - N_ALLOC && pmm_check_in(k) == PMM_OK,
           "free count dropped by exactly 1000, invariants hold");
    serial_puts("selftest: 1000 frames from ");
    serial_hex(frames[0]);
    serial_puts(" to ");
    serial_hex(frames[N_ALLOC - 1]);
    serial_puts("\r\n");

    int ok_free = 1;
    for (int i = N_ALLOC - 1; ok_alloc && i >= 0; i--)
        if (pmm_free_frame(frames[i]) != PMM_OK)
            ok_free = 0;
    expect(ok_free && k->free == free0 &&
           memcmp(alloc_snapshot, k->alloc, sizeof(alloc_snapshot)) == 0 &&
           pmm_check_in(k) == PMM_OK, "releasing all 1000 restores the exact state");

    serial_puts("selftest: pmm free frames ");
    serial_dec(k->free);
    serial_puts(", oracle ");
    serial_dec(want);
    serial_puts(", ignored above 1 GiB ");
    serial_dec(k->ignored_bytes);
    serial_puts("\r\n");
    return report("pmm-kernel");
}

/* Isolated (VOS_TEST_CASE=pmm-exhaust): drain the kernel allocator, then restore it. */
int selftest_pmm_exhaust(const struct boot_info *bi)
{
    const struct pmm *k = pmm_kernel();
    uint64_t f, n = 0, free0 = k->free;
    int err, ok_perm = 1;

    failures = cases = 0;
    memcpy(alloc_snapshot, k->alloc, sizeof(alloc_snapshot));
    while ((err = pmm_alloc_frame(&f)) == PMM_OK && n <= PMM_MAX_FRAMES) {
        if (hits_known_protected(bi, f))
            ok_perm = 0;
        n++;
    }
    expect(err == PMM_ENOMEM && f == 0, "allocation fails with ENOMEM and address 0");
    expect(n == free0 && k->free == 0, "exactly the free frames were handed out");
    expect(ok_perm, "no drained frame is protected");
    expect(pmm_alloc_frame(&f) == PMM_ENOMEM && f == 0 && pmm_check_in(k) == PMM_OK,
           "a further allocation fails cleanly; invariants hold");

    uint64_t back = 0;
    int ok_free = 1;
    for (uint64_t fr = 0; fr < PMM_MAX_FRAMES; fr++) {
        uint64_t phys = fr * PAGE_SIZE;
        int was = (alloc_snapshot[fr / 64] >> (fr % 64)) & 1;
        if (!was && pmm_frame_state_in(k, phys) == PMM_FRAME_ALLOCATED) {
            if (pmm_free_frame(phys) != PMM_OK)
                ok_free = 0;
            back++;
        }
    }
    expect(ok_free && back == n && k->free == free0 &&
           memcmp(alloc_snapshot, k->alloc, sizeof(alloc_snapshot)) == 0 &&
           pmm_check_in(k) == PMM_OK, "every drained frame returned; state restored");
    serial_puts("selftest: drained ");
    serial_dec(n);
    serial_puts(" frames\r\n");
    return report("pmm-exhaust");
}
