/* R4-M2: the frozen accepted pmm.c (tests/reference/pmm-r4m2.c, the hardened main; public symbols renamed ref_ except the
 * R4-M1 checks, which both sides take from the production Rust) against the production Rust pmm_alloc_in, pmm_free_in and
 * pmm_reserve_in, as whole-instance state machines.
 *
 * Each side has its own arena, flush against PROT_NONE guard pages, holding the struct pmm, both bitmaps and the output word
 * in one of several layouts: separate regions; identical bitmaps; bitmaps overlapping by whole words; a bitmap lying on the
 * instance's uint64_t fields; the output word on an instance field or a bitmap word. Every layout here is defined C (only
 * uint64_t objects alias each other; the pointer fields and the int state are never overlapped). Both sides start
 * byte-identical (pointers relative to their own arena) and get the same operation and corruption sequence: allocation,
 * freeing (valid, double, unaligned, out of range, protected), reservation, exhaustion and reuse, counter/hint/state/bitmap
 * corruption. After every operation the return value, the output word and every byte of the arena must be identical.
 *
 * Outside the documented storage (NULL or misaligned bitmaps, or a word at or past PMM_WORDS(frames)), where the C would
 * access memory it does not own, only the Rust runs, and it must refuse (ECORRUPT) without touching anything past what the
 * C would already have touched. Prints a transcript digest so release and sanitizer runs can be compared. */
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
int ref_pmm_free_in(struct pmm *, uint64_t);
int ref_pmm_reserve_in(struct pmm *, uint64_t);
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }
const struct mb2_mmap_entry *ref_boot_mmap_entry(const struct boot_info *bi, uint32_t i)
{ return (const struct mb2_mmap_entry *)((const uint8_t *)bi->mmap + sizeof *bi->mmap + (size_t)i * bi->mmap->entry_size); }

static uint64_t digest = 1469598103934665603ull, ops, checks, mismatches, rng = 0x52344d3254524e31ull;   /* "R4M2TRN1" */
static uint64_t cov[3][11], cov_layout[8], cov_failed, cov_exhaust, cov_reuse, cov_refusal, cov_phys_alias, cov_nonmultiple, cov_over_max;
static void mix(uint64_t v) { for (int i = 0; i < 8; i++) { digest ^= (v >> (8 * i)) & 0xff; digest *= 1099511628211ull; } }
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void fail(const char *what, int layout, uint64_t a, uint64_t b)
{
    if (++mismatches <= 20) fprintf(stderr, "MISMATCH %s layout=%d %#llx %#llx\n", what, layout, (unsigned long long)a, (unsigned long long)b);
}

/* ---- arenas ---- */
static size_t PG;
struct arena { uint8_t *map, *base; size_t len, size; };
static void arena_new(struct arena *a, size_t size)
{
    a->size = (size + 7) & ~(size_t)7;
    size_t body = (a->size + PG - 1) / PG * PG;
    a->len = body + 2 * PG;
    a->map = mmap(0, a->len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a->map == MAP_FAILED || mprotect(a->map, PG, PROT_NONE) || mprotect(a->map + PG + body, PG, PROT_NONE)) abort();
    a->base = a->map + PG + body - a->size;      /* flush against the trailing guard */
    memset(a->base, 0, a->size);
}
static void arena_free(struct arena *a) { munmap(a->map, a->len); }

/* A world: one side's arena with the offsets of its objects (the same on both sides). */
struct layout { size_t size, S, A, L, P; uint64_t frames; int kind; };
struct world { struct arena ar; struct pmm *p; uint64_t *phys; };
static void world_new(struct world *w, const struct layout *l)
{
    arena_new(&w->ar, l->size);
    w->p = (struct pmm *)(w->ar.base + l->S); w->phys = (uint64_t *)(w->ar.base + l->P);
}
static void world_pointers(struct world *w, const struct layout *l)
{
    w->p->avail = l->A == (size_t)-1 ? 0 : (uint64_t *)(w->ar.base + l->A);
    w->p->alloc = l->L == (size_t)-1 ? 0 : (uint64_t *)(w->ar.base + l->L);
}
/* Copy every byte of `from` to `to`, then re-point `to`'s bitmaps into its own arena. */
static void world_sync(struct world *to, const struct world *from, const struct layout *l)
{ memcpy(to->ar.base, from->ar.base, l->size); world_pointers(to, l); }
/* Compare every byte except the two pointer fields, which must point to the same offsets. */
static void world_compare(const struct world *a, const struct world *b, const struct layout *l, const char *what)
{
    checks++;
    size_t pf = l->S, pe = l->S + 16;
    for (size_t i = 0; i < l->size; i++) {
        if (i >= pf && i < pe) continue;
        if (a->ar.base[i] != b->ar.base[i]) { fail(what, l->kind, i, ((uint64_t)a->ar.base[i] << 8) | b->ar.base[i]); return; }
    }
    if ((uint8_t *)a->p->avail - a->ar.base != (uint8_t *)b->p->avail - b->ar.base || (uint8_t *)a->p->alloc - a->ar.base != (uint8_t *)b->p->alloc - b->ar.base)
        fail("bitmap pointers moved", l->kind, 0, 0);
    for (size_t i = 0; i < l->size; i += 8) { uint64_t v; memcpy(&v, a->ar.base + i, 8); if (i < pf || i >= pe) mix(v); }
}

/* ---- one operation on both sides ---- */
enum { OP_ALLOC, OP_FREE, OP_RESERVE };
static uint64_t cov_outside_storage;
static void op(struct world *r, struct world *c, const struct layout *l, int kind, uint64_t phys)
{
    int a, b;
    /* reserve of a frame whose word lies at or past PMM_WORDS(frames) (frames not a multiple of 64): the C would read (and
     * could write) outside the instance's documented storage; only the Rust runs, and it must refuse without any write */
    if (kind == OP_RESERVE && c->p->state == PMM_READY && phys % PAGE_SIZE == 0 && phys < c->p->frames * PAGE_SIZE && phys / PAGE_SIZE / 64 >= c->p->frames / 64) {
        b = pmm_reserve_in(c->p, phys); ops++; checks += 2; cov_outside_storage++; mix(9); mix(phys);
        if (b != PMM_ECORRUPT) fail("outside-storage reserve refusal", l->kind, (uint64_t)b, phys);
        world_compare(r, c, l, "outside-storage reserve wrote");
        return;
    }
    if (kind == OP_ALLOC) { a = ref_pmm_alloc_in(r->p, r->phys); b = pmm_alloc_in(c->p, c->phys); }
    else if (kind == OP_FREE) { a = ref_pmm_free_in(r->p, phys); b = pmm_free_in(c->p, phys); }
    else { a = ref_pmm_reserve_in(r->p, phys); b = pmm_reserve_in(c->p, phys); }
    ops++; checks++; mix((uint64_t)kind); mix(phys); mix((uint64_t)a);
    if (a != b) fail(kind == OP_ALLOC ? "alloc result" : kind == OP_FREE ? "free result" : "reserve result", l->kind, (uint64_t)a, (uint64_t)b);
    world_compare(r, c, l, kind == OP_ALLOC ? "alloc state" : kind == OP_FREE ? "free state" : "reserve state");
    if (a >= 0 && a < 11) cov[kind][a]++;
    if (kind == OP_ALLOC && a == PMM_ECORRUPT && r->p->state == PMM_FAILED) cov_failed++;
    if (kind == OP_ALLOC && a == PMM_ENOMEM) cov_exhaust++;
}
/* The same corruption on both sides (a write at an arena offset). */
static void poke(struct world *r, struct world *c, const struct layout *l, size_t off, uint64_t v)
{
    if (off + 8 > l->size || (off >= l->S && off < l->S + 16)) return;   /* never the pointer fields */
    memcpy(r->ar.base + off, &v, 8); memcpy(c->ar.base + off, &v, 8);
}
static void poke32(struct world *r, struct world *c, size_t off, int v) { memcpy(r->ar.base + off, &v, 4); memcpy(c->ar.base + off, &v, 4); }

/* ---- a sequence ---- */
static uint64_t pick_phys(const struct world *r, uint64_t frames, uint64_t *mine, int n)
{
    uint64_t k = next() % 12;
    if (n && k < 5) return mine[next() % (uint64_t)n];                    /* something allocated (or freed since: double free) */
    if (k < 6) { uint64_t f = next() % (frames ? frames : 1); uint64_t off = next() % 2 ? 0 : 1 + next() % 0xfff; return f * PAGE_SIZE + off; }
    if (k < 7) return frames * PAGE_SIZE + (next() % 3) * PAGE_SIZE;        /* at/after the limit */
    if (k < 8) return (next() % 256) * PAGE_SIZE;                           /* below 1 MiB (protected) */
    if (k < 9) return next();                                               /* anywhere in 2^64 */
    if (k < 10) { uint64_t w = 1 + next() % (frames / 64 ? frames / 64 : 1); return (64 * w - (next() % 2)) * PAGE_SIZE; }   /* word edges */
    if (k < 11) return (next() % (frames ? frames : 1)) * PAGE_SIZE + PAGE_SIZE / 2;   /* half-page: catches any weaker alignment test */
    (void)r; return (next() % (frames ? frames : 1)) * PAGE_SIZE;
}
static void sequence(struct world *r, struct world *c, const struct layout *l, int steps)
{
    uint64_t mine[256]; int n = 0;
    for (int i = 0; i < steps; i++) {
        uint64_t k = next() % 20, frames = r->p->frames;
        if (k < 8) {
            op(r, c, l, OP_ALLOC, 0);
            if (*r->phys && n < 256) mine[n++] = *r->phys;
        } else if (k < 13) {
            uint64_t ph = pick_phys(r, frames, mine, n);
            op(r, c, l, OP_FREE, ph);
            if (k == 12 && n) { cov_reuse++; op(r, c, l, OP_ALLOC, 0); }      /* reuse: the freed frame comes back first */
        } else if (k < 16) {
            op(r, c, l, OP_RESERVE, pick_phys(r, frames, mine, n));
        } else if (k < 17) {                                                 /* exhaustion, then give some back */
            for (int j = 0; j < 4200 && r->p->state == PMM_READY; j++) {
                op(r, c, l, OP_ALLOC, 0);
                if (j % 97 == 0 && *r->phys && n < 256) mine[n++] = *r->phys;
                if (*r->phys == 0) break;
            }
            for (int j = 0; j < n; j += 2) op(r, c, l, OP_FREE, mine[j]);
        } else {                                                             /* a corruption, identical on both sides */
            uint64_t what = next() % 9; size_t S = l->S;
            if (what == 0) poke(r, c, l, S + offsetof(struct pmm, free), 0);
            else if (what == 1) poke(r, c, l, S + offsetof(struct pmm, free), r->p->free + 1 + next() % 3);
            else if (what == 2) poke(r, c, l, S + offsetof(struct pmm, hint), next() % (frames / 64 + 3));
            else if (what == 3) poke(r, c, l, S + offsetof(struct pmm, hint), next());
            else if (what == 4) poke32(r, c, S + offsetof(struct pmm, state), (int)(next() % 4));
            else if (what == 5 && l->A != (size_t)-1 && frames >= 64) { size_t o = l->A + 8 * (next() % (frames / 64)); uint64_t v = next(); poke(r, c, l, o, v); }   /* avail bits */
            else if (what == 6 && l->L != (size_t)-1 && frames >= 64) { size_t o = l->L + 8 * (next() % (frames / 64)); uint64_t v = next(); v &= next(); poke(r, c, l, o, v); }
            else if (what == 7 && l->A != (size_t)-1 && frames >= 64) poke(r, c, l, l->A, r->p->avail[0] | 1ull << (next() % 64));   /* allocatable below 1 MiB */
            else if (what == 8) poke32(r, c, S + offsetof(struct pmm, state), PMM_READY);
        }
    }
}

/* ---- instances ---- */
struct map { struct mb2_tag_mmap h; struct mb2_mmap_entry e[8]; };
static int init_ref(struct world *r, const struct layout *l, int mapkind)
{
    static const struct mb2_mmap_entry full[] = { { 0, 0x9fc00, 1, 0 }, { 0x100000, 0x3fee0000, 1, 0 }, { 0x3ffe0000, 0x20000, 2, 0 } };
    static const struct mb2_mmap_entry holes[] = { { 0x100000, 0x3ef000, 1, 0 }, { 0x180000, 0x2000, 2, 0 }, { 0x3f0000, 0x10000, 1, 0 }, { 0x400000, 0x40000000, 1, 0 } };
    static const struct mb2_mmap_entry tiny[] = { { 0x100000, 0x3000, 1, 0 } };
    const struct mb2_mmap_entry *e = mapkind == 0 ? full : mapkind == 1 ? holes : tiny; int n = mapkind == 0 ? 3 : mapkind == 1 ? 4 : 1;
    struct map m; memset(&m, 0, sizeof m); m.h.entry_size = sizeof(struct mb2_mmap_entry); m.h.tag.size = (uint32_t)(sizeof m.h + (size_t)n * sizeof *e);
    memcpy(m.e, e, (size_t)n * sizeof *e);
    struct boot_info bi; memset(&bi, 0, sizeof bi); bi.mmap = &m.h; bi.mmap_entries = (uint32_t)n;
    struct resv_table t; memset(&t, 0, sizeof t); t.r[0].start = 0x200000; t.r[0].end = 0x202001; t.count = 1;
    return ref_pmm_init_in(r->p, r->p->avail, r->p->alloc, l->frames, &bi, mapkind == 1 ? &t : 0);
}
/* A filled instance written directly (any layout): random bitmaps, consistent or corrupted counters. */
static void fill_direct(struct world *r, const struct layout *l)
{
    uint64_t words = l->frames / 64;
    if (l->A != (size_t)-1) for (uint64_t w = 0; w < words; w++) { uint64_t v = w < 4 && next() % 4 ? 0 : (next() % 3 ? ~0ull : next()); memcpy(r->ar.base + l->A + 8 * w, &v, 8); }
    if (l->L != (size_t)-1 && l->L != l->A) for (uint64_t w = 0; w < words; w++) { uint64_t v; memcpy(&v, r->ar.base + l->A + 8 * (w % (words ? words : 1)), 8); v &= next(); if (l->A != (size_t)-1) memcpy(r->ar.base + l->L + 8 * w, &v, 8); }
    r->p->frames = l->frames; r->p->state = PMM_READY; r->p->hint = next() % 3 ? 0 : next() % (words + 1);
    uint64_t u = 0, f = 0;
    for (uint64_t w = 0; w < words && l->A != (size_t)-1 && l->L != (size_t)-1; w++) { u += (uint64_t)__builtin_popcountll(r->p->avail[w]); f += (uint64_t)__builtin_popcountll(r->p->avail[w] & ~r->p->alloc[w]); }
    r->p->usable = u; r->p->free = next() % 5 ? f : f + next() % 3;
}
static void run_layout(struct layout l, int steps, int init_kind)
{
    struct world r, c; world_new(&r, &l); world_new(&c, &l);
    world_pointers(&r, &l);
    if (init_kind >= 0) init_ref(&r, &l, init_kind);
    else fill_direct(&r, &l);
    world_sync(&c, &r, &l);
    world_compare(&r, &c, &l, "initial state");
    cov_layout[l.kind]++;
    sequence(&r, &c, &l, steps);
    arena_free(&r.ar); arena_free(&c.ar);
}

/* Outside the documented storage: only the Rust runs and must refuse without touching memory past the C's own writes. */
static void refusals(void)
{
    static const uint64_t frames_list[] = { 256, 512, 100, 4096 };
    for (int k = 0; k < 4; k++)
        for (int q = 0; q < 3; q++) {
            struct arena a; arena_new(&a, 64 + 16 + 8 * 64);
            struct pmm *p = (struct pmm *)a.base; uint64_t *phys = (uint64_t *)(a.base + 64);
            uint64_t *bm = (uint64_t *)(a.base + 80);
            p->frames = frames_list[k]; p->state = PMM_READY; p->free = 5; p->usable = 9; p->hint = 0;
            for (int w = 0; w < 64; w++) bm[w] = ~0ull;
            p->avail = q == 0 ? 0 : q == 1 ? (uint64_t *)((uint8_t *)bm + 4) : bm; p->alloc = q == 2 ? 0 : bm;
            *phys = 0x1234;
            uint8_t before[64 + 16 + 8 * 64]; memcpy(before, a.base, sizeof before);
            int rc = pmm_alloc_in(p, phys); checks += 3; cov_refusal++;
            if (rc != PMM_ECORRUPT || *phys != 0) fail("refusal alloc", 7, (uint64_t)rc, *phys);
            *phys = 0x1234;
            if (memcmp(before, a.base, sizeof before)) fail("refusal alloc wrote beyond *phys", 7, 0, 0);
            rc = pmm_reserve_in(p, 0x1000); cov_refusal++;
            if (rc != PMM_ECORRUPT || memcmp(before, a.base, sizeof before)) fail("refusal reserve", 7, (uint64_t)rc, 0);
            arena_free(&a);
        }
    /* frames not a multiple of 64 with exact storage (1 word for 100 frames): reserving frame 70 (word 1) is outside the
     * documented storage; the Rust refuses, the C would read past the bitmap. */
    struct arena a; arena_new(&a, 64 + 8 + 8);
    struct pmm *p = (struct pmm *)a.base; uint64_t *av = (uint64_t *)(a.base + 64), *al = (uint64_t *)(a.base + 72);
    *av = ~0ull; *al = 0; p->avail = av; p->alloc = al; p->frames = 100; p->state = PMM_READY; p->free = 64; p->usable = 64;
    int rc = pmm_reserve_in(p, 70 * PAGE_SIZE); checks++; cov_refusal++;
    if (rc != PMM_ECORRUPT || *al != 0 || p->free != 64) fail("refusal reserve past storage", 7, (uint64_t)rc, *al);
    arena_free(&a);
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], 0, 0) : 600;
    setvbuf(stdout, 0, _IOLBF, 0);
    PG = (size_t)sysconf(_SC_PAGESIZE);
    /* 0: separate regions, init-built (several sizes up to PMM_MAX_FRAMES) */
    static const uint64_t sizes[] = { 256, 320, 512, 1024, 4096, 16384, PMM_MAX_FRAMES };
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
        for (int mk = 0; mk < 3; mk++) {
            uint64_t words = sizes[s] / 64;
            struct layout l = { 64 + 16 + 16 * words, 0, 80, 80 + 8 * words, 64, sizes[s], 0 };
            run_layout(l, sizes[s] >= 65536 ? 400 : 1500, mk);
        }
    for (unsigned i = 0; i < n; i++) {
        uint64_t k = next() % 10, words = k < 5 ? 1 + next() % 16 : 4 + next() % 300, frames = 64 * words;
        size_t bm = 8 * words; int kind = (int)(next() % 7);
        struct layout l = { 0 };
        l.frames = frames; l.kind = kind;
        switch (kind) {
        case 0: l = (struct layout){ 64 + 16 + 2 * bm, 0, 80, 80 + bm, 64, frames, 0 }; break;                 /* separate */
        case 1: l = (struct layout){ 64 + 16 + bm, 0, 80, 80, 64, frames, 1 }; break;                          /* identical bitmaps */
        case 2: { size_t sh = 8 * (1 + next() % words); l = (struct layout){ 64 + 16 + bm + sh, 0, 80, 80 + sh, 64, frames, 2 }; break; }   /* overlapping */
        case 3: l = (struct layout){ 64 + 16 + bm, 0, 80, 80 + bm - 8 * (words > 1 ? words / 2 : 0), 64, frames, 3 }; l.size = 64 + 16 + bm + 8 * (words - (words > 1 ? words / 2 : 0)); break;  /* alloc starts inside avail */
        /* *phys on usable/free/hint (never on frames: a written frames value would change the instance's documented storage) */
        case 4: { size_t f = 8 * (1 + next() % 3); l = (struct layout){ 64 + 16 + 2 * bm, 0, 80, 80 + bm, 16 + f, frames, 4 }; cov_phys_alias++; break; }
        case 5: { l = (struct layout){ 64 + 16 + 2 * bm, 0, 80, 80 + bm, 0, frames, 5 }; size_t region = next() % 2 ? 80 : 80 + bm; l.P = region + 8 * (next() % words); cov_phys_alias++; break; }  /* *phys on a bitmap word */
        case 6: {   /* a bitmap on the instance's uint64_t fields: avail = &p->free (1 word, 64 frames) or alloc = &p->usable (2 words) */
            words = next() % 2 ? 1 : 2; frames = 64 * words; bm = 8 * words;
            if (words == 1) l = (struct layout){ 64 + 16 + 8, 0, 32, 80, 64, frames, 6 };
            else l = (struct layout){ 64 + 16 + 16, 0, 80, 24, 64, frames, 6 };
            break; }
        }
        int steps = 60 + (int)(next() % 200);   /* each draw sequenced: argument evaluation order is unspecified in C */
        int init_kind = (kind == 0 && words >= 4 && next() % 2) ? (int)(next() % 3) : -1;
        run_layout(l, steps, init_kind);
    }
    /* frames not a multiple of 64, and above PMM_MAX_FRAMES, with their documented storage: both sides, identical */
    for (int i = 0; i < 40; i++) {
        uint64_t frames;
        if (i < 20) { uint64_t base = 64 * (1 + next() % 8); frames = base + 1 + next() % 63; } else frames = PMM_MAX_FRAMES + 64 * (1 + next() % 4);
        uint64_t words = frames / 64; size_t bm = 8 * words;
        struct layout l = { 64 + 16 + 2 * bm, 0, 80, 80 + bm, 64, frames, 0 };
        if (i < 20) cov_nonmultiple++; else cov_over_max++;
        run_layout(l, 200, -1);
    }
    refusals();
    printf("PMM TRANSITION DIFFERENTIAL ops=%llu checks=%llu mismatches=%llu digest=%016llx\n", (unsigned long long)ops, (unsigned long long)checks,
           (unsigned long long)mismatches, (unsigned long long)digest);
    printf("PMM TRANSITION COVERAGE alloc=");
    for (int i = 0; i < 11; i++) printf("%s%llu", i ? "," : "", (unsigned long long)cov[0][i]);
    printf(" free="); for (int i = 0; i < 11; i++) printf("%s%llu", i ? "," : "", (unsigned long long)cov[1][i]);
    printf(" reserve="); for (int i = 0; i < 11; i++) printf("%s%llu", i ? "," : "", (unsigned long long)cov[2][i]);
    printf(" layouts="); for (int i = 0; i < 7; i++) printf("%s%llu", i ? "," : "", (unsigned long long)cov_layout[i]);
    printf(" failed=%llu exhaust=%llu reuse=%llu phys_alias=%llu nonmultiple=%llu over_max=%llu refusal=%llu outside_storage=%llu\n", (unsigned long long)cov_failed,
           (unsigned long long)cov_exhaust, (unsigned long long)cov_reuse, (unsigned long long)cov_phys_alias, (unsigned long long)cov_nonmultiple,
           (unsigned long long)cov_over_max, (unsigned long long)cov_refusal, (unsigned long long)cov_outside_storage);
    return mismatches != 0;
}
