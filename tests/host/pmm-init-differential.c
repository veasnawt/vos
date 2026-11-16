/* R4-M3: the frozen accepted pmm.c (tests/reference/pmm-r4m3.c; its public symbols renamed ref_) against the production Rust
 * pmm_init_in, as a whole-state comparison.
 *
 * One world: two regions between three PROT_NONE guard pages, at a fixed address so that pointers stored in it are the same in
 * every run. It holds the struct pmm, both bitmaps, the boot information, the memory-map tag with its entries and the
 * reservation table, in one of several layouts. The world starts as seeded garbage (so zeroing is checked). The frozen C runs
 * on it; its result and every byte are saved; the world is restored byte for byte; the Rust runs on the same addresses; the
 * return values and every byte of both regions must be equal.
 *
 * Layouts 0-7 are defined C: separate objects with each bitmap flush against a guard page; identical bitmaps; bitmaps
 * overlapping by whole words; a bitmap lying on the instance's uint64_t counters and state; the memory map inside a bitmap; the
 * reservation table inside a bitmap; the instance inside the map entries or the table; the instance on the boot information.
 * Where the C would leave defined behavior (classes X1-X5: a map pointer zeroed by the bitmap clear, an instance whose avail
 * field is overwritten before a protected range is cleared, a reservation count past the table, misaligned or NULL outputs, a
 * misaligned boot information) only the Rust runs: it must return its documented refusal, fault nowhere, and leave every
 * byte outside the instance and the two bitmaps untouched.
 * usage: pmm-init-differential <seeded cases> */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "pmm.h"
#include "multiboot2.h"

/* The layouts arch/x86_64/rust/pmm_init.rs uses, against this compiler's. */
_Static_assert(sizeof(struct pmm) == 64 && offsetof(struct pmm, avail) == 0 && offsetof(struct pmm, alloc) == 8 && offsetof(struct pmm, frames) == 16, "pmm");
_Static_assert(offsetof(struct pmm, usable) == 24 && offsetof(struct pmm, free) == 32 && offsetof(struct pmm, hint) == 40, "pmm");
_Static_assert(offsetof(struct pmm, ignored_bytes) == 48 && offsetof(struct pmm, state) == 56 && sizeof(int) == 4, "pmm");
_Static_assert(offsetof(struct boot_info, mmap) == 32 && offsetof(struct boot_info, mmap_entries) == 40, "boot_info");
_Static_assert(sizeof(struct mb2_tag_mmap) == 16 && offsetof(struct mb2_tag_mmap, entry_size) == 8 && offsetof(struct mb2_tag_mmap, entry_version) == 12, "tag");
_Static_assert(sizeof(struct mb2_mmap_entry) == 24 && offsetof(struct mb2_mmap_entry, base) == 0 && offsetof(struct mb2_mmap_entry, length) == 8 && offsetof(struct mb2_mmap_entry, type) == 16, "entry");
_Static_assert(RESV_MAX == 80 && sizeof(struct phys_range) == 24 && offsetof(struct phys_range, end) == 8 && offsetof(struct resv_table, count) == 1920 && sizeof(size_t) == 8, "resv");
_Static_assert(MB2_MMAP_AVAILABLE == 1 && PMM_MAX_FRAMES == 262144 && PMM_LOW_LIMIT == 0x100000 && PAGE_SIZE == 0x1000, "constants");
_Static_assert(PMM_OK == 0 && PMM_EMMAP == 7 && PMM_EINVAL == 8 && PMM_ECORRUPT == 9 && PMM_READY == 1 && PMM_FAILED == 2, "codes");

int ref_pmm_init_in(struct pmm *, uint64_t *, uint64_t *, uint64_t, const struct boot_info *, const struct resv_table *);
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }
/* The accepted C's entry lookup, for the frozen side. */
const struct mb2_mmap_entry *ref_boot_mmap_entry(const struct boot_info *bi, uint32_t i)
{ return (const struct mb2_mmap_entry *)((const uint8_t *)bi->mmap + sizeof *bi->mmap + (size_t)i * bi->mmap->entry_size); }
#ifdef HOST_RUST    /* host builds of the Rust have no boot_resv.rs: its boot_mmap_entry, statement for statement */
const struct mb2_mmap_entry *boot_mmap_entry(const struct boot_info *bi, uint32_t i)
{
    const uint8_t *tag; uint32_t size;
    if (!bi || ((uintptr_t)bi & 7)) return 0;
    memcpy(&tag, (const uint8_t *)bi + 32, 8);
    if (!tag) return 0;
    memcpy(&size, tag + 8, 4);
    return (const struct mb2_mmap_entry *)(tag + 16 + (size_t)i * size);
}
#endif

#define REGION (160u * 1024)
#define BASE ((uint8_t *)0x200000000000ull)     /* far from the loader's mappings and from the sanitizer's allocator and shadow */
static size_t PG;
static uint8_t *R1, *R2, *save, *after;          /* the two regions; the initial bytes; the frozen C's result */
static uint64_t digest = 1469598103934665603ull, cases, checks, mismatches, rng = 0x52344d33494e4954ull;   /* "R4M3INIT" */
static uint64_t cov_rc[3], cov_layout[8], cov_x[6], cov_small, cov_max, cov_ignored, cov_overflow, cov_reserved, cov_low, cov_round, cov_nousable, cov_alias_write;
static void mix(uint64_t v) { for (int i = 0; i < 8; i++) { digest ^= (v >> (8 * i)) & 0xff; digest *= 1099511628211ull; } }
/* One draw per statement everywhere: the order of two calls inside one expression is unspecified and differs between builds. */
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void fail(const char *what, int layout, uint64_t a, uint64_t b)
{ if (++mismatches <= 20) printf("MISMATCH %s layout=%d %#llx %#llx\n", what, layout, (unsigned long long)a, (unsigned long long)b); }

/* Where each object lives in the current case. */
struct place { uint8_t *p, *avail, *alloc, *bi, *tag, *t; uint64_t frames; uint32_t n, esize; int kind, x; };

static void garbage(void)
{
    for (size_t i = 0; i < REGION; i += 8) { uint64_t v = next(); memcpy(R1 + i, &v, 8); v = next(); memcpy(R2 + i, &v, 8); }
}
static uint64_t pick_frames(void)
{
    static const uint64_t f[] = {64, 128, 192, 256, 320, 512, 1024, 4096, 16384, 65536, PMM_MAX_FRAMES, PMM_MAX_FRAMES};
    return f[next() % (sizeof f / sizeof f[0])];
}
/* One address around the boundaries that matter: 0, 1 MiB, the limit, 2^64. */
static uint64_t pick_addr(uint64_t limit)
{
    uint64_t k = next() % 16, d = next() % 3 == 0 ? next() % (4 * PAGE_SIZE) : (next() % 64) * PAGE_SIZE;
    switch (k) {
    case 0: return d;
    case 1: return PMM_LOW_LIMIT - d;
    case 2: case 3: return PMM_LOW_LIMIT + d;
    case 4: case 5: return limit - d;
    case 6: return limit + d;
    case 7: return 0 - d;                                           /* near 2^64 */
    case 8: return next();
    default: { uint64_t v = next() % (limit + 16 * PAGE_SIZE), aligned = next() % 4; return aligned ? v & ~(PAGE_SIZE - 1) : v; }
    }
}
static void put_entry(uint8_t *e, uint64_t base, uint64_t length, uint32_t type)
{ memcpy(e, &base, 8); memcpy(e + 8, &length, 8); memcpy(e + 16, &type, 4); }
/* The inputs: boot information, tag, entries, table. Returns the table argument (NULL for none). */
static const struct resv_table *build(struct place *pl)
{
    uint64_t limit = pl->frames * PAGE_SIZE; uint32_t esize = pl->esize, version = 0, entries = pl->n;
    if (next() % 40 == 0) esize = (uint32_t[]){0, 8, 16, 20, 23, 28, 36}[next() % 7];
    if (next() % 60 == 0) version = 1 + (uint32_t)next() % 3;
    if (next() % 60 == 0) entries = 0;
    memset(pl->bi, 0, sizeof(struct boot_info));
    uint8_t *tag = next() % 60 == 0 ? 0 : pl->tag;
    memcpy(pl->bi + 32, &tag, 8); memcpy(pl->bi + 40, &entries, 4);
    uint32_t hdr[4] = {6, 16 + pl->n * pl->esize, esize, version}; memcpy(pl->tag, hdr, 16);
    for (uint32_t i = 0; i < pl->n; i++) {
        uint64_t a = pick_addr(limit), b = pick_addr(limit), len = next() % 5 == 0 ? next() % (8 * PAGE_SIZE) : b - a;
        uint32_t type = (uint32_t[]){1, 1, 1, 1, 2, 2, 3, 4, 5, 0, 7}[next() % 11];
        if (next() % 12 == 0) len = 0;
        if (next() % 20 == 0) len = 0 - a + next() % 3;             /* ends at, or wraps just past, 2^64 */
        if (i == 0 && next() % 2) { a = 0; len = limit + (next() % 4) * PAGE_SIZE; type = 1; }       /* a full-range RAM entry to carve */
        put_entry(pl->tag + 16 + (size_t)i * pl->esize, a, len, type);
    }
    if (!pl->t) return 0;
    uint64_t count = next() % 6 == 0 ? RESV_MAX : next() % 12;
    for (uint64_t i = 0; i < RESV_MAX; i++) {
        uint64_t a = pick_addr(limit), aligned = next() % 6, how = next() % 8, pages = 1 + next() % 8, less = next() % 2;
        if (aligned) a &= ~(PAGE_SIZE - 1);
        uint64_t b = how == 0 ? UINT64_MAX : how == 1 ? a : a + pages * PAGE_SIZE - less;
        memcpy(pl->t + i * 24, &a, 8); memcpy(pl->t + i * 24 + 8, &b, 8); memset(pl->t + i * 24 + 16, 0, 8);
    }
    memcpy(pl->t + 1920, &count, 8);
    return (const struct resv_table *)pl->t;
}
/* Lay the objects out. Everything not placed specially goes at the start of region 1. */
static void layout(struct place *pl, int kind)
{
    size_t bytes = pl->frames / 64 * 8;
    pl->kind = kind; pl->x = 0;
    pl->p = R1 + 64; pl->bi = R1 + 256; pl->t = next() % 4 ? R1 + 2048 : 0; pl->tag = R1 + 4096;
    uint64_t many = next() % 5 == 0, count = next(); pl->n = 1 + (uint32_t)(count % (many ? 64 : 12)); pl->esize = (uint32_t[]){24, 24, 24, 32, 40}[next() % 5];
    pl->avail = R1 + REGION - bytes; pl->alloc = R2 + REGION - bytes;                 /* each flush against a guard page */
    uint64_t words = pl->frames / 64, k = 1 + next() % 5, which = next() % 2, where = next(), other = next() % 2;
    switch (kind) {
    case 1: pl->alloc = pl->avail; break;
    case 2: if (words > 1) { pl->alloc = R2 + REGION - bytes; pl->avail = pl->alloc - 8 * (1 + where % (words - 1)); } break;
    case 3: pl->p = R2 + 64; *(which ? &pl->avail : &pl->alloc) = pl->p + 16 + 8 * k; break;      /* on usable..state, never the pointers or frames */
    case 4: { size_t need = 16 + (size_t)pl->n * pl->esize;                    /* the whole map inside one bitmap: zeroed after validation */
              if (need <= bytes) pl->tag = (which ? pl->avail : pl->alloc) + 8 * (where % ((bytes - need) / 8 + 1)); } break;
    case 5: if (pl->t && bytes >= 4096) pl->t = pl->alloc + 8 * (where % (words - 256)); break;      /* in alloc only: avail bits would become its count */
    case 6: if (pl->t && which) pl->p = pl->t + 24 * (where % 70); else pl->p = pl->tag + 16 + 8 * (where % 6); break;
    case 7: pl->p = pl->bi + 8 * (which ? 6 + where % 40 : where % 6) - (other ? 0 : 56);
            if (pl->p + 56 == pl->bi + 32) pl->p += 8;         /* never the state word on bi->mmap: the C would follow FAILED as a pointer */
            break;
    }
}
static int in(const uint8_t *q, const uint8_t *lo, size_t n) { return q >= lo && q < lo + n; }
/* The boot information may be misaligned (X5): read its two fields bytewise. Nonzero if the map would pass validation. */
static int map_valid(const struct place *pl)
{
    const uint8_t *tag; uint32_t n, size, version;
    memcpy(&tag, pl->bi + 32, 8); memcpy(&n, pl->bi + 40, 4);
    if (!tag || !n) return 0;
    memcpy(&size, tag + 8, 4); memcpy(&version, tag + 12, 4);
    return version == 0 && size >= 24 && size % 8 == 0;
}

static void run_case(struct place *pl, const struct resv_table *t, uint64_t frames_arg, uint64_t *avail, uint64_t *alloc, const struct boot_info *bi)
{
    memcpy(save, R1, REGION); memcpy(save + REGION, R2, REGION);
    int a = ref_pmm_init_in((struct pmm *)pl->p, avail, alloc, frames_arg, bi, t);
    memcpy(after, R1, REGION); memcpy(after + REGION, R2, REGION);
    memcpy(R1, save, REGION); memcpy(R2, save + REGION, REGION);
    int b = pmm_init_in((struct pmm *)pl->p, avail, alloc, frames_arg, bi, t);
    cases++; checks += 2; mix((uint64_t)a); mix((uint64_t)pl->kind);
    if (a != b) fail("result", pl->kind, (uint64_t)a, (uint64_t)b);
    for (size_t i = 0; i < 2 * REGION; i++) {
        uint8_t v = i < REGION ? R1[i] : R2[i - REGION];
        if (v != after[i]) { fail("state byte", pl->kind, i, ((uint64_t)after[i] << 8) | v); break; }
    }
    for (size_t i = 0; i < 2 * REGION; i += 8) { uint64_t v; memcpy(&v, after + i, 8); mix(v); }
    cov_rc[a == PMM_OK ? 0 : a == PMM_EINVAL ? 1 : 2]++; cov_layout[pl->kind]++;
    if (a != PMM_OK || pl->kind > 2) return;
    struct pmm *p = (struct pmm *)pl->p; uint64_t limit = pl->frames * PAGE_SIZE;
    cov_small += pl->frames < 256; cov_max += pl->frames == PMM_MAX_FRAMES; cov_ignored += p->ignored_bytes != 0; cov_nousable += p->usable == 0;
    for (uint32_t i = 0; i < pl->n && bi && bi->mmap; i++) {
        uint64_t base, len; uint32_t type; const uint8_t *e = pl->tag + 16 + (size_t)i * pl->esize;
        memcpy(&base, e, 8); memcpy(&len, e + 8, 8); memcpy(&type, e + 16, 4);
        cov_overflow += len && base + len < base; cov_low += type == 1 && len && base < PMM_LOW_LIMIT; cov_round += type == 1 && len && base < limit && ((base | (base + len)) & (PAGE_SIZE - 1));
    }
    cov_reserved += t && t->count;
}
/* A case only the Rust may run: the documented refusal, no fault, and nothing written outside the instance and the bitmaps. */
static void rust_only(struct place *pl, const struct resv_table *t, uint64_t *avail, uint64_t *alloc, const struct boot_info *bi, struct pmm *p, int want, int x)
{
    memcpy(save, R1, REGION); memcpy(save + REGION, R2, REGION);
    int b = pmm_init_in(p, avail, alloc, pl->frames, bi, t);
    size_t bytes = pl->frames / 64 * 8;
    cases++; checks += 2; cov_x[x]++; mix(100 + (uint64_t)x); mix((uint64_t)b);
    if (b != want) fail("refusal result", 100 + x, (uint64_t)b, (uint64_t)want);
    for (size_t i = 0; i < 2 * REGION; i++) {
        uint8_t *q = i < REGION ? R1 + i : R2 + (i - REGION);
        if (*q != save[i] && !in(q, pl->p, 64) && !in(q, pl->avail, bytes) && !in(q, pl->alloc, bytes)) { fail("refusal wrote outside the instance and bitmaps", 100 + x, i, *q); break; }
    }
    if (((uintptr_t)p & 7) == 0 && p && !in((uint8_t *)p + 56, pl->avail, bytes) && !in((uint8_t *)p + 56, pl->alloc, bytes) && p->state != PMM_FAILED) fail("refusal left the instance usable", 100 + x, (uint64_t)p->state, 0);
}

static void one(int kind)
{
    struct place pl; pl.frames = pick_frames(); garbage(); layout(&pl, kind);
    const struct resv_table *t = build(&pl);
    uint64_t *avail = (uint64_t *)pl.avail, *alloc = (uint64_t *)pl.alloc, frames = pl.frames; const struct boot_info *bi = (const struct boot_info *)pl.bi;
    uint64_t r = next() % 40;                                       /* argument refusals, on every layout */
    if (r == 0) avail = 0; else if (r == 1) alloc = 0; else if (r == 2) bi = 0;
    else if (r == 3) frames = (uint64_t[]){0, 100, 63, 32, 96, PMM_MAX_FRAMES + 32, PMM_MAX_FRAMES + 64, 1ull << 40, ~0ull}[next() % 9];
    if (kind >= 3 && frames == pl.frames && avail && alloc && bi) cov_alias_write++;
    run_case(&pl, t, frames, avail, alloc, bi);
}
static void refusals(void)
{
    struct place pl; uint64_t x = next() % 5; pl.frames = pick_frames(); garbage(); layout(&pl, 0); pl.t = R1 + 2048;
    size_t bytes = pl.frames / 64 * 8; uint64_t count; const struct resv_table *t;
    switch (x) {
    case 0:     /* X1: the bitmap clear zeroes bi->mmap but not bi->mmap_entries */
        pl.bi = R1 + 40960; pl.avail = pl.bi + 40 - bytes; pl.t = 0;
        t = build(&pl); if (!map_valid(&pl)) return;
        rust_only(&pl, t, (uint64_t *)pl.avail, (uint64_t *)pl.alloc, (const struct boot_info *)pl.bi, (struct pmm *)pl.p, PMM_EMMAP, 1); break;
    case 1:     /* X2: the instance's avail field lies in the alloc bitmap; one reserved entry inside the range forces a protect */
        if (bytes < 128) return;
        pl.p = pl.alloc + 8 * (1 + next() % (bytes / 8 - 9)); pl.t = 0; t = build(&pl);
        if (!map_valid(&pl)) return;
        { uint32_t hdr[4] = {6, 16 + pl.n * pl.esize, pl.esize, 0}; memcpy(pl.tag, hdr, 16); put_entry(pl.tag + 16, PMM_LOW_LIMIT / 2, PAGE_SIZE, 2); if (pl.frames * PAGE_SIZE <= PMM_LOW_LIMIT / 2) return; }
        rust_only(&pl, t, (uint64_t *)pl.avail, (uint64_t *)pl.alloc, (const struct boot_info *)pl.bi, (struct pmm *)pl.p, PMM_ECORRUPT, 2); break;
    case 2:     /* X3: a reservation count past the table, which is flush against a guard page */
        pl.t = R2 + REGION - sizeof(struct resv_table); pl.alloc = R2 + 4096; t = build(&pl);
        if (!map_valid(&pl)) return;
        { uint64_t far = next() % 3, by = next() % 1000, all = next() % 4 == 0; count = all ? ~0ull : RESV_MAX + 1 + far * by; } memcpy(pl.t + 1920, &count, 8);
        rust_only(&pl, t, (uint64_t *)pl.avail, (uint64_t *)pl.alloc, (const struct boot_info *)pl.bi, (struct pmm *)pl.p, PMM_EINVAL, 3); break;
    case 3:     /* X4: misaligned or NULL outputs */
        t = build(&pl); x = next() % 4;
        if (x == 0) { struct pmm *bad = next() % 2 ? 0 : (struct pmm *)(pl.p + 1 + next() % 7); uint8_t *keep = pl.p; pl.p = R1; rust_only(&pl, t, (uint64_t *)pl.avail, (uint64_t *)pl.alloc, (const struct boot_info *)pl.bi, bad, PMM_EINVAL, 4);
                      pl.p = keep; for (size_t i = 0; i < 2 * REGION; i++) if ((i < REGION ? R1[i] : R2[i - REGION]) != save[i]) { fail("a refused instance pointer was written through", 104, i, 0); break; } }
        else { uint64_t *a = (uint64_t *)pl.avail, *l = (uint64_t *)pl.alloc; if (x == 1) a = (uint64_t *)(pl.avail - 8 + 1 + next() % 7); else if (x == 2) l = (uint64_t *)(pl.alloc - 8 + 1 + next() % 7); else { a = (uint64_t *)(pl.avail - 4); l = (uint64_t *)(pl.alloc - 4); }
               uint8_t *ka = pl.avail, *kl = pl.alloc; pl.avail = pl.alloc = pl.p;     /* no bitmap byte may change */
               rust_only(&pl, t, a, l, (const struct boot_info *)pl.bi, (struct pmm *)pl.p, PMM_EINVAL, 4); pl.avail = ka; pl.alloc = kl; }
        break;
    case 4:     /* X5: a misaligned boot information */
        pl.bi = R1 + 256 + 1 + next() % 7; t = build(&pl);
        if (!map_valid(&pl)) return;
        rust_only(&pl, t, (uint64_t *)pl.avail, (uint64_t *)pl.alloc, (const struct boot_info *)pl.bi, (struct pmm *)pl.p, PMM_EMMAP, 5); break;
    }
}

int main(int argc, char **argv)
{
    unsigned long long seeded = argc > 1 ? strtoull(argv[1], 0, 10) : 0;
    PG = 4096; size_t body = REGION, len = 3 * PG + 2 * body;
    uint8_t *m = mmap(BASE, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != BASE || mprotect(m, PG, PROT_NONE) || mprotect(m + PG + body, PG, PROT_NONE) || mprotect(m + 2 * PG + 2 * body, PG, PROT_NONE)) { perror("world"); return 2; }
    R1 = m + PG; R2 = m + 2 * PG + body; save = malloc(2 * REGION); after = malloc(2 * REGION);
    for (int kind = 0; kind < 8; kind++)                    /* a fixed share of every layout, then the seeded mix */
        for (int i = 0; i < 400; i++) one(kind);
    for (unsigned long long i = 0; i < seeded; i++) { if (next() % 10 == 0) refusals(); else one((int)(next() % 8)); }
    printf("PMM INIT COVERAGE rc=%llu,%llu,%llu layouts=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu refusals=%llu,%llu,%llu,%llu,%llu small=%llu max=%llu ignored=%llu overflow=%llu reserved=%llu low=%llu round=%llu nousable=%llu aliased=%llu\n",
           (unsigned long long)cov_rc[0], (unsigned long long)cov_rc[1], (unsigned long long)cov_rc[2], (unsigned long long)cov_layout[0], (unsigned long long)cov_layout[1], (unsigned long long)cov_layout[2],
           (unsigned long long)cov_layout[3], (unsigned long long)cov_layout[4], (unsigned long long)cov_layout[5], (unsigned long long)cov_layout[6], (unsigned long long)cov_layout[7],
           (unsigned long long)cov_x[1], (unsigned long long)cov_x[2], (unsigned long long)cov_x[3], (unsigned long long)cov_x[4], (unsigned long long)cov_x[5], (unsigned long long)cov_small, (unsigned long long)cov_max,
           (unsigned long long)cov_ignored, (unsigned long long)cov_overflow, (unsigned long long)cov_reserved, (unsigned long long)cov_low, (unsigned long long)cov_round, (unsigned long long)cov_nousable, (unsigned long long)cov_alias_write);
    printf("PMM INIT DIFFERENTIAL cases=%llu checks=%llu mismatches=%llu digest=%016llx\n", (unsigned long long)cases, (unsigned long long)checks, (unsigned long long)mismatches, (unsigned long long)digest);
    return mismatches ? 1 : 0;
}
