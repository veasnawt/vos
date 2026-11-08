/* R4-M1: the frozen accepted pmm.c (tests/reference/pmm-r4m1.c, every public symbol renamed ref_) against the
 * production pmm_free_check_in, pmm_frame_state_in and pmm_check_in (Rust, kernel/rust/mm/pmm.rs behind
 * arch/x86_64/rust/pmm.rs), on the same instances. Every bitmap lies flush against PROT_NONE guard pages, so any read
 * outside its PMM_WORDS(frames) words faults; "no access" cases point the bitmaps at a PROT_NONE page or NULL.
 *
 * Classes: instances built by the frozen pmm_init_in/alloc/free/reserve from memory maps; uninitialized, failed and
 * other states; the 1 MiB boundary; alignment and range (including 2^64 wrap); free, allocated, protected and
 * double-freed frames; frame and bitmap-word boundaries; counter, hint and bitmap corruption (allocated outside
 * avail, allocatable below 1 MiB, free below the hint, totals, free > usable); early refusal without bitmap access;
 * then seeded instances. Each call's return value must be identical, and neither side may change the instance or its
 * bitmaps.
 *
 * Outside the documented instance bounds (a READY instance whose frames is not a nonzero multiple of 64 at most
 * PMM_MAX_FRAMES, or whose bitmaps are NULL or misaligned — pmm_init_in never produces one), the Rust refuses
 * (ECORRUPT / FRAME_UNAVAILABLE) without bitmap access wherever the C would read a bitmap; there the C is not run
 * on unmapped bitmaps. Where such an instance has real buffers, the C result is still computed and every difference
 * is counted and printed as the documented divergence. Prints a transcript digest so release and sanitizer runs can
 * be compared. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "pmm.h"
#include "multiboot2.h"

int ref_pmm_init_in(struct pmm *, uint64_t *, uint64_t *, uint64_t, const struct boot_info *, const struct resv_table *);
int ref_pmm_alloc_in(struct pmm *, uint64_t *);
int ref_pmm_free_check_in(const struct pmm *, uint64_t);
int ref_pmm_free_in(struct pmm *, uint64_t);
int ref_pmm_reserve_in(struct pmm *, uint64_t);
int ref_pmm_frame_state_in(const struct pmm *, uint64_t);
int ref_pmm_check_in(const struct pmm *);

__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }
/* The frozen pmm_init_in is built with -Dboot_mmap_entry=ref_boot_mmap_entry: entry i of the test map, as the kernel computes it. */
const struct mb2_mmap_entry *ref_boot_mmap_entry(const struct boot_info *bi, uint32_t i)
{ return (const struct mb2_mmap_entry *)((const uint8_t *)bi->mmap + sizeof *bi->mmap + (size_t)i * bi->mmap->entry_size); }

/* ---- transcript, mismatches, coverage ---- */
static uint64_t digest = 1469598103934665603ull, instances, probes, checks, mismatches;
static uint64_t cov_free[11], cov_state[3], cov_check[11];
static uint64_t cov_early, cov_outside, cov_cdefined, divergence, cov_lowmem, cov_wordedge, cov_hint, cov_counter, cov_outside_avail, cov_exhaustive;
static void mix(uint64_t v) { for (int i = 0; i < 8; i++) { digest ^= (v >> (8 * i)) & 0xff; digest *= 1099511628211ull; } }
static void fail(const char *what, uint64_t a, uint64_t b, int r, int c)
{
    if (++mismatches <= 20)
        fprintf(stderr, "MISMATCH %s frames=%#llx phys=%#llx ref=%d rust=%d\n", what, (unsigned long long)a, (unsigned long long)b, r, c);
}
static uint64_t rng = 0x52344d31504d4d31ull;   /* "R4M1PMM1" */
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* ---- guarded memory ---- */
static size_t PG;
static uint64_t *nowhere;           /* a PROT_NONE page: any read faults */
struct region { uint8_t *base; size_t len; };
static uint64_t *guarded(size_t nwords, struct region *r)
{
    size_t body = (nwords * 8 + PG - 1) / PG * PG;
    r->len = body + 2 * PG;
    r->base = mmap(0, r->len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->base == MAP_FAILED || mprotect(r->base, PG, PROT_NONE) || mprotect(r->base + PG + body, PG, PROT_NONE)) abort();
    return (uint64_t *)(r->base + PG + body - nwords * 8);
}
static void release(struct region *r) { if (r->base) munmap(r->base, r->len); r->base = 0; }

struct inst { struct pmm p; struct region ra, rl; uint64_t words; uint64_t *ba, *bl, *sa, *sl; };   /* ba/bl: the guarded buffers */
static void inst_new(struct inst *x, uint64_t frames)
{
    memset(x, 0, sizeof *x);
    x->words = frames / 64;
    x->p.avail = x->ba = guarded(x->words, &x->ra);
    x->p.alloc = x->bl = guarded(x->words, &x->rl);
    x->p.frames = frames;
    x->p.state = PMM_READY;
}
static void inst_free(struct inst *x) { release(&x->ra); release(&x->rl); free(x->sa); free(x->sl); x->sa = x->sl = 0; }
static int bit(const uint64_t *bm, uint64_t f) { return (bm[f / 64] >> (f % 64)) & 1; }
static uint64_t pop(uint64_t v) { uint64_t n = 0; for (; v; v &= v - 1) n++; return n; }
static uint64_t first_free_word(const struct inst *x)
{
    for (uint64_t w = 0; w < x->words; w++) if (x->p.avail[w] & ~x->p.alloc[w]) return w;
    return x->words;
}
/* Counters and hint as the PMM maintains them. */
static void inst_recount(struct inst *x)
{
    uint64_t u = 0, f = 0;
    for (uint64_t w = 0; w < x->words; w++) { u += pop(x->p.avail[w]); f += pop(x->p.avail[w] & ~x->p.alloc[w]); }
    x->p.usable = u; x->p.free = f;
    uint64_t ff = first_free_word(x);
    x->p.hint = ff ? next() % (ff + 1) : 0;
}

/* Read-only: snapshot the instance's bitmaps, compare later (each call is also checked against the struct). */
static void ro_begin(struct inst *x)
{
    free(x->sa); free(x->sl);
    x->sa = malloc(x->words * 8 + 8); x->sl = malloc(x->words * 8 + 8);
    if (!x->sa || !x->sl) abort();
    memcpy(x->sa, x->ba, x->words * 8); memcpy(x->sl, x->bl, x->words * 8);
}
static void ro_end(struct inst *x)
{
    checks++;
    if (memcmp(x->sa, x->ba, x->words * 8) || memcmp(x->sl, x->bl, x->words * 8)) fail("bitmaps changed", x->p.frames, 0, 0, 0);
}

/* ---- one comparison of each function ---- */
static void same_struct(const struct pmm *a, const struct pmm *b, const char *what)
{
    checks++;
    if (memcmp(a, b, sizeof *a)) fail(what, a->frames, 0, 0, 0);
}
static void cmp_free(const struct pmm *p, uint64_t phys)
{
    struct pmm s = *p;
    int r = ref_pmm_free_check_in(p, phys); same_struct(&s, p, "free_check: ref changed the instance");
    int c = pmm_free_check_in(p, phys); same_struct(&s, p, "free_check: rust changed the instance");
    probes++; checks++; mix(1); mix(phys); mix((uint64_t)r);
    if (r != c) fail("free_check", p->frames, phys, r, c);
    if (r >= 0 && r < 11) cov_free[r]++;
}
static void cmp_state(const struct pmm *p, uint64_t phys)
{
    struct pmm s = *p;
    int r = ref_pmm_frame_state_in(p, phys); same_struct(&s, p, "frame_state: ref changed the instance");
    int c = pmm_frame_state_in(p, phys); same_struct(&s, p, "frame_state: rust changed the instance");
    probes++; checks++; mix(2); mix(phys); mix((uint64_t)r);
    if (r != c) fail("frame_state", p->frames, phys, r, c);
    if (r >= 0 && r < 3) cov_state[r]++;
}
static void cmp_check(const struct pmm *p)
{
    struct pmm s = *p;
    int r = ref_pmm_check_in(p); same_struct(&s, p, "check: ref changed the instance");
    int c = pmm_check_in(p); same_struct(&s, p, "check: rust changed the instance");
    probes++; checks++; mix(3); mix(p->frames); mix((uint64_t)r);
    if (r != c) fail("check", p->frames, 0, r, c);
    if (r >= 0 && r < 11) cov_check[r]++;
}
static void cmp_phys(const struct pmm *p, uint64_t phys) { cmp_free(p, phys); cmp_state(p, phys); }

/* Rust alone, outside the documented bounds, where the C would read a bitmap: the refusal, no access. */
static void outside(const struct pmm *p, uint64_t phys, int what)
{
    struct pmm s = *p; int c, want;
    if (what == 0) { c = pmm_free_check_in(p, phys); want = PMM_ECORRUPT; }
    else if (what == 1) { c = pmm_frame_state_in(p, phys); want = PMM_FRAME_UNAVAILABLE; }
    else { c = pmm_check_in(p); want = PMM_ECORRUPT; }
    same_struct(&s, p, "outside: rust changed the instance");
    probes++; checks++; cov_outside++; mix(4); mix(phys); mix((uint64_t)c);
    if (c != want) fail("outside-contract refusal", p->frames, phys, want, c);
}
/* The C reads a bitmap here: a READY instance on which the C, before any bitmap word, has not already decided. */
static int c_reads_free(const struct pmm *p, uint64_t phys)
{ return p->state == PMM_READY && phys % PAGE_SIZE == 0 && phys < p->frames * PAGE_SIZE; }
static int c_reads_state(const struct pmm *p, uint64_t phys)
{ return p->state == PMM_READY && phys / PAGE_SIZE < p->frames; }
static int c_reads_check(const struct pmm *p) { return p->state == PMM_READY && PMM_WORDS(p->frames) != 0; }

/* ---- probes ---- */
static void probe_all(struct inst *x, int exhaustive)
{
    const struct pmm *p = &x->p;
    uint64_t F = p->frames, lim = F * PAGE_SIZE;
    static const uint64_t fixed[] = { 0, 1, 0x7ff, 0xfff, 0x1000, 0x1001, 0xff000, 0xfffff, 0x100000 - 0x1000, 0x100000, 0x100001,
                                      0x100800, 0x101000, 0x1ff000, 0x200000, 0x3ffff000, 0x40000000, 0x40001000,
                                      0x8000000000000000ull, 0xfffffffffffff000ull, 0xffffffffffffffffull, 0xffffffffffffefffull };
    ro_begin(x);
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++) cmp_phys(p, fixed[i]);
    uint64_t edges[] = { lim - 0x1000, lim - 1, lim, lim + 1, lim + 0x1000, lim - 0x800 };
    for (unsigned i = 0; i < 6; i++) cmp_phys(p, edges[i]);
    /* bitmap-word boundaries: frame 64k-1, 64k, 64k+1 (all when small, a sample when large) */
    for (uint64_t k = 1; k <= x->words; k++) {
        if (x->words > 24 && k > 8 && k + 8 < x->words && next() % 64) continue;
        for (int d = -1; d <= 1; d++) { cmp_phys(p, (64 * k + d) * PAGE_SIZE); cov_wordedge++; }
    }
    /* the 1 MiB boundary: frames 255, 256 */
    cmp_phys(p, 255 * PAGE_SIZE); cmp_phys(p, 256 * PAGE_SIZE); cov_lowmem += 2;
    if (exhaustive)
        for (uint64_t f = 0; f < F + 2; f++) { cmp_phys(p, f * PAGE_SIZE); cmp_free(p, f * PAGE_SIZE + 0x800); }
    for (int i = 0; i < 48; i++) { uint64_t f = F ? next() % F : 0; cmp_phys(p, f * PAGE_SIZE); }
    for (int i = 0; i < 8; i++) cmp_phys(p, next());
    cmp_check(p);
    ro_end(x);
}

/* Each corruption alone (then undone), with the probes that see it. */
static void corruptions(struct inst *x)
{
    struct pmm *p = &x->p; struct pmm keep = *p;
    uint64_t W = x->words, ff = first_free_word(x);
    static const int64_t dv[] = { -1, 1 };
    for (int i = 0; i < 2; i++) {
        p->usable = keep.usable + (uint64_t)dv[i]; cmp_check(p); cov_counter++; *p = keep;
        p->free = keep.free + (uint64_t)dv[i]; cmp_check(p); cov_counter++; *p = keep;
    }
    p->free = p->usable; cmp_check(p); cov_counter++;
    for (uint64_t w = 4; w < W; w++) {   /* free == usable with an allocated frame: free_check refuses as corrupt */
        uint64_t a = p->alloc[w];
        if (a) { cmp_free(p, (w * 64 + (uint64_t)__builtin_ctzll(a)) * PAGE_SIZE); break; }
    }
    *p = keep;
    p->free = keep.usable + 1; p->usable = keep.usable; cmp_check(p); cov_counter++; *p = keep;
    p->usable = UINT64_MAX; cmp_check(p); p->free = UINT64_MAX; cmp_check(p); cov_counter += 2; *p = keep;
    p->usable = 0; p->free = 0; cmp_check(p); cov_counter++; *p = keep;
    /* hint: above the first free word, exactly there, at the end, past the end, 2^64 - 1 */
    uint64_t hints[] = { ff + 1, ff, W, W + 1, UINT64_MAX, 0 };
    for (int i = 0; i < 6; i++) { p->hint = hints[i]; cmp_check(p); cov_hint++; }
    *p = keep;
    /* allocated outside avail: a protected frame's alloc bit */
    for (uint64_t f = 0; f < 64 * W; f += 1 + next() % 97) {
        if (bit(p->avail, f) || bit(p->alloc, f)) continue;
        p->alloc[f / 64] |= 1ull << (f % 64);
        cmp_check(p); cmp_phys(p, f * PAGE_SIZE); cov_outside_avail++;
        p->free--; cmp_check(p); p->free = keep.free;
        p->alloc[f / 64] &= ~(1ull << (f % 64));
        break;
    }
    /* allocatable below 1 MiB (word 3, last bit; word 0, first bit), counters adjusted so only that invariant breaks */
    if (W > 4) {
        uint64_t lows[] = { 255, 0, 192 };
        for (int i = 0; i < 3; i++) {
            uint64_t f = lows[i], sa = p->avail[f / 64];
            p->avail[f / 64] |= 1ull << (f % 64); p->usable++; p->free++;
            cmp_check(p); cmp_phys(p, f * PAGE_SIZE); cov_lowmem++;
            p->avail[f / 64] = sa; *p = keep;
        }
        /* frame 256 (word 4) is allowed */
        if (!bit(p->avail, 256)) {
            p->avail[4] |= 1; p->usable++; p->free++; p->hint = 0;
            cmp_check(p); cmp_phys(p, 256 * PAGE_SIZE); cov_lowmem++;
            p->avail[4] &= ~1ull; *p = keep;
        }
    }
    /* an allocated bit with the counters not updated, and a cleared alloc bit (counts say allocated) */
    for (uint64_t f = 256; f < 64 * W; f += 1 + next() % 61) {
        if (!bit(p->avail, f) || bit(p->alloc, f)) continue;
        p->alloc[f / 64] |= 1ull << (f % 64); cmp_check(p); cmp_phys(p, f * PAGE_SIZE);
        p->alloc[f / 64] &= ~(1ull << (f % 64)); cov_counter++; break;
    }
    for (uint64_t f = 256; f < 64 * W; f += 1 + next() % 61) {
        if (!bit(p->alloc, f)) continue;
        p->alloc[f / 64] &= ~(1ull << (f % 64)); cmp_check(p); cmp_phys(p, f * PAGE_SIZE);
        p->alloc[f / 64] |= 1ull << (f % 64); cov_counter++; break;
    }
    *p = keep;
}

/* Other states, frames and pointers that the C refuses before any bitmap read: both run, no access allowed. */
static void early(const struct inst *x)
{
    static const int states[] = { PMM_UNINIT, PMM_FAILED, 3, -1, 0x7fffffff, (int)0x80000000 };
    uint64_t *ptrs[][2] = { { nowhere, nowhere }, { 0, 0 }, { 0, nowhere }, { nowhere, 0 }, { (uint64_t *)((uint8_t *)nowhere + 4), nowhere },
                            { nowhere, (uint64_t *)((uint8_t *)nowhere + 4) } };
    static const uint64_t frs[] = { 0, 1, 63, 64, 65, 512, PMM_MAX_FRAMES, PMM_MAX_FRAMES + 64, 1ull << 52, (1ull << 52) + 64, UINT64_MAX };
    struct pmm p = x->p;
    for (unsigned s = 0; s < sizeof states / sizeof states[0]; s++)
        for (unsigned q = 0; q < 6; q++)
            for (unsigned f = 0; f < sizeof frs / sizeof frs[0]; f++) {
                p.state = states[s]; p.avail = ptrs[q][0]; p.alloc = ptrs[q][1]; p.frames = frs[f];
                p.usable = next() % 3; p.free = next() % 3; p.hint = next() % 3;
                cmp_phys(&p, 0x200000); cmp_phys(&p, 0x200001); cmp_check(&p); cov_early += 5;
            }
    /* READY, bitmaps unmapped/NULL/misaligned: whichever check the C decides without a bitmap word, both run;
     * where the C would read, only the Rust runs and must refuse */
    for (unsigned q = 0; q < 6; q++)
        for (unsigned f = 0; f < sizeof frs / sizeof frs[0]; f++) {
            p.state = PMM_READY; p.avail = ptrs[q][0]; p.alloc = ptrs[q][1]; p.frames = frs[f];
            int ok = frs[f] != 0 && frs[f] % 64 == 0 && frs[f] <= PMM_MAX_FRAMES && q == 0;   /* in bounds but unmapped */
            for (int z = 0; z < 2; z++) {
                p.usable = z ? 5 : 0; p.free = z ? 3 : 0; p.hint = 0;
                uint64_t ph[] = { 0, 0x1000, 0x100000, 0x200000, 0x200001, 0xfff, frs[f] * PAGE_SIZE, frs[f] * PAGE_SIZE - 0x1000,
                                  (frs[f] - 1) * PAGE_SIZE + 1, UINT64_MAX, 0xfffffffffffff000ull, 64 * PAGE_SIZE };
                for (unsigned i = 0; i < sizeof ph / sizeof ph[0]; i++) {
                    if (!c_reads_free(&p, ph[i])) { cmp_free(&p, ph[i]); cov_early++; }
                    else if (!ok) outside(&p, ph[i], 0);
                    if (!c_reads_state(&p, ph[i])) { cmp_state(&p, ph[i]); cov_early++; }
                    else if (!ok) outside(&p, ph[i], 1);
                }
                if (!c_reads_check(&p)) { cmp_check(&p); cov_early++; }
                else if (!ok) outside(&p, 0, 2);
            }
        }
}

/* Outside the bounds but with real, guarded buffers the C can read: the C result is computed and every difference
 * from the Rust refusal is the documented divergence. */
static void outside_with_buffers(void)
{
    static const uint64_t frs[] = { 1, 63, 65, 100, 127, 129, 4095 * 64 + 1, PMM_MAX_FRAMES + 64, PMM_MAX_FRAMES + 1 };
    for (unsigned i = 0; i < sizeof frs / sizeof frs[0]; i++) {
        uint64_t F = frs[i], words = (F + 63) / 64;           /* enough words for every frame below F */
        struct region ra, rl;
        uint64_t *a = guarded(words, &ra), *l = guarded(words, &rl);
        for (uint64_t w = 0; w < words; w++) { a[w] = w < 4 ? 0 : next(); l[w] = a[w] & next(); }
        struct pmm p = { a, l, F, 0, 0, 0, 0, PMM_READY };
        for (uint64_t w = 0; w < PMM_WORDS(F); w++) { p.usable += pop(a[w]); p.free += pop(a[w] & ~l[w]); }
        for (int j = 0; j < 64; j++) {
            uint64_t ph = (j < 8 ? (F - 1 - (uint64_t)j % F) : next() % F) * PAGE_SIZE;
            int r1 = ref_pmm_free_check_in(&p, ph), c1 = pmm_free_check_in(&p, ph);
            int r2 = ref_pmm_frame_state_in(&p, ph), c2 = pmm_frame_state_in(&p, ph);
            cov_cdefined += 2; checks += 2;
            if (c1 != PMM_ECORRUPT) fail("outside (buffers): free_check refusal", F, ph, PMM_ECORRUPT, c1);
            if (c2 != PMM_FRAME_UNAVAILABLE) fail("outside (buffers): frame_state refusal", F, ph, PMM_FRAME_UNAVAILABLE, c2);
            divergence += (r1 != c1) + (r2 != c2);
            mix(5); mix(ph); mix((uint64_t)r1); mix((uint64_t)r2);
        }
        int r3 = ref_pmm_check_in(&p), c3 = pmm_check_in(&p);
        cov_cdefined++; checks++;
        if (PMM_WORDS(F) ? c3 != PMM_ECORRUPT : c3 != r3) fail("outside (buffers): check", F, 0, PMM_ECORRUPT, c3);
        divergence += r3 != c3;
        release(&ra); release(&rl);
    }
    /* misaligned bitmaps over real memory (bounds otherwise valid) */
    struct region ra, rl;
    uint64_t *a = guarded(9, &ra), *l = guarded(9, &rl);
    for (int w = 0; w < 9; w++) { a[w] = w < 5 ? 0 : ~0ull; l[w] = 0; }
    struct pmm p = { (uint64_t *)((uint8_t *)a + 4), (uint64_t *)((uint8_t *)l + 4), 512, 0, 0, 0, 0, PMM_READY };
    for (uint64_t f = 0; f < 512; f += 37) {
        int c1 = pmm_free_check_in(&p, f * PAGE_SIZE), c2 = pmm_frame_state_in(&p, f * PAGE_SIZE);
        cov_outside += 2; checks += 2;
        if (c1 != PMM_ECORRUPT || c2 != PMM_FRAME_UNAVAILABLE) fail("misaligned bitmaps refused", 512, f * PAGE_SIZE, 0, c1);
    }
    checks++; cov_outside++;
    if (pmm_check_in(&p) != PMM_ECORRUPT) fail("misaligned bitmaps: check refused", 512, 0, PMM_ECORRUPT, pmm_check_in(&p));
    release(&ra); release(&rl);
}

/* ---- instances from memory maps, through the frozen init/alloc/free/reserve ---- */
struct map { struct mb2_tag_mmap h; struct mb2_mmap_entry e[8]; };
static void from_map(uint64_t frames, const struct mb2_mmap_entry *e, int n, const struct resv_table *t, int ops)
{
    struct map m; memset(&m, 0, sizeof m);
    m.h.entry_size = sizeof(struct mb2_mmap_entry); m.h.entry_version = 0;
    m.h.tag.size = (uint32_t)(sizeof m.h + (size_t)n * sizeof(struct mb2_mmap_entry));
    memcpy(m.e, e, (size_t)n * sizeof *e);
    struct boot_info bi; memset(&bi, 0, sizeof bi); bi.mmap = &m.h; bi.mmap_entries = (uint32_t)n;
    struct inst x; inst_new(&x, frames);
    int err = ref_pmm_init_in(&x.p, x.p.avail, x.p.alloc, frames, &bi, t);
    instances++; mix(6); mix((uint64_t)err);
    probe_all(&x, frames <= 1024);
    /* allocate, free (incl. double free), reserve with the frozen C, probing as the state evolves */
    uint64_t got[64]; int k = 0;
    for (int i = 0; i < ops && k < 64; i++) {
        uint64_t a; int r = ref_pmm_alloc_in(&x.p, &a); mix(7); mix((uint64_t)r);
        if (r) break;
        got[k++] = a; cmp_phys(&x.p, a);
    }
    for (int i = 0; i < k; i += 2) {
        cmp_free(&x.p, got[i]);
        mix((uint64_t)ref_pmm_free_in(&x.p, got[i]));
        cmp_phys(&x.p, got[i]);     /* double free: ENOTALLOC; state FREE */
    }
    if (x.p.state == PMM_READY) {
        uint64_t f = 300 + next() % 64;
        if (f < frames) { mix((uint64_t)ref_pmm_reserve_in(&x.p, f * PAGE_SIZE)); cmp_phys(&x.p, f * PAGE_SIZE); }
        /* exhaust, then the hint is PMM_WORDS(frames) */
        uint64_t a; for (int i = 0; i < 4096 && ref_pmm_alloc_in(&x.p, &a) == PMM_OK; i++) ;
        cmp_check(&x.p);
        if (k) { ref_pmm_free_in(&x.p, got[1 % k]); cmp_phys(&x.p, got[1 % k]); cmp_check(&x.p); }
    }
    probe_all(&x, frames <= 1024);
    if (x.p.state == PMM_READY) corruptions(&x);
    else cmp_check(&x.p);
    early(&x);
    inst_free(&x);
}

static void deterministic(void)
{
    struct resv_table none; memset(&none, 0, sizeof none);
    struct resv_table t; memset(&t, 0, sizeof t);
    t.r[0].start = 0x200000; t.r[0].end = 0x203001; t.r[1].start = 0x3ff800; t.r[1].end = 0x400800; t.count = 2;
    const struct mb2_mmap_entry ram2[] = { { 0, 0x9f000, 1, 0 }, { 0x100000, 0x100000, 1, 0 } };
    const struct mb2_mmap_entry ram3[] = { { 0x200000, 0x3000, 1, 0 } };
    const struct mb2_mmap_entry holes[] = { { 0, 0xa0000, 1, 0 }, { 0x100000, 0x3ef000, 1, 0 }, { 0x180000, 0x2000, 2, 0 },
                                            { 0x3f0000, 0x10000, 1, 0 }, { 0x400000, 0x40000000, 1, 0 }, { 0x41000000, 0x1000, 3, 0 } };
    const struct mb2_mmap_entry full[] = { { 0, 0x9fc00, 1, 0 }, { 0x9fc00, 0x400, 2, 0 }, { 0xf0000, 0x10000, 2, 0 },
                                           { 0x100000, 0x3fee0000, 1, 0 }, { 0x3ffe0000, 0x20000, 2, 0 }, { 0xfffc0000, 0x40000, 2, 0 } };
    const struct mb2_mmap_entry lowonly[] = { { 0, 0x100000, 1, 0 } };
    const struct mb2_mmap_entry odd[] = { { 0x100800, 0x2800, 1, 0 }, { 0x13f000, 0x2000, 1, 0 }, { 0xffffffffffff0000ull, 0x20000, 2, 0 } };
    /* The frozen pmm_init_in clears frames [0, 256) whatever `frames` is (its step 5), so for frames below 256 it
     * writes past PMM_WORDS(frames) words: init-built instances here use frames >= 256, as every in-tree caller
     * does (PMM_MAX_FRAMES, 16384). Smaller READY instances are built directly (seeded, small frames). */
    from_map(256, lowonly, 1, &none, 4);                         /* every frame below 1 MiB: usable 0 */
    from_map(512, ram3, 1, &none, 8);                            /* the accepted three-frame map */
    from_map(512, ram2, 2, &t, 64);
    from_map(1024, holes, 6, &t, 64);
    from_map(1024, odd, 3, &none, 8);
    from_map(4096, holes, 6, &t, 64);
    from_map(PMM_MAX_FRAMES, full, 6, &t, 64);
    from_map(PMM_MAX_FRAMES, holes, 6, &none, 64);
    /* init refused: the instance is FAILED */
    const struct mb2_mmap_entry bad[] = { { 0x100000, 0x1000, 1, 0 } };
    from_map(64, bad, 0, &none, 1);
    outside_with_buffers();
}

static void seeded(unsigned n)
{
    for (unsigned s = 0; s < n; s++) {
        uint64_t k, pick = next() % 10;
        static const uint64_t small[] = { 1, 2, 3, 4, 5, 8, 16, 17 };
        k = pick < 4 ? small[next() % 8] : pick < 8 ? 1 + next() % 4096 : 4096;
        struct inst x; inst_new(&x, 64 * k);
        for (uint64_t w = 0; w < x.words; w++) {
            uint64_t r = next(), kind = next() % 8;
            uint64_t a = kind < 3 ? ~0ull : kind < 4 ? 0 : kind < 6 ? r : r & next() & next();
            if (w < 4 && next() % 16) a = 0;
            x.p.avail[w] = a;
            uint64_t akind = next() % 6;
            x.p.alloc[w] = next() % 40 == 0 ? next() : akind < 2 ? 0 : akind < 3 ? a : a & next();
        }
        inst_recount(&x);
        if (next() % 5 == 0) {
            switch (next() % 6) {
            case 0: x.p.usable += (next() % 3) - 1; break;
            case 1: x.p.free += (next() % 3) - 1; break;
            case 2: x.p.hint = next() % (x.words + 2); break;
            case 3: x.p.free = x.p.usable + next() % 2; break;
            case 4: x.p.state = (int)(next() % 4); break;
            default: x.p.hint = next(); break;
            }
        }
        instances++;
        ro_begin(&x);
        for (int i = 0; i < 40; i++) {
            uint64_t ph, r = next() % 8, F = x.p.frames;
            if (r < 4) ph = (next() % F) * PAGE_SIZE;
            else if (r < 5) ph = (64 * (1 + next() % x.words) + (next() % 3) - 1) * PAGE_SIZE;
            else if (r < 6) ph = F * PAGE_SIZE + ((next() % 3) - 1) * PAGE_SIZE;
            else if (r < 7) ph = (next() % F) * PAGE_SIZE + 1 + next() % 0xfff;
            else ph = next();
            cmp_phys(&x.p, ph);
        }
        if (x.words <= 16) {    /* 64..1,024 frames: every frame, aligned and mid-frame, and the two past the end */
            for (uint64_t f = 0; f < x.p.frames + 2; f++) { cmp_phys(&x.p, f * PAGE_SIZE); cmp_free(&x.p, f * PAGE_SIZE + 0x800); }
            cov_exhaustive++;
        }
        cmp_check(&x.p);
        ro_end(&x);
        inst_free(&x);
    }
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], 0, 0) : 3000;
    PG = (size_t)sysconf(_SC_PAGESIZE);
    nowhere = mmap(0, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nowhere == MAP_FAILED) abort();
    /* ABI: struct pmm layout as the Rust adapter asserts it */
    if (sizeof(struct pmm) != 64 || offsetof(struct pmm, avail) != 0 || offsetof(struct pmm, alloc) != 8 || offsetof(struct pmm, frames) != 16 ||
        offsetof(struct pmm, usable) != 24 || offsetof(struct pmm, free) != 32 || offsetof(struct pmm, hint) != 40 ||
        offsetof(struct pmm, ignored_bytes) != 48 || offsetof(struct pmm, state) != 56 || _Alignof(struct pmm) != 8) {
        fprintf(stderr, "ABI MISMATCH struct pmm\n"); return 2;
    }
    printf("PMM ABI struct=64 avail=0 alloc=8 frames=16 usable=24 free=32 hint=40 ignored=48 state=56\n");
    deterministic();
    seeded(n);
    printf("PMM DIFFERENTIAL instances=%llu probes=%llu checks=%llu mismatches=%llu digest=%016llx\n", (unsigned long long)instances,
           (unsigned long long)probes, (unsigned long long)checks, (unsigned long long)mismatches, (unsigned long long)digest);
    printf("PMM COVERAGE free=");
    for (int i = 0; i < 11; i++) printf("%s%llu", i ? "," : "", (unsigned long long)cov_free[i]);
    printf(" state=%llu,%llu,%llu check=", (unsigned long long)cov_state[0], (unsigned long long)cov_state[1], (unsigned long long)cov_state[2]);
    for (int i = 0; i < 11; i++) printf("%s%llu", i ? "," : "", (unsigned long long)cov_check[i]);
    printf(" early=%llu outside=%llu outside_c_defined=%llu lowmem=%llu wordedge=%llu hint=%llu counter=%llu alloc_outside_avail=%llu exhaustive=%llu\n",
           (unsigned long long)cov_early, (unsigned long long)cov_outside, (unsigned long long)cov_cdefined, (unsigned long long)cov_lowmem,
           (unsigned long long)cov_wordedge, (unsigned long long)cov_hint, (unsigned long long)cov_counter, (unsigned long long)cov_outside_avail,
           (unsigned long long)cov_exhaustive);
    printf("PMM DOCUMENTED DIVERGENCE outside-contract instances with real buffers: %llu of %llu results differ from the C\n",
           (unsigned long long)divergence, (unsigned long long)cov_cdefined);
    return mismatches != 0;
}
