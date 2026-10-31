/* A2: differential of the Rust mb2_parse against the frozen C reference (tests/host/reference/bootinfo_ref.c, a verbatim copy of
 * the accepted arch/x86_64/bootinfo.c, built here under the names *_reference). Both parse exactly the same bytes and must
 * agree on the returned error string and on every DEFINED field of struct boot_info, after a success and after a failure
 * (the structure is left partly filled on an error, and that partial state is compared too). Structure padding is not
 * compared: it is not part of the contract (both zero it today; the Rust side is asserted to).
 *
 * Inputs: deterministic sweeps over structurally valid bases (every aligned 32-bit word of every base set to boundary
 * values, every tag-header pair, every byte, every total_size, every physical address near the limits, memory-map and
 * module field products, string termination, tag and module counts) and a bounded, seeded random phase that stops on a
 * coverage rule (no new outcome signature for a plateau of inputs), not on an arbitrary count.
 *
 * Memory safety: each block is copied into a mapping whose neighbours are PROT_NONE guard pages, once flush with the rear
 * guard and once with the front guard, so a read outside [info, info + total_size) by either implementation faults and fails
 * the test (ASan covers the C reference only, not the Rust machine code). A call whose physical address the contract rejects
 * before any read gets a pointer into a guard page; when total_size is invalid only the 8-byte header is mapped.
 * Hardening outside the C contract (null or misaligned structure, null info, overlap) is checked for the Rust function only
 * and is never run through the C reference, whose behavior there is undefined.
 *
 * usage: mb2-differential     exit 0: no mismatch and the coverage rules held. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "bootinfo.h"
#include "resv.h"

const char *mb2_parse_reference(const void *, uint64_t, struct boot_info *);

/* The reference object also carries boot_reserve, which is never called here; it needs resv_add to link. */
int resv_add(struct resv_table *t, uint64_t start, uint64_t end, const char *what)
{
    (void)t, (void)start, (void)end, (void)what;
    return RESV_OK;
}

__attribute__((noreturn)) void panic(const char *message)
{
    fprintf(stderr, "unexpected Rust panic: %s\n", message);
    exit(2);
}

/* The layout the Rust glue pins, checked against this C compiler. */
_Static_assert(sizeof(struct boot_module) == 24, "boot_module");
_Static_assert(sizeof(struct boot_info) == 1624 && _Alignof(struct boot_info) == 8, "boot_info size");
_Static_assert(offsetof(struct boot_info, info_start) == 0 && offsetof(struct boot_info, info_end) == 8, "info");
_Static_assert(offsetof(struct boot_info, loader) == 16 && offsetof(struct boot_info, cmdline) == 24, "strings");
_Static_assert(offsetof(struct boot_info, mmap) == 32 && offsetof(struct boot_info, mmap_entries) == 40, "mmap");
_Static_assert(offsetof(struct boot_info, modules) == 48 && offsetof(struct boot_info, module_count) == 1584, "modules");
_Static_assert(offsetof(struct boot_info, fb) == 1592 && offsetof(struct boot_info, fb_tags) == 1600, "fb");
_Static_assert(offsetof(struct boot_info, efi) == 1604 && offsetof(struct boot_info, efi_mmap) == 1608, "efi");
_Static_assert(offsetof(struct boot_info, efi_mmap_tags) == 1616 && offsetof(struct boot_info, efi_bs_active) == 1620, "bs");

#define PAGE 4096u
#define DATA (MB2_INFO_MAX + PAGE) /* mapped block area; a guard page sits on each side of it */
#define LIMIT 0x40000000ull
static uint8_t *front_guard, *data, *rear_guard;
static unsigned long cases;
static int in_random;

static const char *ERRS[] = {
    "info block address invalid", "info block size invalid", "tag size invalid", "end tag size invalid", "no memory map",
    "duplicate command line tag", "command line not terminated", "module tag too small",
    "module command line not terminated", "module range empty or reversed", "module extends above 1 GiB", "too many modules",
    "duplicate memory map tag", "memory map tag invalid", "memory map version unsupported", "memory map empty",
    "memory map has a partial entry", "no end tag",
};
#define NERR (sizeof ERRS / sizeof ERRS[0])
static unsigned long hist[NERR + 1], hist_random[NERR + 1]; /* index 0: success; i + 1: ERRS[i] */

static int outcome(const char *s)
{
    if (!s)
        return 0;
    for (unsigned i = 0; i < NERR; i++)
        if (!strcmp(s, ERRS[i]))
            return (int)i + 1;
    return -1;
}

/* Fields only: struct padding ([44, 48) and [1588, 1592)) is unspecified and never compared. */
static int same(const struct boot_info *a, const struct boot_info *b)
{
    return !memcmp(a, b, 44) && !memcmp((const uint8_t *)a + 48, (const uint8_t *)b + 48, 1588 - 48) &&
           !memcmp((const uint8_t *)a + 1592, (const uint8_t *)b + 1592, sizeof *a - 1592);
}

static int padding_zero(const struct boot_info *a)
{
    const uint8_t *p = (const uint8_t *)a;
    for (int i = 44; i < 48; i++)
        if (p[i])
            return 0;
    for (int i = 1588; i < 1592; i++)
        if (p[i])
            return 0;
    return 1;
}

__attribute__((noreturn)) static void fail(const char *what, const uint8_t *bytes, uint32_t n, uint64_t phys, const char *rust, const char *ref)
{
    fprintf(stderr, "MISMATCH (%s): phys %#llx len %u Rust=%s C=%s\n", what, (unsigned long long)phys, n, rust ? rust : "(success)",
            ref ? ref : "(success)");
    char name[64];
    snprintf(name, sizeof name, "mb2-mismatch-%lu.bin", cases);
    FILE *f = fopen(name, "wb");
    if (f) {
        fwrite(bytes, 1, n, f);
        fclose(f);
        fprintf(stderr, "input kept in %s\n", name);
    }
    exit(1);
}

static void note_signature(const struct boot_info *bi, int oc);

/* The 32-bit LE word at the start of the image: its total_size claim. */
static uint32_t claim(const uint8_t *img, uint32_t len)
{
    uint32_t t = 0;
    memcpy(&t, img, len < 4 ? len : 4);
    return t;
}

/* One input, both implementations, both guard placements. `len` is how many bytes of `img` exist; the mapped block is
 * exactly total_size bytes when the contract lets the parser read them (zero padded if `img` is shorter), the 8-byte header
 * when total_size is invalid, and a guard page when the physical address is rejected before any read. */
static void run(const uint8_t *img, uint32_t len, uint64_t phys)
{
    static struct boot_info a, b;
    uint32_t total = claim(img, len);
    int addr_ok = phys != 0 && phys % 8 == 0 && phys <= LIMIT - 8;
    int size_ok = addr_ok && total >= 16 && total <= MB2_INFO_MAX && phys + total <= LIMIT;
    uint32_t mapped = size_ok ? total : 8;

    cases++;
    for (int place = 0; place < 2; place++) {
        uint8_t *p;
        if (!addr_ok) {
            p = place ? front_guard : rear_guard;
        } else {
            p = place == 0 ? data + DATA - mapped : data; /* flush with the rear guard, then with the front guard */
            memset(p, 0, mapped);
            memcpy(p, img, len < mapped ? len : mapped);
        }
        memset(&a, 0xA5, sizeof a);
        memset(&b, 0xA5, sizeof b);
        const char *x = mb2_parse(p, phys, &a);
        const char *y = mb2_parse_reference(p, phys, &b);
        int ox = outcome(x), oy = outcome(y);
        if (ox < 0 || oy < 0 || ox != oy)
            fail("result", img, len, phys, x, y);
        if (!same(&a, &b))
            fail("boot_info fields", img, len, phys, x, y);
        if (!padding_zero(&a))
            fail("Rust padding not zero", img, len, phys, x, y);
        if (place == 0) {
            hist[ox]++;
            if (in_random)
                hist_random[ox]++;
            note_signature(&a, ox);
        }
    }
}

/* ---- block builder ------------------------------------------------------------------------------------------------ */

#define AUTO 0xfffffffeu
static uint8_t blk[MB2_INFO_MAX + 64];
static uint32_t boff;

static void b_begin(void)
{
    memset(blk, 0, sizeof blk);
    boff = 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void b_tag(uint32_t type, uint32_t size_field, const void *payload, uint32_t n)
{
    put32(blk + boff, type);
    put32(blk + boff + 4, size_field == AUTO ? 8 + n : size_field);
    if (payload && n)
        memcpy(blk + boff + 8, payload, n);
    boff = (boff + 8 + n + 7) & ~7u;
}

static uint32_t b_end(void)
{
    b_tag(0, AUTO, NULL, 0);
    put32(blk, boff);
    return boff;
}

static void b_str(uint32_t type, const char *s)
{
    b_tag(type, AUTO, s, (uint32_t)strlen(s) + 1);
}

static void b_module(uint32_t start, uint32_t end, const char *s)
{
    uint8_t p[8 + 64];
    put32(p, start);
    put32(p + 4, end);
    size_t n = strlen(s) + 1;
    memcpy(p + 8, s, n);
    b_tag(3, AUTO, p, (uint32_t)(8 + n));
}

static void b_mmap(uint32_t entry_size, uint32_t version, unsigned entries)
{
    static uint8_t p[8 + 32 * 8];
    memset(p, 0, sizeof p);
    put32(p, entry_size);
    put32(p + 4, version);
    for (unsigned i = 0; i < entries; i++) {
        uint64_t base = 0x100000ull * i, len = 0x80000, type = 1;
        memcpy(p + 8 + (size_t)i * entry_size, &base, 8);
        memcpy(p + 16 + (size_t)i * entry_size, &len, 8);
        memcpy(p + 24 + (size_t)i * entry_size, &type, 4);
    }
    b_tag(6, AUTO, p, 8 + entry_size * entries);
}

static void b_fb(void)
{
    uint8_t p[16 + 8 + 8 + 8 + 6] = {0};
    p[0] = 0x00, p[1] = 0x00, p[2] = 0x80, p[3] = 0xfd; /* addr */
    put32(p + 8, 4096);
    put32(p + 12, 1024);
    put32(p + 16, 768);
    p[20] = 32, p[21] = 1;
    p[24] = 16, p[25] = 8, p[26] = 8, p[27] = 8, p[28] = 0, p[29] = 8;
    b_tag(8, AUTO, p, 30);
}

struct base {
    uint8_t bytes[MB2_INFO_MAX + 64];
    uint32_t total;
};
#define MAXB 12
static struct base bases[MAXB];
static unsigned nbases;

static void keep(void)
{
    uint32_t t = b_end();
    memcpy(bases[nbases].bytes, blk, t);
    bases[nbases].total = t;
    nbases++;
}

static void make_bases(void)
{
    uint8_t efi_pl[16] = {0}, efimap[16 + 40] = {0};
    put32(efimap, 40), put32(efimap + 4, 1);
    b_begin(); /* 0: minimal */
    b_mmap(24, 0, 2);
    keep();
    b_begin(); /* 1: everything */
    b_str(1, "vos.console=1 vos.input_diag=1");
    b_str(2, "GRUB 2.12");
    b_module(0x200000, 0x210000, "hello");
    b_module(0x300000, 0x301000, "app two");
    b_fb();
    b_tag(12, AUTO, efi_pl, 8);
    b_tag(17, AUTO, efimap, sizeof efimap);
    b_tag(18, AUTO, NULL, 0);
    b_mmap(24, 0, 5);
    keep();
    b_begin(); /* 2: entry size 32 */
    b_str(1, "x");
    b_mmap(32, 0, 3);
    keep();
    b_begin(); /* 3: 64 modules (the limit) */
    for (unsigned i = 0; i < 64; i++)
        b_module(0x200000 + i * 0x2000, 0x201000 + i * 0x2000, "m");
    b_mmap(24, 0, 2);
    keep();
    b_begin(); /* 4: map first, then modules and strings */
    b_mmap(24, 0, 4);
    b_module(0x400000, 0x401000, "late");
    b_str(2, "loader");
    b_str(1, "cmd");
    keep();
    b_begin(); /* 5: unknown tags of odd sizes */
    b_tag(14, 13, "abcde", 5);
    b_tag(0x1234, AUTO, "0123456789abcdef", 16);
    b_tag(21, AUTO, NULL, 3);
    b_mmap(24, 0, 1);
    keep();
    b_begin(); /* 6: repeated framebuffer and EFI tags, 32-bit system table */
    b_fb();
    b_fb();
    b_tag(11, AUTO, efi_pl, 4);
    b_tag(17, AUTO, efimap, sizeof efimap);
    b_tag(17, AUTO, efimap, sizeof efimap);
    b_mmap(24, 0, 2);
    keep();
    b_begin(); /* 7: a block of the maximum size */
    b_tag(14, AUTO, NULL, MB2_INFO_MAX - 8 - 8 - (8 + 16 + 24) - 8 - 8 /* unknown + map + end */);
    b_mmap(24, 0, 1);
    keep();
}

/* ---- boundary values ---------------------------------------------------------------------------------------------- */

static const uint32_t V32[] = {0, 1, 2, 3, 4, 7, 8, 9, 12, 15, 16, 17, 23, 24, 25, 31, 32, 33, 40, 48, 0x7f, 0x80, 0xff, 0x100,
                               0x1000, 0xfff8, 0xffff, 0x10000, 0x10001, 0x10008, 0x3ffffffe, 0x3fffffff, 0x40000000, 0x40000001,
                               0x7fffffff, 0x80000000, 0xfffffff8, 0xfffffffe, 0xffffffff};
#define NV32 (sizeof V32 / sizeof V32[0])
static const uint64_t PH[] = {0, 1, 4, 8, 12, 16, 0x1000, 0x68000, 0x100000, 0x200000, 0x3fff0000ull, 0x3fffffe8ull, 0x3ffffff0ull,
                              0x3ffffff8ull, 0x40000000ull, 0x40000008ull, 0x7ffffff8ull, 0xfffffffffffffff8ull,
                              0xffffffffffffffffull, 0x8000000000000000ull};
#define NPH (sizeof PH / sizeof PH[0])

static uint8_t work[MB2_INFO_MAX + 64];
static const uint64_t PHYS_OK = 0x200000;

static void sweep_total(const struct base *b)
{
    for (unsigned i = 0; i < NV32; i++) {
        memcpy(work, b->bytes, b->total);
        put32(work, V32[i]);
        run(work, b->total, PHYS_OK);
    }
    for (uint32_t t = 0; t <= b->total + 16 && t <= MB2_INFO_MAX + 2; t++) { /* every truncation and overshoot */
        memcpy(work, b->bytes, b->total);
        put32(work, t);
        run(work, b->total, PHYS_OK);
    }
}

static void sweep_phys(const struct base *b)
{
    for (unsigned i = 0; i < NPH; i++)
        run(b->bytes, b->total, PH[i]);
    for (int d = -9; d <= 9; d++) /* the block ends at, just below and just above the 1 GiB limit */
        run(b->bytes, b->total, (LIMIT - b->total + d) & ~7ull), run(b->bytes, b->total, LIMIT - b->total + d);
}

static void sweep_words(const struct base *b)
{
    for (uint32_t pos = 0; pos + 4 <= b->total; pos += 4)
        for (unsigned i = 0; i < NV32; i++) {
            memcpy(work, b->bytes, b->total);
            put32(work + pos, V32[i]);
            run(work, b->total, PHYS_OK);
        }
}

static void sweep_bytes(const struct base *b)
{
    static const uint8_t BV[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
    for (uint32_t pos = 0; pos < b->total; pos++)
        for (unsigned i = 0; i < sizeof BV; i++) {
            memcpy(work, b->bytes, b->total);
            work[pos] = BV[i];
            run(work, b->total, PHYS_OK);
        }
}

/* Pairs of tag-header words (type and size of every tag, plus total_size): the rules that interact across tags. */
static void sweep_pairs(const struct base *b)
{
    static const uint32_t PV[] = {0, 1, 7, 8, 15, 16, 17, 24, 0x10000, 0xffffffff};
    uint32_t pos[256], n = 0;
    pos[n++] = 0;
    for (uint32_t off = 8; off + 8 <= b->total && n + 2 <= 256;) {
        uint32_t size = get32(b->bytes + off + 4);
        pos[n++] = off, pos[n++] = off + 4;
        if (size < 8 || size > b->total - off)
            break;
        off += (size + 7) & ~7u;
    }
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++)
            for (unsigned x = 0; x < sizeof PV / 4; x++)
                for (unsigned y = 0; y < sizeof PV / 4; y++) {
                    memcpy(work, b->bytes, b->total);
                    put32(work + pos[i], PV[x]);
                    put32(work + pos[j], PV[y]);
                    run(work, b->total, PHYS_OK);
                }
}

static void built(void)
{
    uint32_t t = b_end();
    run(blk, t, PHYS_OK);
}

static void structural(void)
{
    static const uint32_t ES[] = {0, 1, 8, 16, 23, 24, 25, 32, 40, 48, 0xffffffff};
    uint8_t pl[8 + 256] = {0};
    /* memory-map tag: every entry size, version and payload length, with and without the closing tag */
    for (unsigned e = 0; e < sizeof ES / 4; e++)
        for (uint32_t v = 0; v < 3; v++)
            for (uint32_t len = 0; len <= 130; len++) {
                b_begin();
                put32(pl, ES[e]);
                put32(pl + 4, v == 2 ? 0xffffffff : v);
                b_tag(6, AUTO, pl, 8 + len);
                built();
            }
    /* duplicates and ordering of every tag type the parser counts */
    for (unsigned t = 0; t < 22; t++) {
        for (unsigned k = 0; k <= 3; k++) {
            b_begin();
            for (unsigned i = 0; i < k; i++)
                b_tag(t, AUTO, "0123456789abcdef\0\0\0\0\0\0\0\0", t == 3 ? 24 : t == 17 ? 24 : 16);
            b_mmap(24, 0, 2);
            built();
            b_begin();
            b_mmap(24, 0, 2);
            for (unsigned i = 0; i < k; i++)
                b_tag(t, AUTO, "0123456789abcdef\0\0\0\0\0\0\0\0", t == 3 ? 24 : 16);
            built();
        }
    }
    b_begin(); /* two memory maps */
    b_mmap(24, 0, 2);
    b_mmap(24, 0, 2);
    built();
    b_begin(); /* end tag before the map; tags after the end tag; no end tag at all */
    b_tag(0, AUTO, NULL, 0);
    b_mmap(24, 0, 2);
    put32(blk, boff);
    run(blk, boff, PHYS_OK);
    b_begin();
    b_mmap(24, 0, 2);
    b_tag(0, AUTO, NULL, 0);
    b_tag(1, 0xffffffff, NULL, 0);
    put32(blk, boff);
    run(blk, boff, PHYS_OK);
    b_begin();
    b_mmap(24, 0, 2);
    put32(blk, boff);
    run(blk, boff, PHYS_OK);
    for (uint32_t s = 0; s <= 24; s++) { /* end tag sizes */
        b_begin();
        b_mmap(24, 0, 2);
        b_tag(0, s, NULL, 16);
        put32(blk, boff);
        run(blk, boff, PHYS_OK);
    }
    /* modules: count, range and string variants */
    for (unsigned k = 0; k <= 66; k++) {
        b_begin();
        for (unsigned i = 0; i < k; i++)
            b_module(0x200000 + i * 0x2000, 0x201000 + i * 0x2000, "m");
        b_mmap(24, 0, 2);
        built();
    }
    static const uint32_t W[] = {0, 1, 2, 0xfff, 0x1000, 0x3ffffffe, 0x3fffffff, 0x40000000, 0x40000001, 0x7fffffff, 0x80000000, 0xffffffff};
    for (unsigned s = 0; s < sizeof W / 4; s++)
        for (unsigned e = 0; e < sizeof W / 4; e++) {
            b_begin();
            b_module(W[s], W[e], "m");
            b_mmap(24, 0, 2);
            built();
        }
    /* string tags of every length and either termination */
    for (uint32_t len = 0; len <= 40; len++)
        for (unsigned last = 0; last < 2; last++)
            for (unsigned t = 1; t <= 3; t++) {
                uint8_t s[8 + 48];
                memset(s, 'a', sizeof s);
                put32(s, 0x200000), put32(s + 4, 0x201000);
                uint32_t head = t == 3 ? 8 : 0;
                if (len)
                    s[head + len - 1] = last ? 0 : 'z';
                b_begin();
                b_tag(t, AUTO, s, head + len);
                b_mmap(24, 0, 2);
                built();
            }
    /* odd tag sizes and the alignment of the next tag */
    for (uint32_t n = 0; n <= 24; n++) {
        b_begin();
        b_tag(14, AUTO, "ABCDEFGHIJKLMNOPQRSTUVWX", n);
        b_mmap(24, 0, 2);
        built();
    }
}

/* ---- seeded random phase ------------------------------------------------------------------------------------------ */

static uint64_t rng = 0x564f53324d423231ull;
static uint64_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

#define SIGS 65536
/* The plateau rule watches the STRUCTURAL class of a result: (outcome, module bucket, framebuffer tags, EFI map tags, EFI
 * system table, boot-services flag). The presence flags of the command line, loader name and memory map are independent of
 * each other and are swept one by one by the deterministic phase, so they are counted separately and only reported. */
static uint32_t class_seen[SIGS], full_seen[SIGS];
static unsigned long classes, fulls, last_new;

static int insert(uint32_t *table, uint32_t sig)
{
    uint32_t h = (sig * 2654435761u) & (SIGS - 1);
    while (table[h] && table[h] != sig + 1)
        h = (h + 1) & (SIGS - 1);
    if (table[h])
        return 0;
    table[h] = sig + 1;
    return 1;
}

static void note_signature(const struct boot_info *bi, int oc)
{
    uint32_t mb = bi->module_count == 0 ? 0 : bi->module_count == 1 ? 1 : bi->module_count < 64 ? 2 : 3;
    uint32_t fb = bi->fb_tags > 2 ? 2 : bi->fb_tags, em = bi->efi_mmap_tags > 2 ? 2 : bi->efi_mmap_tags;
    uint32_t cls = (uint32_t)oc | mb << 5 | fb << 7 | em << 9 | (uint32_t)(bi->efi != 0) << 11 | (uint32_t)(bi->efi_bs_active != 0) << 12;
    uint32_t full = cls | (uint32_t)(bi->cmdline != NULL) << 13 | (uint32_t)(bi->loader != NULL) << 14 | (uint32_t)(bi->mmap != NULL) << 15;
    if (insert(class_seen, cls)) {
        classes++;
        last_new = cases;
    }
    if (insert(full_seen, full))
        fulls++;
}

static void random_phase(unsigned long minimum, unsigned long cap, unsigned long plateau)
{
    unsigned long done = 0;
    in_random = 1;
    for (;;) {
        const struct base *b = &bases[next() % nbases];
        uint32_t total = b->total, edits = 1 + next() % 4;
        memcpy(work, b->bytes, total);
        for (uint32_t e = 0; e < edits; e++) {
            switch (next() % 4) {
            case 0:
                put32(work + (next() % (total / 4)) * 4, V32[next() % NV32]);
                break;
            case 1:
                put32(work + (next() % (total / 4)) * 4, (uint32_t)next());
                break;
            case 2:
                work[next() % total] ^= (uint8_t)(1u << (next() % 8));
                break;
            default:
                work[next() % total] = (uint8_t)next();
                break;
            }
        }
        if (next() % 8 == 0) {
            uint32_t t = V32[next() % NV32] % (MB2_INFO_MAX + 2);
            put32(work, t);
        }
        run(work, total, next() % 16 == 0 ? PH[next() % NPH] : PHYS_OK);
        done++;
        if ((done >= minimum && cases - last_new >= plateau) || done >= cap)
            break;
    }
    in_random = 0;
    printf("random phase: %lu inputs; %lu structural classes (outcome x module bucket x framebuffer x EFI flags), last new at input %lu; "
           "%lu classes including the string/map presence flags\n",
           done, classes, last_new, fulls);
}

/* ---- hardening outside the C contract: the Rust function only ----------------------------------------------------- */

static void hardening(void)
{
    static struct boot_info bi __attribute__((aligned(8)));
    const char *bad = "info block address invalid";
    b_begin();
    b_mmap(24, 0, 2);
    uint32_t t = b_end();
    uint8_t *p = data;
    memcpy(p, blk, t);
    memset(&bi, 0xA5, sizeof bi);
    if (!mb2_parse(p, PHYS_OK, NULL))
        exit(1); /* a null structure is refused */
    const char *r = mb2_parse(p, PHYS_OK, (struct boot_info *)((uint8_t *)&bi + 1));
    if (!r || strcmp(r, bad) || ((uint8_t *)&bi)[0] != 0xA5)
        fail("misaligned structure not refused without writing", blk, t, PHYS_OK, r, bad);
    r = mb2_parse(NULL, PHYS_OK, &bi);
    if (!r || strcmp(r, bad) || bi.info_start != 0)
        fail("null info not refused", blk, t, PHYS_OK, r, bad);
    /* a valid block that lies inside the structure is refused without writing (the C would zero it first) */
    memcpy(&bi, blk, t);
    r = mb2_parse((const void *)&bi, PHYS_OK, &bi);
    if (!r || strcmp(r, bad) || memcmp(&bi, blk, t))
        fail("overlapping block not refused without writing", blk, t, PHYS_OK, r, bad);
    r = mb2_parse(data, PHYS_OK, &bi); /* and a good call still works afterwards */
    if (r)
        fail("a valid block was refused after the hardening checks", blk, t, PHYS_OK, r, NULL);
    /* an invalid physical address never reads the block (rear guard pointer, no mapping behind it) */
    r = mb2_parse(rear_guard, 0, &bi);
    if (!r || strcmp(r, bad))
        fail("zero physical address", blk, t, 0, r, bad);
}

int main(void)
{
    uint8_t *m = mmap(NULL, PAGE + DATA + PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m + PAGE, DATA, PROT_READ | PROT_WRITE)) {
        perror("mmap");
        return 2;
    }
    front_guard = m, data = m + PAGE, rear_guard = m + PAGE + DATA;
    make_bases();
    hardening();
    unsigned long phase_start = cases;
    for (unsigned i = 0; i < nbases; i++) {
        sweep_total(&bases[i]);
        sweep_phys(&bases[i]);
        sweep_words(&bases[i]);
        sweep_bytes(&bases[i]);
        if (i != 3 && i != 7)
            sweep_pairs(&bases[i]);
    }
    structural();
    unsigned long deterministic = cases - phase_start;
    random_phase(100000, 400000, 50000);
    unsigned long unseen = 0;
    for (unsigned i = 0; i <= NERR; i++)
        if (!hist[i]) {
            fprintf(stderr, "outcome never produced: %s\n", i ? ERRS[i - 1] : "success");
            unseen++;
        }
    if (unseen)
        return 3;
    printf("A2 mb2_parse differential: %lu inputs PASS (%lu deterministic over %u bases, %lu random); result, error text and every "
           "defined boot_info field agree, Rust padding zero, guard pages never touched\n",
           cases, deterministic, nbases, cases - deterministic);
    printf("outcomes (all inputs): 0=%lu", hist[0]);
    for (unsigned i = 1; i <= NERR; i++)
        printf(" %u=%lu", i, hist[i]);
    printf("\noutcomes (random phase): 0=%lu", hist_random[0]);
    for (unsigned i = 1; i <= NERR; i++)
        printf(" %u=%lu", i, hist_random[i]);
    printf("\n");
    return 0;
}
