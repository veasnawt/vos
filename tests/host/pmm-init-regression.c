/* R4-M1H: pmm_init_in small-instance memory-safety regression. The frozen accepted pmm.c (tests/reference/pmm-r4m1.c, every
 * public symbol renamed ref_) against the production arch/x86_64/pmm.c, whose only change bounds the defensive low-memory clear
 * to min(frames, PMM_LOW_LIMIT / PAGE_SIZE). Every bitmap is exactly PMM_WORDS(frames) words, flush against a PROT_NONE guard
 * page, with canary bytes before it in the same page; both bitmaps start filled with a garbage pattern, so init's zeroing is
 * checked too.
 *
 * 1. Negative control (64, 128, 192 frames): the frozen C runs in a forked child on exact bitmaps and must fault on the guard,
 *    at exactly the first byte past the bitmap (its low-memory clear reads and rewrites the words after the last tracked
 *    frame). The child reports the fault address through a pipe, so the expected fault cannot end the suite.
 * 2. Small instances (64, 128, 192 frames): the production code completes on exact bitmaps over many maps and reservations;
 *    return value, state, counters, hint, ignored bytes and every in-bounds word must equal the frozen C, which runs on a
 *    4-word bitmap so that it stays defined. The frozen C's writes past PMM_WORDS(frames) are its defect, not an output.
 * 3. Large instances (256 frames up to PMM_MAX_FRAMES, fixed and seeded): both run on exact bitmaps; everything must be
 *    identical, field for field and word for word.
 * 4. Refused initialization (EINVAL, EMMAP) for small and large frames: identical results, and no bitmap word written.
 * Prints a transcript digest so release and sanitizer runs can be compared. */
#define _GNU_SOURCE
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pmm.h"
#include "multiboot2.h"

int ref_pmm_init_in(struct pmm *, uint64_t *, uint64_t *, uint64_t, const struct boot_info *, const struct resv_table *);

__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }
/* Both sides are built with -Dboot_mmap_entry=<side>_boot_mmap_entry: entry i of the test map, as the kernel computes it. */
static const struct mb2_mmap_entry *entry(const struct boot_info *bi, uint32_t i)
{ return (const struct mb2_mmap_entry *)((const uint8_t *)bi->mmap + sizeof *bi->mmap + (size_t)i * bi->mmap->entry_size); }
const struct mb2_mmap_entry *ref_boot_mmap_entry(const struct boot_info *bi, uint32_t i) { return entry(bi, i); }
const struct mb2_mmap_entry *cand_boot_mmap_entry(const struct boot_info *bi, uint32_t i) { return entry(bi, i); }

static uint64_t digest = 1469598103934665603ull, cases, checks, mismatches;
static uint64_t cov_small, cov_large, cov_max, cov_error, cov_seeded, cov_frame256, cov_ignored, cov_reserved, cov_overflow;
static void mix(uint64_t v) { for (int i = 0; i < 8; i++) { digest ^= (v >> (8 * i)) & 0xff; digest *= 1099511628211ull; } }
static void fail(const char *what, uint64_t frames, uint64_t extra)
{
    if (++mismatches <= 20) fprintf(stderr, "MISMATCH %s frames=%llu extra=%#llx\n", what, (unsigned long long)frames, (unsigned long long)extra);
}
static uint64_t rng = 0x52344d3148494e31ull;   /* "R4M1HIN1" */
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* ---- guarded bitmaps ---- */
#define GARBAGE 0xa5a5a5a5a5a5a5a5ull
#define CANARY  0x5a
static size_t PG;
struct region { uint8_t *base; size_t len, body; uint64_t *words; size_t n; };
static uint64_t *guarded(size_t nwords, struct region *r)
{
    r->body = (nwords * 8 + PG - 1) / PG * PG; if (!r->body) r->body = PG;
    r->len = r->body + 2 * PG; r->n = nwords;
    r->base = mmap(0, r->len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->base == MAP_FAILED || mprotect(r->base, PG, PROT_NONE) || mprotect(r->base + PG + r->body, PG, PROT_NONE)) abort();
    memset(r->base + PG, CANARY, r->body);
    r->words = (uint64_t *)(r->base + PG + r->body - nwords * 8);
    for (size_t i = 0; i < nwords; i++) r->words[i] = GARBAGE;
    return r->words;
}
static int canary_ok(const struct region *r)
{
    const uint8_t *p = r->base + PG, *end = (const uint8_t *)r->words;
    for (; p < end; p++) if (*p != CANARY) return 0;
    return 1;
}
static void release(struct region *r) { munmap(r->base, r->len); }

/* ---- memory maps and reservation tables ---- */
struct map { struct mb2_tag_mmap h; struct mb2_mmap_entry e[12]; };
struct input { struct map m; struct boot_info bi; struct resv_table t; int has_t; };
static void map_init(struct input *in, const struct mb2_mmap_entry *e, int n)
{
    memset(in, 0, sizeof *in);
    in->m.h.entry_size = sizeof(struct mb2_mmap_entry); in->m.h.entry_version = 0;
    in->m.h.tag.size = (uint32_t)(sizeof in->m.h + (size_t)n * sizeof(struct mb2_mmap_entry));
    memcpy(in->m.e, e, (size_t)n * sizeof *e);
    in->bi.mmap = &in->m.h; in->bi.mmap_entries = (uint32_t)n;
}
static void resv(struct input *in, uint64_t s, uint64_t e) { in->t.r[in->t.count].start = s; in->t.r[in->t.count].end = e; in->t.count++; in->has_t = 1; }
static void seeded_map(struct input *in, uint64_t limit)
{
    struct mb2_mmap_entry e[12]; int n = 1 + (int)(next() % 8);
    for (int i = 0; i < n; i++) {
        uint64_t k = next() % 10, base, len;
        if (k < 3) base = next() % (limit + 0x200000);                       /* anywhere around the instance */
        else if (k < 5) base = next() % 0x110000;                            /* around 1 MiB */
        else if (k < 6) base = limit - (next() % 0x10000);                   /* straddling the limit */
        else if (k < 7) base = 0xffffffffffffffffull - (next() % 0x100000);  /* overflowing */
        else base = next() % (limit ? limit : 1);
        len = (k == 9) ? 0 : (next() % 4 == 0 ? next() : next() % (limit / 2 + 0x30000));
        if (k == 8 && next() % 2) len = 0xffffffffffffffffull - (next() % 0x100000);   /* wraps past 2^64 from inside */
        if (next() % 3 == 0) { base &= ~0xfffull; len &= ~0xfffull; }
        e[i] = (struct mb2_mmap_entry){ base, len, (uint32_t)(next() % 6 ? (next() % 3 ? 1 : 2) : 3 + next() % 3), 0 };
    }
    map_init(in, e, n);
    int r = (int)(next() % 5);
    for (int i = 0; i < r; i++) {
        uint64_t s = next() % (limit + 0x100000), len = next() % 5 == 0 ? UINT64_MAX - s : next() % 0x80000;
        resv(in, s, s + len);
    }
    if (next() % 4 == 0) in->has_t = 0;
}

/* ---- one comparison ---- */
struct result { int ret; struct pmm p; };
static void compare(const char *what, uint64_t frames, const struct result *a, const struct result *b, const uint64_t *aa, const uint64_t *al,
                    const uint64_t *ba, const uint64_t *bl, size_t words)
{
    checks += 9;
    if (a->ret != b->ret) fail("return value", frames, (uint64_t)a->ret << 32 | (uint32_t)b->ret);
    if (a->p.state != b->p.state) fail("state", frames, 0);
    if (a->p.frames != b->p.frames || a->p.usable != b->p.usable || a->p.free != b->p.free || a->p.hint != b->p.hint || a->p.ignored_bytes != b->p.ignored_bytes)
        fail("counters, hint or ignored bytes", frames, a->p.usable ^ b->p.usable);
    if ((a->p.avail == 0) != (b->p.avail == 0) || (a->p.alloc == 0) != (b->p.alloc == 0)) fail("bitmap pointers", frames, 0);
    for (size_t w = 0; w < words; w++) {
        checks += 2;
        if (aa[w] != ba[w]) { fail(what, frames, w); break; }
        if (al[w] != bl[w]) { fail(what, frames, w | 1ull << 63); break; }
    }
    mix((uint64_t)a->ret); mix(a->p.usable); mix(a->p.free); mix(a->p.ignored_bytes); mix((uint64_t)a->p.state);
    for (size_t w = 0; w < words; w++) { mix(aa[w]); mix(al[w]); }
    if (a->p.state == PMM_READY && words > 4 && (aa[4] & 1)) cov_frame256++;   /* the first frame at 1 MiB is allocatable */
    if (a->p.ignored_bytes) cov_ignored++;
    cases++;
}

/* Production on exact bitmaps; the frozen C on `ref_words` (= words for large instances, 4 for small ones). */
static void run_case(uint64_t frames, const struct input *in, int small)
{
    size_t words = (size_t)(frames / 64), ref_words = small ? 4 : words;
    struct region ra, rl, ca, cl;
    uint64_t *a = guarded(ref_words, &ra), *l = guarded(ref_words, &rl), *x = guarded(words, &ca), *y = guarded(words, &cl);
    struct result r = { 0 }, c = { 0 };
    r.ret = ref_pmm_init_in(&r.p, a, l, frames, &in->bi, in->has_t ? &in->t : 0);
    c.ret = pmm_init_in(&c.p, x, y, frames, &in->bi, in->has_t ? &in->t : 0);
    checks += 2;
    if (!canary_ok(&ca) || !canary_ok(&cl)) fail("production wrote before its bitmap", frames, 0);
    if (c.ret == PMM_OK && (c.p.avail != x || c.p.alloc != y)) fail("production bitmap pointers", frames, 0);
    compare(small ? "small in-bounds word" : "large word", frames, &r, &c, a, l, x, y, words);
    if (in->has_t) cov_reserved++;
    for (uint32_t i = 0; i < in->bi.mmap_entries; i++) if (in->m.e[i].base + in->m.e[i].length < in->m.e[i].base) { cov_overflow++; break; }
    release(&ra); release(&rl); release(&ca); release(&cl);
}

/* ---- 1. negative control: the frozen C faults on an exact small bitmap ---- */
static volatile sig_atomic_t fault_fd = -1;
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)ctx;
    uintptr_t addr = (uintptr_t)si->si_addr;
    if (write(fault_fd, &addr, sizeof addr) != sizeof addr) _exit(3);
    _exit(77);
}
static int negative_control(uint64_t frames, const struct input *in, uintptr_t *at, uintptr_t *expect)
{
    int fd[2]; if (pipe(fd)) abort();
    struct region ra, rl;
    uint64_t *a = guarded(frames / 64, &ra), *l = guarded(frames / 64, &rl);
    *expect = (uintptr_t)(a + frames / 64);
    fflush(0);
    pid_t pid = fork(); if (pid < 0) abort();
    if (pid == 0) {
        close(fd[0]); fault_fd = fd[1];
        static uint8_t altstack[65536];
        stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack }; sigaltstack(&ss, 0);
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
        struct pmm p;
        int r = ref_pmm_init_in(&p, a, l, frames, &in->bi, in->has_t ? &in->t : 0);
        _exit(r == PMM_OK ? 10 : 11);   /* completed: no out-of-bounds access happened */
    }
    close(fd[1]);
    uintptr_t addr = 0; ssize_t got = read(fd[0], &addr, sizeof addr); close(fd[0]);
    int st; waitpid(pid, &st, 0);
    release(&ra); release(&rl);
    *at = got == sizeof addr ? addr : 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], 0, 0) : 4000;
    setvbuf(stdout, 0, _IOLBF, 0);   /* a mutant that faults still shows how far it got */
    PG = (size_t)sysconf(_SC_PAGESIZE);
    struct input in;
    /* deterministic maps */
    static const struct mb2_mmap_entry lowonly[] = { { 0, 0x100000, 1, 0 } };
    static const struct mb2_mmap_entry bios[] = { { 0, 0x9fc00, 1, 0 }, { 0x9fc00, 0x400, 2, 0 }, { 0xf0000, 0x10000, 2, 0 }, { 0x100000, 0x3fee0000, 1, 0 },
                                                  { 0x3ffe0000, 0x20000, 2, 0 }, { 0xfffc0000, 0x40000, 2, 0 } };
    static const struct mb2_mmap_entry holes[] = { { 0, 0xa0000, 1, 0 }, { 0x100000, 0x3ef000, 1, 0 }, { 0x180000, 0x2000, 2, 0 }, { 0x3f0000, 0x10000, 1, 0 },
                                                   { 0x400000, 0x40000000, 1, 0 }, { 0x41000000, 0x1000, 3, 0 } };
    static const struct mb2_mmap_entry overlap[] = { { 0x80000, 0x200000, 1, 0 }, { 0x100000, 0x80000, 1, 0 }, { 0x140000, 0x1800, 4, 0 }, { 0x1ff800, 0x2000, 5, 0 } };
    static const struct mb2_mmap_entry overflow[] = { { 0x100000, 0x100000, 1, 0 }, { 0xfffffffffffff000ull, 0x2000, 1, 0 }, { 0xffffffffffff0000ull, 0x20000, 2, 0 },
                                                      { 0x180800, 0x0, 2, 0 }, { 0x1c0800, 0x800, 2, 0 },
                                                      { 0x1f0000, 0xffffffffffff0000ull, 2, 0 } };   /* reserved, wraps past 2^64 from inside the instance */
    static const struct mb2_mmap_entry unaligned[] = { { 0x100800, 0x2800, 1, 0 }, { 0x13f001, 0x2fff, 1, 0 }, { 0x7f000, 0x82000, 1, 0 } };
    static const struct mb2_mmap_entry high[] = { { 0x0, 0x9f000, 1, 0 }, { 0x100000, 0x7ff00000, 1, 0 }, { 0x100000000ull, 0x40000000, 1, 0 } };
    static const struct mb2_mmap_entry none[] = { { 0x0, 0x40000000, 2, 0 } };
    struct { const struct mb2_mmap_entry *e; int n; } maps[] = { { lowonly, 1 }, { bios, 6 }, { holes, 6 }, { overlap, 4 }, { overflow, 6 }, { unaligned, 3 }, { high, 3 }, { none, 1 } };
    const int nmaps = (int)(sizeof maps / sizeof maps[0]);

    /* 1. negative control */
    unsigned faults = 0;
    for (uint64_t frames = 64; frames < 256; frames += 64) {
        map_init(&in, bios, 6);
        uintptr_t at, expect; int rc = negative_control(frames, &in, &at, &expect);
        int ok = rc == 77 && at == expect;
        faults += ok;
        printf("PMM INIT NEGATIVE CONTROL frames=%llu frozen-C=%s fault_offset=%s\n", (unsigned long long)frames,
               rc == 77 ? "fault" : rc == 10 || rc == 11 ? "completed" : "abnormal", ok ? "bitmap_end" : "UNEXPECTED");
        mix(frames); mix((uint64_t)ok);
    }
    /* 2. small instances */
    for (uint64_t frames = 64; frames < 256; frames += 64) {
        for (int m = 0; m < nmaps; m++) {
            map_init(&in, maps[m].e, maps[m].n); run_case(frames, &in, 1); cov_small++;
            map_init(&in, maps[m].e, maps[m].n); resv(&in, 0x1000, 0x3000); resv(&in, frames * PAGE_SIZE - 0x1800, UINT64_MAX); run_case(frames, &in, 1); cov_small++;
        }
        for (unsigned i = 0; i < n / 8; i++) { seeded_map(&in, frames * PAGE_SIZE); run_case(frames, &in, 1); cov_small++; cov_seeded++; }
    }
    /* 3. large instances */
    static const uint64_t fixed[] = { 256, 320, 384, 448, 512, 1024, 4096, 16384, 65536, PMM_MAX_FRAMES };
    for (unsigned f = 0; f < sizeof fixed / sizeof fixed[0]; f++)
        for (int m = 0; m < nmaps; m++) {
            map_init(&in, maps[m].e, maps[m].n); run_case(fixed[f], &in, 0); cov_large++;
            map_init(&in, maps[m].e, maps[m].n); resv(&in, 0xff000, 0x101001); resv(&in, 0x200000, 0x203001); resv(&in, fixed[f] * PAGE_SIZE - 0x800, UINT64_MAX);
            run_case(fixed[f], &in, 0); cov_large++;
            if (fixed[f] == PMM_MAX_FRAMES) cov_max += 2;
        }
    for (unsigned i = 0; i < n; i++) {
        uint64_t k = next() % 8, frames = k < 3 ? 256 + 64 * (next() % 16) : k < 7 ? 64 * (4 + next() % (PMM_MAX_FRAMES / 64 - 3)) : PMM_MAX_FRAMES;
        seeded_map(&in, frames * PAGE_SIZE); run_case(frames, &in, 0); cov_large++; cov_seeded++;
        if (frames == PMM_MAX_FRAMES) cov_max++;
    }
    /* 4. refused initialization: identical results, and no bitmap word written */
    static const uint64_t ef[] = { 0, 1, 63, 64, 100, 128, 192, 255, 256, 4096, PMM_MAX_FRAMES, PMM_MAX_FRAMES + 64, UINT64_MAX & ~63ull };
    for (unsigned f = 0; f < sizeof ef / sizeof ef[0]; f++)
        for (int kind = 0; kind < 9; kind++) {
            map_init(&in, bios, 6);
            const struct boot_info *bi = &in.bi;
            if (kind == 1) bi = 0;
            if (kind == 2) in.bi.mmap = 0;
            if (kind == 3) in.bi.mmap_entries = 0;
            if (kind == 4) in.m.h.entry_version = 1;
            if (kind == 5) in.m.h.entry_size = 16;
            if (kind == 6) in.m.h.entry_size = 28;
            size_t words = ef[f] / 64 <= PMM_WORDS(PMM_MAX_FRAMES) ? (size_t)(ef[f] / 64) : 1;
            if (ef[f] % 64 || !ef[f] || ef[f] > PMM_MAX_FRAMES) words = 4;   /* refused by EINVAL: no bitmap is touched */
            if (kind < 7 && ef[f] % 64 == 0 && ef[f] && ef[f] <= PMM_MAX_FRAMES && kind == 0) continue;   /* the valid cases run above */
            struct region ra, rl, ca, cl;
            uint64_t *a = guarded(words, &ra), *l = guarded(words, &rl), *x = guarded(words, &ca), *y = guarded(words, &cl);
            struct result r = { 0 }, c = { 0 };
            uint64_t *ra_ = kind == 7 ? 0 : a, *rl_ = kind == 8 ? 0 : l, *xa = kind == 7 ? 0 : x, *yl = kind == 8 ? 0 : y;
            r.ret = ref_pmm_init_in(&r.p, ra_, rl_, ef[f], bi, 0);
            c.ret = pmm_init_in(&c.p, xa, yl, ef[f], bi, 0);
            checks++;
            if (c.ret == PMM_OK) fail("refused case accepted", ef[f], (uint64_t)kind);
            compare("refused case wrote a bitmap", ef[f], &r, &c, a, l, x, y, words);
            for (size_t w = 0; w < words; w++) { checks++; if (x[w] != GARBAGE || y[w] != GARBAGE) { fail("refused case wrote a bitmap", ef[f], w); break; } }
            cov_error++;
            release(&ra); release(&rl); release(&ca); release(&cl);
        }
    printf("PMM INIT REGRESSION cases=%llu checks=%llu mismatches=%llu negative_faults=%u/3 digest=%016llx\n", (unsigned long long)cases,
           (unsigned long long)checks, (unsigned long long)mismatches, faults, (unsigned long long)digest);
    printf("PMM INIT COVERAGE small=%llu large=%llu max=%llu refused=%llu seeded=%llu frame256=%llu ignored=%llu reserved=%llu overflow=%llu\n",
           (unsigned long long)cov_small, (unsigned long long)cov_large, (unsigned long long)cov_max, (unsigned long long)cov_error, (unsigned long long)cov_seeded,
           (unsigned long long)cov_frame256, (unsigned long long)cov_ignored, (unsigned long long)cov_reserved, (unsigned long long)cov_overflow);
    return mismatches != 0 || faults != 3;
}
