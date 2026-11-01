/* B1: differential of the Rust boot reservation boundary (resv_init/resv_add/resv_overlaps, boot_mmap_entry/boot_reserve, and
 * the C half of the EFI exclusion: efi_memory_exclude/efi_memory_firmware_frame/efi_memory_strerror/efi_memory_what) against the
 * frozen C references (tests/host/reference/{bootinfo,resv,efi_memory}_ref.c, the accepted sources verbatim, built here under
 * the names *_reference). The reference EFI glue calls the same unchanged Rust vos_efi_exclusions_v1, so what is compared is
 * exactly what B1 moved: marshalling, the reservation table and its provenance, capacity and sink failure.
 *
 * Every call is compared on the returned message (by content) and on the WHOLE reservation table afterwards: the count, every
 * entry's range and reason (by content, or by identity for the EFI reason), and the untouched slots, which must keep their
 * poison. A failing call keeps the entries it added before the error, so a partial table is part of the comparison.
 *
 * Inputs: deterministic sweeps over boundary values of the four physical ranges and the module list on six memory-map shapes,
 * table pre-fill from empty to full, and a bounded seeded random phase with an explicit stopping rule; the same for the table
 * operations and for the EFI glue.
 *
 * Memory safety: every tag is copied into a mapping whose neighbours are PROT_NONE guard pages, once flush with the rear guard
 * and once with the front guard, exactly as long as the tag claims, so a read outside the tag by either implementation faults.
 * Hardening outside the C contract (null or misaligned pointers, overlapping storage, a tag shorter than its entries) is checked
 * for the Rust functions only and is never run through the C references, whose behavior there is undefined.
 *
 * usage: boot-resv-differential     exit 0: no mismatch, every outcome produced. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "bootinfo.h"
#include "efi_memory.h"
#include "resv.h"

/* the frozen references, renamed at compile time */
const char *boot_reserve_reference(const struct boot_info *, uint64_t, uint64_t, struct resv_table *);
const struct mb2_mmap_entry *boot_mmap_entry_reference(const struct boot_info *, uint32_t);
void resv_init_reference(struct resv_table *);
int resv_add_reference(struct resv_table *, uint64_t, uint64_t, const char *);
int resv_overlaps_reference(const struct resv_table *, uint64_t, uint64_t);
int efi_memory_exclude_reference(const struct boot_info *, struct resv_table *, struct efi_memory_report *);
int efi_memory_firmware_frame_reference(const struct boot_info *, const struct resv_table *, uint64_t);
const char *efi_memory_strerror_reference(uint32_t);
extern const char efi_memory_what_reference[];

__attribute__((noreturn)) void panic(const char *message)
{
    fprintf(stderr, "unexpected Rust panic: %s\n", message);
    exit(2);
}

/* The layouts the Rust glue pins, checked against this C compiler. */
_Static_assert(sizeof(struct phys_range) == 24 && offsetof(struct phys_range, end) == 8 && offsetof(struct phys_range, what) == 16, "phys_range");
_Static_assert(sizeof(struct resv_table) == 1928 && offsetof(struct resv_table, r) == 0 && offsetof(struct resv_table, count) == 1920, "resv_table");
_Static_assert(RESV_MAX == 80 && RESV_OK == 0 && RESV_BAD_RANGE == 1 && RESV_FULL == 2, "resv constants");
_Static_assert(sizeof(struct boot_module) == 24 && offsetof(struct boot_module, end) == 8, "boot_module");
_Static_assert(sizeof(struct boot_info) == 1624 && offsetof(struct boot_info, info_end) == 8 && offsetof(struct boot_info, mmap) == 32, "boot_info 1");
_Static_assert(offsetof(struct boot_info, mmap_entries) == 40 && offsetof(struct boot_info, modules) == 48 &&
               offsetof(struct boot_info, module_count) == 1584, "boot_info 2");
_Static_assert(offsetof(struct boot_info, efi) == 1604 && offsetof(struct boot_info, efi_mmap) == 1608 &&
               offsetof(struct boot_info, efi_mmap_tags) == 1616 && offsetof(struct boot_info, efi_bs_active) == 1620, "boot_info 3");
_Static_assert(sizeof(struct mb2_tag_mmap) == 16 && sizeof(struct mb2_mmap_entry) == 24 && BOOT_MAX_MODULES == 64, "mmap tag");
_Static_assert(sizeof(struct efi_memory_input) == 56 && sizeof(struct efi_memory_report) == 32, "efi layouts");

#define PAGE 4096u
#define DATA (16 * PAGE)
struct arena {
    uint8_t *front, *data, *rear;
};
static struct arena A1, A2; /* the memory-map tag, the EFI tag */
static unsigned long cases;
static int in_random;
static uint64_t rng = 0x9E3779B97F4A7C15ull;

static struct arena make_arena(void)
{
    uint8_t *m = mmap(NULL, PAGE + DATA + PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m + PAGE, DATA, PROT_READ | PROT_WRITE)) {
        perror("mmap");
        exit(2);
    }
    return (struct arena){m, m + PAGE, m + PAGE + DATA};
}

static uint64_t next(void) /* splitmix64 */
{
    uint64_t z = (rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t below(uint64_t n) { return n ? next() % n : 0; }

static uint8_t *place(const struct arena *a, const uint8_t *bytes, size_t n, int where)
{
    uint8_t *p = where ? a->data : a->data + DATA - n;
    memcpy(p, bytes, n);
    return p;
}

__attribute__((noreturn)) static void fail(const char *what, const char *detail)
{
    fprintf(stderr, "MISMATCH (%s): %s\n", what, detail);
    exit(1);
}

__attribute__((format(printf, 2, 3), noreturn)) static void failf(const char *what, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fail(what, buf);
}

/* ---- the reservation table ---------------------------------------------------------------------------------------- */

static const char PREWHAT[] = "prefill";
static const unsigned char POISON = 0xA5;

static void poison_table(struct resv_table *t, unsigned prefill)
{
    memset(t, POISON, sizeof *t);
    t->count = prefill;
    for (unsigned i = 0; i < prefill && i < RESV_MAX; i++)
        t->r[i] = (struct phys_range){0x100000000ull + (uint64_t)i * PAGE, 0x100000000ull + (uint64_t)i * PAGE + PAGE, PREWHAT};
}

/* The reason of an entry: equal by content, or both the EFI mark of their own implementation. */
static int what_eq(const char *x, const char *y)
{
    int ex = x == efi_memory_what, ey = y == efi_memory_what_reference;
    if (ex || ey)
        return ex && ey;
    return x == y || (x && y && !strcmp(x, y));
}

static void compare_tables(const char *op, const struct resv_table *p, const struct resv_table *q)
{
    if (p->count != q->count)
        failf(op, "count Rust=%zu C=%zu", p->count, q->count);
    for (size_t i = 0; i < p->count && i < RESV_MAX; i++) {
        if (p->r[i].start != q->r[i].start || p->r[i].end != q->r[i].end)
            failf(op, "entry %zu Rust=[%#llx,%#llx) C=[%#llx,%#llx)", i, (unsigned long long)p->r[i].start, (unsigned long long)p->r[i].end,
                  (unsigned long long)q->r[i].start, (unsigned long long)q->r[i].end);
        if (!what_eq(p->r[i].what, q->r[i].what))
            failf(op, "entry %zu reason Rust=%s C=%s", i, p->r[i].what ? p->r[i].what : "(null)", q->r[i].what ? q->r[i].what : "(null)");
    }
    for (size_t i = p->count < RESV_MAX ? p->count : RESV_MAX; i < RESV_MAX; i++) {
        const unsigned char *a = (const unsigned char *)&p->r[i], *b = (const unsigned char *)&q->r[i];
        for (size_t k = 0; k < sizeof p->r[i]; k++)
            if (a[k] != POISON || b[k] != POISON)
                failf(op, "slot %zu beyond the count was written", i);
    }
}

/* ---- boot_reserve ------------------------------------------------------------------------------------------------- */

static const char *MSG[] = {
    "no memory map", "reservation table full", "kernel image below 1 MiB", "kernel image not in usable RAM",
    "multiboot2 info in physical page 0", "multiboot2 info in the legacy hole (0xA0000-1 MiB)",
    "multiboot2 info shares a page with the kernel image", "multiboot2 info not in usable RAM", "module overlaps the multiboot2 info",
    "modules overlap", "module in physical page 0", "module in the legacy hole (0xA0000-1 MiB)", "module overlaps the kernel image",
    "module not in usable RAM",
};
#define NMSG (sizeof MSG / sizeof MSG[0])
#define NOUT (NMSG + 1) /* 0: success, i + 1: MSG[i] */
static unsigned long hist[NOUT], hist_random[NOUT];

static int outcome(const char *s)
{
    if (!s)
        return 0;
    for (unsigned i = 0; i < NMSG; i++)
        if (!strcmp(s, MSG[i]))
            return (int)i + 1;
    return -1;
}

#define MAXE 12
#define MAXMOD 64
struct Scn {
    uint32_t es, n;
    struct {
        uint64_t base, len;
        uint32_t type;
    } e[MAXE];
    uint64_t ks, ke, is, ie;
    unsigned nmod;
    uint64_t ms[MAXMOD], me[MAXMOD];
    unsigned prefill;
    int null_mmap;
};

static size_t build_map(const struct Scn *s, uint8_t *out)
{
    uint32_t size = 16 + s->n * s->es, type = 6, version = 0;
    memset(out, 0, size);
    memcpy(out, &type, 4);
    memcpy(out + 4, &size, 4);
    memcpy(out + 8, &s->es, 4);
    memcpy(out + 12, &version, 4);
    for (uint32_t i = 0; i < s->n; i++) {
        uint8_t *p = out + 16 + (size_t)i * s->es;
        memcpy(p, &s->e[i].base, 8);
        memcpy(p + 8, &s->e[i].len, 8);
        memcpy(p + 16, &s->e[i].type, 4);
        for (uint32_t k = 24; k < s->es; k++)
            p[k] = (uint8_t)(0xC0 + k); /* junk beyond the 24 bytes the code reads */
    }
    return size;
}

static void fill_bi(struct boot_info *bi, const struct Scn *s, const uint8_t *tag)
{
    memset(bi, 0, sizeof *bi);
    bi->mmap = (const struct mb2_tag_mmap *)tag;
    bi->mmap_entries = s->n;
    bi->info_start = s->is, bi->info_end = s->ie;
    bi->module_count = s->nmod;
    for (unsigned i = 0; i < s->nmod; i++)
        bi->modules[i] = (struct boot_module){s->ms[i], s->me[i], "m"};
}

static void note_class(int oc, const struct Scn *s);

static void run_reserve(const struct Scn *s)
{
    static struct boot_info bi;
    static struct resv_table ta, tb;
    uint8_t tag[16 + MAXE * 64];
    size_t n = build_map(s, tag);

    cases++;
    for (int where = 0; where < 2; where++) {
        fill_bi(&bi, s, s->null_mmap ? NULL : place(&A1, tag, n, where));
        poison_table(&ta, s->prefill);
        poison_table(&tb, s->prefill);
        const char *x = boot_reserve(&bi, s->ks, s->ke, &ta);
        const char *y = boot_reserve_reference(&bi, s->ks, s->ke, &tb);
        int ox = outcome(x), oy = outcome(y);
        if (ox < 0 || oy < 0 || ox != oy)
            failf("boot_reserve result", "kernel [%#llx,%#llx) info [%#llx,%#llx) modules %u prefill %u Rust=%s C=%s", (unsigned long long)s->ks,
                  (unsigned long long)s->ke, (unsigned long long)s->is, (unsigned long long)s->ie, s->nmod, s->prefill,
                  x ? x : "(success)", y ? y : "(success)");
        compare_tables("boot_reserve table", &ta, &tb);
        if (!s->null_mmap)
            for (uint32_t i = 0; i < s->n + 3; i++)
                if (boot_mmap_entry(&bi, i) != boot_mmap_entry_reference(&bi, i))
                    failf("boot_mmap_entry", "entry %u", i);
        if (where == 0) {
            hist[ox]++;
            if (in_random)
                hist_random[ox]++;
            note_class(ox, s);
        }
    }
}

/* ---- map shapes and boundary values ------------------------------------------------------------------------------- */

static const uint64_t SMALL[] = {0x0, 0x1000, 0x9F000, 0xA0000, 0xFFFFF, 0x100000, 0x1FFFFF, 0x200000, 0x2FFFFF, 0x300000, 0x3FFFFFFF, 0x40000000};
static const uint64_t ADDR[] = {0x0, 0x1, 0xFFF, 0x1000, 0x1001, 0x64000, 0x9F000, 0x9FC00, 0xA0000, 0xA0001, 0xFFFFF, 0x100000, 0x100001,
                                0x1FFFFF, 0x200000, 0x200001, 0x2FFFFF, 0x300000, 0x3FFFF000, 0x3FFFFFFF, 0x40000000, 0x40000001,
                                0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFF000ull, 0xFFFFFFFFFFFFFFFFull};
#define NSMALL (sizeof SMALL / sizeof SMALL[0])
#define NADDR (sizeof ADDR / sizeof ADDR[0])

static struct Scn shape(unsigned k)
{
    struct Scn s = {.es = 24, .ks = 0x200000, .ke = 0x300000, .is = 0x68000, .ie = 0x68100};
    switch (k) {
    case 0: /* a BIOS e820 map */
        s.n = 5;
        s.e[0] = (typeof(s.e[0])){0x0, 0x9FC00, 1};
        s.e[1] = (typeof(s.e[0])){0x9FC00, 0x400, 2};
        s.e[2] = (typeof(s.e[0])){0xF0000, 0x10000, 2};
        s.e[3] = (typeof(s.e[0])){0x100000, 0x7EE0000, 1};
        s.e[4] = (typeof(s.e[0])){0xFFFC0000ull, 0x40000, 2};
        break;
    case 1: /* a UEFI-like map: RAM below 1 MiB, holes, ACPI */
        s.n = 7, s.es = 32;
        s.e[0] = (typeof(s.e[0])){0x0, 0xA0000, 1};
        s.e[1] = (typeof(s.e[0])){0x100000, 0x700000, 1};
        s.e[2] = (typeof(s.e[0])){0x800000, 0x1000, 2};
        s.e[3] = (typeof(s.e[0])){0x801000, 0x3F7FF000ull, 1};
        s.e[4] = (typeof(s.e[0])){0x3FFFF000ull, 0x1000, 3};
        s.e[5] = (typeof(s.e[0])){0x40000000ull, 0x40000000ull, 1};
        s.e[6] = (typeof(s.e[0])){0xFEE00000ull, 0x1000, 2};
        break;
    case 2: /* entries that overflow 2^64 */
        s.n = 4;
        s.e[0] = (typeof(s.e[0])){0x0, 0xA0000, 1};
        s.e[1] = (typeof(s.e[0])){0x100000, 0x3FF00000, 1};
        s.e[2] = (typeof(s.e[0])){0xFFFFFFFFFFFFF000ull, 0x2000, 1};
        s.e[3] = (typeof(s.e[0])){0xFFFFFFFFFFFFF000ull, 0x2000, 2};
        break;
    case 3: /* zero-length and adjacent entries */
        s.n = 6, s.es = 40;
        s.e[0] = (typeof(s.e[0])){0x0, 0xA0000, 1};
        s.e[1] = (typeof(s.e[0])){0x100000, 0, 1};
        s.e[2] = (typeof(s.e[0])){0x100000, 0x1000, 1};
        s.e[3] = (typeof(s.e[0])){0x101000, 0x1FF000, 1};
        s.e[4] = (typeof(s.e[0])){0x300000, 0, 2};
        s.e[5] = (typeof(s.e[0])){0x300000, 0x10000000, 1};
        break;
    case 4: /* RAM with a reserved hole under the kernel and an unknown type */
        s.n = 5;
        s.e[0] = (typeof(s.e[0])){0x0, 0x9F000, 1};
        s.e[1] = (typeof(s.e[0])){0x100000, 0x100000, 1};
        s.e[2] = (typeof(s.e[0])){0x200000, 0x1000, 99};
        s.e[3] = (typeof(s.e[0])){0x201000, 0xFE000, 1};
        s.e[4] = (typeof(s.e[0])){0x2FF000, 0x200000, 4};
        break;
    default: /* no entries at all */
        s.n = 0;
        break;
    }
    return s;
}
#define NSHAPES 6

/* a layout that succeeds on shape 0, 1, 3 */
static void good(struct Scn *s, unsigned variant)
{
    s->ks = 0x200000, s->ke = 0x2A0000;
    s->is = variant & 1 ? 0x68000 : 0x2B0000;
    s->ie = s->is + 0x100;
    s->nmod = variant & 2 ? 2 : 0;
    s->ms[0] = variant & 1 ? 0x64000 : 0x2C0000, s->me[0] = s->ms[0] + 0x800;
    s->ms[1] = 0x2D0000, s->me[1] = 0x2D0100;
}

static unsigned long classes_seen;
static unsigned char cls[NOUT][4][2];
static unsigned long last_new;

static void note_class(int oc, const struct Scn *s)
{
    unsigned m = s->nmod > 3 ? 3 : s->nmod;
    unsigned char *c = &cls[oc][m][s->prefill >= 77];
    if (!*c) {
        *c = 1;
        classes_seen++;
        last_new = cases;
    }
}

static void sweep_a(void) /* kernel and info ranges over the small boundary set on every shape */
{
    for (unsigned sh = 0; sh < NSHAPES; sh++)
        for (unsigned a = 0; a < NSMALL; a++)
            for (unsigned b = 0; b < NSMALL; b++)
                for (unsigned c = 0; c < NSMALL; c++)
                    for (unsigned d = 0; d < NSMALL; d++) {
                        struct Scn s = shape(sh);
                        s.ks = SMALL[a], s.ke = SMALL[b], s.is = SMALL[c], s.ie = SMALL[d];
                        run_reserve(&s);
                    }
}

static void sweep_b(void) /* each single value over the full boundary set, the rest good */
{
    for (unsigned sh = 0; sh < NSHAPES; sh++)
        for (unsigned v = 0; v < 4; v++)
            for (unsigned i = 0; i < NADDR; i++)
                for (unsigned w = 0; w < 4; w++) {
                    struct Scn s = shape(sh);
                    good(&s, w);
                    (v == 0 ? &s.ks : v == 1 ? &s.ke : v == 2 ? &s.is : &s.ie)[0] = ADDR[i];
                    run_reserve(&s);
                    if (v == 0 || v == 1)
                        continue;
                    /* an info range moved: also with a kernel range at the extremes of the set */
                    s.ks = ADDR[i], s.ke = ADDR[(i + 7) % NADDR];
                    run_reserve(&s);
                }
}

static void sweep_pairs(void) /* the kernel range, then the info range, over every pair of the full set */
{
    for (unsigned sh = 0; sh < NSHAPES; sh++)
        for (unsigned w = 0; w < 2; w++)
            for (unsigned a = 0; a < NADDR; a++)
                for (unsigned b = 0; b < NADDR; b++) {
                    struct Scn s = shape(sh);
                    good(&s, w);
                    s.ks = ADDR[a], s.ke = ADDR[b];
                    run_reserve(&s);
                    s = shape(sh);
                    good(&s, w);
                    s.is = ADDR[a], s.ie = ADDR[b];
                    run_reserve(&s);
                }
}

static void sweep_modules(void) /* one module over the full set, then two */
{
    for (unsigned sh = 0; sh < NSHAPES; sh++)
        for (unsigned w = 0; w < 2; w++) {
            for (unsigned a = 0; a < NADDR; a++)
                for (unsigned b = 0; b < NADDR; b++) {
                    struct Scn s = shape(sh);
                    good(&s, w);
                    s.nmod = 1, s.ms[0] = ADDR[a], s.me[0] = ADDR[b];
                    run_reserve(&s);
                }
            if (sh == 2 || sh == 4 || sh == 5)
                continue; /* two modules: the shapes with ordinary RAM; the odd shapes get the one-module sweep above */
            for (unsigned a = 0; a < NSMALL; a++)
                for (unsigned b = 0; b < NSMALL; b++)
                    for (unsigned c = 0; c < NSMALL; c++)
                        for (unsigned d = 0; d < NSMALL; d++) {
                            struct Scn s = shape(sh);
                            good(&s, w);
                            s.nmod = 2, s.ms[0] = SMALL[a], s.me[0] = SMALL[b], s.ms[1] = SMALL[c], s.me[1] = SMALL[d];
                            run_reserve(&s);
                        }
        }
}

static void sweep_prefill(void) /* the table full at every point of the sequence, with 0, 1, 2 and many modules */
{
    for (unsigned sh = 0; sh < 2; sh++)
        for (unsigned w = 0; w < 4; w++)
            for (unsigned pre = 0; pre <= RESV_MAX + 1; pre++) {
                for (unsigned m = 0; m < 3; m++) {
                    struct Scn s = shape(sh);
                    good(&s, w & 1);
                    s.nmod = m;
                    for (unsigned i = 2; i < m; i++) /* more modules, all distinct, inside the second RAM block */
                        s.ms[i] = 0x2E0000 + i * 0x1000, s.me[i] = s.ms[i] + 0x100;
                    s.prefill = pre;
                    run_reserve(&s);
                }
                struct Scn many = shape(1);
                good(&many, 1);
                many.nmod = MAXMOD; /* 64 modules: the table limit is hit part-way */
                for (unsigned i = 0; i < MAXMOD; i++)
                    many.ms[i] = 0x400000 + i * 0x2000, many.me[i] = many.ms[i] + 0x100;
                many.prefill = pre;
                run_reserve(&many);
            }
}

static void sweep_entries(void) /* every entry type, length and base class at the one RAM entry the kernel needs */
{
    static const uint32_t types[] = {0, 1, 2, 3, 4, 5, 99, 0xFFFFFFFFu};
    static const uint64_t lens[] = {0, 1, 0xFFF, 0x1000, 0x100000, 0x3FF00000, 0xFFFFFFFFFFFFF000ull, 0xFFFFFFFFFFFFFFFFull};
    static const uint64_t bases[] = {0, 0xFFF, 0x100000, 0x1FF000, 0x200000, 0x2A0000, 0x300000, 0xFFFFFFFFFFFFF000ull};
    for (unsigned t0 = 0; t0 < 8; t0++)
        for (unsigned l0 = 0; l0 < 8; l0++)
            for (unsigned b0 = 0; b0 < 8; b0++)
                for (unsigned t1 = 0; t1 < 8; t1++)
                    for (unsigned l1 = 0; l1 < 8; l1 += 2) {
                        struct Scn s = shape(0);
                        good(&s, 1);
                        s.n = 3;
                        s.e[0] = (typeof(s.e[0])){0x0, 0x9F000, 1};
                        s.e[1] = (typeof(s.e[0])){bases[b0], lens[l0], types[t0]};
                        s.e[2] = (typeof(s.e[0])){0x100000, lens[l1], types[t1]};
                        run_reserve(&s);
                    }
}

static void sweep_null(void) /* no memory-map tag at all, with the table empty, nearly full and full */
{
    for (unsigned pre = 0; pre <= RESV_MAX; pre += 40) {
        struct Scn s = shape(0);
        good(&s, 3);
        s.null_mmap = 1, s.prefill = pre;
        run_reserve(&s);
    }
}

/* ---- the random phase --------------------------------------------------------------------------------------------- */

static uint64_t pick_addr(void)
{
    switch (below(5)) {
    case 0:
        return ADDR[below(NADDR)];
    case 1:
        return below(0x41000000) & ~0xFFFull;
    case 2:
        return below(0x41000000);
    case 3:
        return ADDR[below(NADDR)] + below(5) - 2;
    default:
        return below(0x400000) & ~0xFFull;
    }
}

static void random_scn(struct Scn *s)
{
    static const uint32_t types[] = {1, 1, 1, 1, 2, 2, 3, 4, 5, 0, 99};
    static const uint32_t sizes[] = {24, 24, 24, 32, 40, 48};
    memset(s, 0, sizeof *s);
    s->es = sizes[below(6)];
    s->n = (uint32_t)below(MAXE + 1);
    for (uint32_t i = 0; i < s->n; i++) {
        s->e[i].base = below(4) ? (below(2) ? below(0x41000000) & ~0xFFFull : pick_addr()) : ADDR[below(NADDR)];
        s->e[i].len = below(6) ? (below(0x40000000) & ~0xFFFull) + (below(4) ? 0 : below(4096)) : ADDR[below(NADDR)];
        s->e[i].type = types[below(11)];
    }
    /* a constructive core: a RAM block that holds a good layout, then a mutation of one field */
    if (below(10) < 7 && s->n >= 2) {
        s->e[1] = (typeof(s->e[0])){0x100000 + (below(8) << 12), 0x2000000 + below(0x3000000), 1};
        s->e[0] = (typeof(s->e[0])){0x0, below(2) ? 0xA0000 : 0x9F000, 1};
    }
    uint64_t ram = s->n >= 2 ? s->e[1].base : 0x100000;
    s->ks = ram + (below(4) << 12), s->ke = s->ks + 0x1000 + below(0x40000);
    s->is = below(2) ? 0x68000 : s->ke + below(0x10000), s->ie = s->is + 1 + below(0x400);
    s->nmod = below(8) < 5 ? (unsigned)below(4) : (unsigned)below(MAXMOD + 1);
    for (unsigned i = 0; i < s->nmod; i++)
        s->ms[i] = (below(2) ? 0x64000 : s->ke + 0x20000) + i * 0x1000 + below(3), s->me[i] = s->ms[i] + 1 + below(0x900);
    if (below(2))
        switch (below(6)) {
        case 0: s->ks = pick_addr(); break;
        case 1: s->ke = pick_addr(); break;
        case 2: s->is = pick_addr(); break;
        case 3: s->ie = pick_addr(); break;
        case 4: if (s->nmod) s->ms[below(s->nmod)] = pick_addr(); break;
        default: if (s->nmod) s->me[below(s->nmod)] = pick_addr(); break;
        }
    s->prefill = below(8) == 0 ? (unsigned)(RESV_MAX - below(5) - (below(2) ? 0 : 3)) : below(12) == 0 ? (unsigned)below(RESV_MAX + 1) : 0;
    s->null_mmap = below(60) == 0;
}

static void random_phase(unsigned long minimum, unsigned long cap, unsigned long plateau)
{
    unsigned long done = 0;
    int converged = 0;

    in_random = 1;
    for (;;) {
        struct Scn s;
        random_scn(&s);
        run_reserve(&s);
        done++;
        int every = 1;
        for (unsigned i = 0; i < NOUT; i++)
            if (hist[i] < 10)
                every = 0;
        if (done >= minimum && cases - last_new >= plateau && every) {
            converged = 1;
            break;
        }
        if (done >= cap)
            break;
    }
    in_random = 0;
    printf("random phase: %lu inputs; %lu structural classes (outcome x modules x table fill), last new at input %lu; %s\n", done,
           classes_seen, last_new, converged ? "stopped on the plateau rule with every outcome seen at least 10 times" : "ended at the cap, NOT converged");
}

/* ---- resv_init / resv_add / resv_overlaps ------------------------------------------------------------------------- */

static unsigned long table_cases;

static void table_ops(void)
{
    static struct resv_table ta, tb;
    static const unsigned counts[] = {0, 1, 2, 78, 79, 80, 81, 200};
    /* the boundary pairs, on empty, nearly full, full and out-of-range counts */
    for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; c++)
        for (unsigned a = 0; a < NADDR; a++)
            for (unsigned b = 0; b < NADDR; b++) {
                poison_table(&ta, counts[c] > RESV_MAX ? RESV_MAX : counts[c]);
                ta.count = counts[c];
                poison_table(&tb, counts[c] > RESV_MAX ? RESV_MAX : counts[c]);
                tb.count = counts[c];
                int x = resv_add(&ta, ADDR[a], ADDR[b], "w"), y = resv_add_reference(&tb, ADDR[a], ADDR[b], "w");
                table_cases++;
                if (x != y)
                    failf("resv_add", "count %u [%#llx,%#llx) Rust=%d C=%d", counts[c], (unsigned long long)ADDR[a], (unsigned long long)ADDR[b], x, y);
                compare_tables("resv_add table", &ta, &tb);
            }
    /* resv_init leaves everything but the count alone */
    poison_table(&ta, 5);
    poison_table(&tb, 5);
    resv_init(&ta);
    resv_init_reference(&tb);
    if (memcmp(&ta, &tb, sizeof ta) || ta.count != 0 || ta.r[0].what != PREWHAT)
        fail("resv_init", "differs from the reference, or touched more than the count");
    /* random sequences of adds, then queries over the boundary set */
    for (unsigned round = 0; round < 400; round++) {
        poison_table(&ta, 0);
        poison_table(&tb, 0);
        resv_init(&ta);
        resv_init_reference(&tb);
        unsigned adds = round % 5 == 0 ? RESV_MAX + 3 : (unsigned)below(20);
        for (unsigned i = 0; i < adds; i++) {
            uint64_t s = pick_addr(), e = below(3) ? s + below(0x40000) : pick_addr();
            int x = resv_add(&ta, s, e, "r"), y = resv_add_reference(&tb, s, e, "r");
            table_cases++;
            if (x != y)
                failf("resv_add sequence", "[%#llx,%#llx) Rust=%d C=%d", (unsigned long long)s, (unsigned long long)e, x, y);
        }
        compare_tables("resv_add sequence table", &ta, &tb);
        for (unsigned a = 0; a < NADDR; a++)
            for (unsigned b = 0; b < NADDR; b++) {
                table_cases++;
                int x = resv_overlaps(&ta, ADDR[a], ADDR[b]), y = resv_overlaps_reference(&tb, ADDR[a], ADDR[b]);
                if ((x != 0) != (y != 0))
                    failf("resv_overlaps", "[%#llx,%#llx) Rust=%d C=%d", (unsigned long long)ADDR[a], (unsigned long long)ADDR[b], x, y);
            }
        for (unsigned q = 0; q < 200; q++) {
            uint64_t s = pick_addr(), e = s + below(0x20000);
            table_cases++;
            if ((resv_overlaps(&ta, s, e) != 0) != (resv_overlaps_reference(&tb, s, e) != 0))
                failf("resv_overlaps random", "[%#llx,%#llx)", (unsigned long long)s, (unsigned long long)e);
        }
    }
}

/* ---- the EFI glue ------------------------------------------------------------------------------------------------- */

#define WB (1ull << 3)
#define RUNTIME (1ull << 63)
#define SPECIFIC (1ull << 18)
struct Efi {
    struct Scn map; /* the tag-6 map */
    uint32_t ds, version, ndesc, tag_type_delta;
    struct {
        uint32_t type;
        uint64_t start, pages, attr;
    } d[24];
    int uefi, bs_active, no_efi_map, null_mmap;
    uint32_t tags;
    unsigned prefill;
    int trunc, odd_size;
};
static unsigned long hist_efi[4]; /* applied, not-efi, refused, other */
static unsigned long efi_errors[16];

static size_t build_efi(const struct Efi *f, uint8_t *out)
{
    uint32_t ds = f->ds, size = 16 + f->ndesc * ds + (f->odd_size ? 4 : 0), type = 17 + f->tag_type_delta;
    memset(out, 0, size);
    memcpy(out, &type, 4);
    memcpy(out + 4, &size, 4);
    memcpy(out + 8, &ds, 4);
    memcpy(out + 12, &f->version, 4);
    for (uint32_t i = 0; i < f->ndesc; i++) {
        uint8_t *p = out + 16 + (size_t)i * ds;
        memcpy(p, &f->d[i].type, 4);
        memcpy(p + 8, &f->d[i].start, 8);
        memcpy(p + 24, &f->d[i].pages, 8);
        memcpy(p + 32, &f->d[i].attr, 8);
    }
    return size;
}

static void compare_reports(const struct efi_memory_report *a, const struct efi_memory_report *b)
{
    if (memcmp(a, b, sizeof *a))
        failf("efi report", "frames %llu/%llu status %d/%d error %u/%u desc %u/%u ranges %u/%u coalesced %u/%u", (unsigned long long)a->frames,
              (unsigned long long)b->frames, a->status, b->status, a->error, b->error, a->descriptors, b->descriptors, a->ranges, b->ranges,
              a->coalesced, b->coalesced);
}

static void run_efi(const struct Efi *f)
{
    static struct boot_info bi;
    static struct resv_table ta, tb;
    static uint8_t mtag[16 + MAXE * 64], etag[16 + 24 * 1032 + 4];
    size_t mn = build_map(&f->map, mtag), en = build_efi(f, etag);

    cases++;
    for (int where = 0; where < 2; where++) {
        fill_bi(&bi, &f->map, f->null_mmap ? NULL : place(&A1, mtag, mn, where));
        bi.efi = f->uefi, bi.efi_bs_active = f->bs_active;
        bi.efi_mmap_tags = f->tags;
        bi.efi_mmap = f->no_efi_map ? NULL : (const struct mb2_tag *)place(&A2, etag, en, where);
        poison_table(&ta, f->prefill);
        poison_table(&tb, f->prefill);
        struct efi_memory_report ra, rb;
        memset(&ra, 0x5A, sizeof ra);
        memset(&rb, 0x5A, sizeof rb);
        int x = efi_memory_exclude(&bi, &ta, &ra), y = efi_memory_exclude_reference(&bi, &tb, &rb);
        if (x != y)
            failf("efi_memory_exclude", "Rust=%d C=%d (error %u/%u)", x, y, ra.error, rb.error);
        compare_reports(&ra, &rb);
        compare_tables("efi_memory_exclude table", &ta, &tb);
        /* the firmware-frame answer for every page around each entry, and for entries re-labelled as not EFI */
        for (size_t i = 0; i < ta.count && i < RESV_MAX; i++) {
            uint64_t frames[] = {ta.r[i].start - PAGE, ta.r[i].start, ta.r[i].start + PAGE, ta.r[i].end - PAGE, ta.r[i].end, 0xFFFFFFFFFFFFF000ull};
            for (unsigned k = 0; k < sizeof frames / sizeof frames[0]; k++) {
                int p = efi_memory_firmware_frame(&bi, &ta, frames[k]), q = efi_memory_firmware_frame_reference(&bi, &tb, frames[k]);
                if (p != q)
                    failf("efi_memory_firmware_frame", "frame %#llx Rust=%d C=%d", (unsigned long long)frames[k], p, q);
            }
        }
        if (ta.count && ta.count <= RESV_MAX) {
            const char *keep_a = ta.r[0].what, *keep_b = tb.r[0].what;
            ta.r[0].what = tb.r[0].what = PREWHAT; /* the same range, no longer marked as a firmware exclusion */
            if (efi_memory_firmware_frame(&bi, &ta, ta.r[0].start) != 0 || efi_memory_firmware_frame_reference(&bi, &tb, tb.r[0].start) != 0)
                fail("efi_memory_firmware_frame provenance", "a relabelled entry was accepted");
            ta.r[0].what = keep_a, tb.r[0].what = keep_b;
        }
        if (where == 0) {
            hist_efi[x == 0 ? 0 : x == 1 ? 1 : x == -1 ? 2 : 3]++;
            if (rb.error < 16)
                efi_errors[rb.error]++;
        }
    }
}

static struct Efi efi_base(void)
{
    struct Efi f = {.ds = 48, .version = 1, .uefi = 1, .tags = 1};
    f.map = shape(1);
    f.ndesc = 5;
    f.d[0] = (typeof(f.d[0])){1, 0x100000, 0x100, WB};
    f.d[1] = (typeof(f.d[0])){2, 0x200000, 0x80, WB};
    f.d[2] = (typeof(f.d[0])){7, 0x300000, 0x200, WB};
    f.d[3] = (typeof(f.d[0])){6, 0x500000, 0x10, WB | RUNTIME};
    f.d[4] = (typeof(f.d[0])){7, 0x600000, 0x40, WB};
    return f;
}

static void efi_sweep(void)
{
    struct Efi f;
    /* the failure modes of the evidence, then the table fill from empty to full */
    f = efi_base(), f.uefi = 0, f.tags = 0, f.no_efi_map = 1; /* legacy BIOS: the e820 map alone applies */
    run_efi(&f);
    f = efi_base(), f.uefi = 0;
    run_efi(&f);
    f = efi_base(), f.no_efi_map = 1, f.tags = 0;
    run_efi(&f);
    f = efi_base(), f.bs_active = 1;
    run_efi(&f);
    f = efi_base(), f.tags = 2;
    run_efi(&f);
    f = efi_base(), f.null_mmap = 1;
    run_efi(&f);
    f = efi_base(), f.map.es = 24, f.map.n = 0;
    run_efi(&f);
    for (unsigned k = 0; k < 8; k++) {
        f = efi_base();
        switch (k) {
        case 0: f.tag_type_delta = 1; break;
        case 1: f.ds = 40 - 8; break;
        case 2: f.ds = 1032; break;
        case 3: f.ds = 52; break;
        case 4: f.version = 2; break;
        case 5: f.ndesc = 0; break;
        case 6: f.odd_size = 1; break;
        default: f.d[2].start += 0x10; break;
        }
        run_efi(&f);
    }
    f = efi_base(), f.d[1].pages = 0xFFFFFFFFFFFFFFFFull;
    run_efi(&f);
    f = efi_base(), f.d[1].start = 0xFFFFFFFFFFFFF000ull, f.d[1].pages = 2;
    run_efi(&f);
    for (unsigned pre = 0; pre <= RESV_MAX; pre++) /* a count above RESV_MAX makes the C read past the table */
        for (unsigned w = 0; w < 3; w++) {
            f = efi_base(), f.prefill = pre;
            if (w == 1) /* many separate ranges: the sink fails part-way */
                for (unsigned i = 0; i < 12; i++)
                    f.d[i] = (typeof(f.d[0])){i % 2 ? 7u : 1u, 0x100000 + (uint64_t)i * 0x20000, 0x10, WB}, f.ndesc = 12;
            if (w == 2) /* descriptors that coalesce, and one that is not usable */
                f.d[1] = (typeof(f.d[0])){7, 0x200000, 0x80, WB}, f.d[3] = (typeof(f.d[0])){7, 0x500000, 0x10, SPECIFIC};
            run_efi(&f);
        }
}

static void efi_random(unsigned long count)
{
    static const uint32_t types[] = {1, 2, 7, 7, 0, 5, 6, 3, 14};
    static const uint64_t attrs[] = {WB, WB, WB, WB | RUNTIME, 0, WB | SPECIFIC, 1};
    for (unsigned long i = 0; i < count; i++) {
        struct Efi f = efi_base();
        f.map = shape((unsigned)below(NSHAPES));
        f.ndesc = (uint32_t)(1 + below(12));
        f.ds = below(8) == 0 ? 48 + 8 * (uint32_t)below(4) : 48;
        for (uint32_t k = 0; k < f.ndesc; k++)
            f.d[k] = (typeof(f.d[0])){types[below(9)], (below(4) ? below(0x4000) : below(0x100000)) << 12, below(10) ? 1 + below(0x400) : below(4), attrs[below(7)]};
        f.uefi = below(12) != 0;
        f.bs_active = below(25) == 0;
        f.tags = below(25) == 0 ? 2 : 1;
        f.prefill = below(4) == 0 ? (unsigned)(RESV_MAX - below(15)) : 0;
        f.no_efi_map = below(30) == 0;
        if (!f.uefi && below(2))
            f.tags = 0, f.no_efi_map = 1;
        f.null_mmap = below(40) == 0;
        if (below(20) == 0)
            f.d[below(f.ndesc)].start |= 0x10;
        run_efi(&f);
    }
}

static void strerror_all(void)
{
    for (uint32_t e = 0; e < 40; e++)
        if (strcmp(efi_memory_strerror(e), efi_memory_strerror_reference(e)))
            failf("efi_memory_strerror", "code %u Rust=%s C=%s", e, efi_memory_strerror(e), efi_memory_strerror_reference(e));
    if (strcmp(efi_memory_strerror(0xFFFFFFFFu), "unknown") || strcmp(efi_memory_what, efi_memory_what_reference) ||
        strlen(efi_memory_what) != 21 || (const void *)efi_memory_what == (const void *)efi_memory_what_reference)
        fail("efi_memory_what", "text differs or the two objects are one");
}

/* ---- hardening outside the C contract: the Rust functions only ----------------------------------------------------- */

#define EXPECT(cond, what)                                                                                                                       \
    do {                                                                                                                                         \
        if (!(cond))                                                                                                                             \
            fail("hardening", what);                                                                                                             \
    } while (0)

static void hardening(void)
{
    static struct boot_info bi __attribute__((aligned(8)));
    static struct resv_table t __attribute__((aligned(8))), keep __attribute__((aligned(8)));
    static uint8_t big[8192] __attribute__((aligned(8)));
    struct Scn s = shape(0);
    uint8_t tag[16 + MAXE * 64];
    size_t n;
    struct efi_memory_report rep;

    good(&s, 0);
    n = build_map(&s, tag);
    fill_bi(&bi, &s, place(&A1, tag, n, 0));
    poison_table(&t, 3);
    keep = t;
    EXPECT(resv_add(NULL, 0, 0x1000, "x") == RESV_BAD_RANGE, "resv_add on a null table");
    EXPECT(resv_add((struct resv_table *)((uint8_t *)&t + 1), 0, 0x1000, "x") == RESV_BAD_RANGE, "resv_add on a misaligned table");
    EXPECT(resv_overlaps(NULL, 0, 0x1000) == 0 && resv_overlaps((const struct resv_table *)((uint8_t *)&t + 1), 0, 0x1000) == 0, "resv_overlaps on bad tables");
    resv_init(NULL);
    resv_init((struct resv_table *)((uint8_t *)&t + 1));
    EXPECT(!memcmp(&t, &keep, sizeof t), "a refused table call wrote");
    const char *r = boot_reserve(NULL, 0x200000, 0x2A0000, &t);
    EXPECT(r && !strcmp(r, "no memory map"), "boot_reserve on a null boot_info");
    r = boot_reserve((const struct boot_info *)((uint8_t *)&bi + 1), 0x200000, 0x2A0000, &t);
    EXPECT(r && !strcmp(r, "no memory map"), "boot_reserve on a misaligned boot_info");
    r = boot_reserve(&bi, 0x200000, 0x2A0000, NULL);
    EXPECT(r && !strcmp(r, "reservation table full"), "boot_reserve on a null table");
    r = boot_reserve(&bi, 0x200000, 0x2A0000, (struct resv_table *)((uint8_t *)&t + 1));
    EXPECT(r && !strcmp(r, "reservation table full") && !memcmp(&t, &keep, sizeof t), "boot_reserve on a misaligned table");
    /* a table lying on top of the boot_info or on the memory-map tag is refused before anything is written */
    {
        struct boot_info *b2 = (struct boot_info *)big;
        *b2 = bi;
        r = boot_reserve(b2, 0x200000, 0x2A0000, (struct resv_table *)(big + 8));
        EXPECT(r && !strcmp(r, "reservation table full") && !memcmp(b2, &bi, sizeof bi), "a table on the boot_info was not refused");
        memcpy(big + 4096, tag, n);
        bi.mmap = (const struct mb2_tag_mmap *)(big + 4096);
        r = boot_reserve(&bi, 0x200000, 0x2A0000, (struct resv_table *)(big + 4096 + 8));
        EXPECT(r && !strcmp(r, "reservation table full") && !memcmp(big + 4096, tag, n), "a table on the memory-map tag was not refused");
        bi.mmap = (const struct mb2_tag_mmap *)place(&A1, tag, n, 0);
    }
    /* a tag shorter than the entries it claims: refused with the existing message instead of reading past it */
    bi.mmap_entries = s.n + 50;
    poison_table(&t, 0);
    keep = t;
    r = boot_reserve(&bi, 0x200000, 0x2A0000, &t);
    EXPECT(r && !strcmp(r, "no memory map") && !memcmp(&t, &keep, sizeof t), "a tag too short for its entries was not refused");
    bi.mmap_entries = s.n;
    EXPECT(boot_mmap_entry(NULL, 0) == NULL && boot_mmap_entry((const struct boot_info *)((uint8_t *)&bi + 1), 0) == NULL, "boot_mmap_entry on bad boot_info");
    bi.mmap = NULL;
    EXPECT(boot_mmap_entry(&bi, 0) == NULL, "boot_mmap_entry without a map");
    /* the EFI entry points */
    memset(&rep, 0x5A, sizeof rep);
    EXPECT(efi_memory_exclude(NULL, &t, &rep) == -1 && rep.status == -1, "efi_memory_exclude on a null boot_info");
    EXPECT(efi_memory_exclude(&bi, NULL, &rep) == -1 && rep.status == -1, "efi_memory_exclude on a null table");
    EXPECT(efi_memory_exclude(&bi, &t, NULL) == -2, "efi_memory_exclude without a report");
    EXPECT(efi_memory_firmware_frame(NULL, &t, 0x100000) == 0 && efi_memory_firmware_frame(&bi, NULL, 0x100000) == 0, "firmware_frame on null");
    EXPECT(efi_memory_firmware_frame((const struct boot_info *)((uint8_t *)&bi + 1), &t, 0x100000) == 0, "firmware_frame on a misaligned boot_info");
    /* a count above the capacity (a caller bug; the C would read past the table) is clamped, not followed */
    poison_table(&t, RESV_MAX);
    t.count = 200;
    EXPECT(resv_add(&t, 0x1000, 0x2000, "x") == RESV_FULL, "resv_add on an over-full count");
    EXPECT(resv_overlaps(&t, 0x100000000ull, 0x100001000ull) == 1 && resv_overlaps(&t, 0, 0x1000) == 0, "resv_overlaps on an over-full count");
    EXPECT(efi_memory_firmware_frame(&bi, &t, 0x100000000ull) == 0, "firmware_frame on an over-full count");
    /* a good call still works afterwards */
    bi.mmap = (const struct mb2_tag_mmap *)place(&A1, tag, n, 0);
    poison_table(&t, 0);
    r = boot_reserve(&bi, 0x200000, 0x2A0000, &t);
    EXPECT(!r && t.count == 3 && !strcmp(t.r[0].what, "low 1 MiB") && !strcmp(t.r[1].what, "kernel image") && !strcmp(t.r[2].what, "multiboot2 info"),
           "a valid call failed after the hardening checks");
}

int main(void)
{
    A1 = make_arena();
    A2 = make_arena();
    hardening();
    unsigned long start = cases;
    sweep_a();
    sweep_b();
    sweep_pairs();
    sweep_modules();
    sweep_prefill();
    sweep_entries();
    sweep_null();
    unsigned long deterministic = cases - start;
    unsigned unseen = 0;
    for (unsigned i = 0; i < NOUT; i++)
        if (!hist[i]) {
            fprintf(stderr, "outcome never produced in the deterministic phase: %s\n", i ? MSG[i - 1] : "success");
            unseen++;
        }
    if (unseen)
        return 3;
    random_phase(100000, 300000, 50000);
    unsigned long reserve_cases = cases;
    table_ops();
    strerror_all();
    efi_sweep();
    efi_random(20000);
    if (!hist_efi[0] || !hist_efi[1] || !hist_efi[2]) {
        fprintf(stderr, "EFI glue outcomes not all produced: applied=%lu not-efi=%lu refused=%lu\n", hist_efi[0], hist_efi[1], hist_efi[2]);
        return 3;
    }
    unsigned ncodes = 0;
    for (unsigned i = 1; i < 14; i++)
        ncodes += efi_errors[i] != 0;
    /* "reservation refused" (13) needs the table to refuse a range the capacity check let through, which the window-bounded ranges
     * never do; the sink's failure sense is covered by a negative control instead (scripts/test-boot-resv.py). */
    if (efi_errors[12] == 0) {
        fprintf(stderr, "EFI capacity refusals not produced\n");
        return 3;
    }
    printf("B1 boot reservation differential: %lu boot_reserve inputs PASS (%lu deterministic over %u shapes, %lu random), %lu table operations, "
           "%lu EFI glue inputs; result, whole table (count, ranges, reasons, untouched slots) and boot_mmap_entry agree, guard pages never touched\n",
           reserve_cases, deterministic, NSHAPES, reserve_cases - deterministic, table_cases, cases - reserve_cases);
    printf("boot_reserve outcomes (all inputs): success=%lu", hist[0]);
    for (unsigned i = 0; i < NMSG; i++)
        printf(" e%u=%lu", i + 1, hist[i + 1]);
    printf("\nboot_reserve outcomes (random phase): success=%lu", hist_random[0]);
    for (unsigned i = 0; i < NMSG; i++)
        printf(" e%u=%lu", i + 1, hist_random[i + 1]);
    printf("\nefi_memory_exclude: applied=%lu not-efi=%lu refused=%lu other=%lu; refusal codes seen:", hist_efi[0], hist_efi[1], hist_efi[2], hist_efi[3]);
    for (unsigned i = 1; i < 14; i++)
        printf(" %u=%lu", i, efi_errors[i]);
    printf(" (%u of 13)\n", ncodes);
    return 0;
}
