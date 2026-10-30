/* A1: differential of the Rust elf_validate/elf_strerror against the frozen C reference (arch/x86_64/elf.c, byte-identical,
 * built here under the names *_reference). Both are called on exactly the same bytes; the result code, every field of the
 * image (never the structure padding) and the message text must agree on every input, and a failure must leave the image
 * all zero. Inputs: deterministic sweeps (every header, program-header and note field at boundary values, field pairs that
 * interact in one rule, every truncation, structurally built images over program-header sequences and note variants), then a
 * bounded, seeded random phase that stops on a coverage rule, not on an arbitrary count.
 *
 * Memory safety: each image is copied into a mapping whose neighbours are PROT_NONE guard pages, once flush with the front
 * guard and once with the rear guard, so a read outside [img, img + size) by either implementation faults and fails the test
 * (ASan covers the C reference only, not the Rust machine code). Calls whose contract forbids reading (size < 64 or
 * > 1 MiB) get a pointer into a guard page. Hardening outside the C contract (null/misaligned out, null img, overlap) is
 * checked for the Rust function only.
 *
 * usage: elf-differential VALID_BASE.elf...      exit 0: no mismatch and the coverage rules held. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "elf.h"

int elf_validate_reference(const uint8_t *, uint64_t, struct elf_image *);
const char *elf_strerror_reference(int);

__attribute__((noreturn)) void panic(const char *message)
{
    fprintf(stderr, "unexpected Rust panic: %s\n", message);
    exit(2);
}

#define PAGE 4096u
#define DATA (ELF_MAX_FILE + PAGE) /* mapped image area; a guard page sits on each side of it */
static uint8_t *front_guard, *data, *rear_guard;
static unsigned long cases;
static unsigned long hist[ELF_NERRORS];
static unsigned long hist_random[ELF_NERRORS];
static unsigned long phase_hist[ELF_NERRORS];
static int in_random;

static int equal(const struct elf_image *a, const struct elf_image *b)
{
    if (a->entry != b->entry || a->pages != b->pages || a->abi != b->abi || a->nload != b->nload)
        return 0;
    for (unsigned i = 0; i < ELF_MAX_LOADS; i++) {
        const struct elf_segment *x = &a->load[i], *y = &b->load[i];
        if (x->vaddr != y->vaddr || x->memsz != y->memsz || x->offset != y->offset || x->filesz != y->filesz ||
            x->flags != y->flags)
            return 0;
    }
    return 1; /* fields, never unspecified padding */
}

static int zero_image(const struct elf_image *a)
{
    struct elf_image z;
    memset(&z, 0, sizeof z);
    return equal(a, &z);
}

__attribute__((noreturn)) static void fail(const char *what, const uint8_t *bytes, uint64_t n, int rust, int ref)
{
    fprintf(stderr, "MISMATCH (%s): size %llu Rust=%d (%s) C=%d (%s)\n", what, (unsigned long long)n, rust,
            elf_strerror(rust), ref, elf_strerror_reference(ref));
    char name[64];
    snprintf(name, sizeof name, "elf-mismatch-%lu.bin", cases);
    FILE *f = fopen(name, "wb");
    if (f) {
        fwrite(bytes, 1, n >= 64 && n <= ELF_MAX_FILE ? n : 0, f); /* only a size at which the input was really read */
        fclose(f);
        fprintf(stderr, "input kept in %s\n", name);
    }
    exit(1);
}

static void note_signature(int code, const struct elf_image *im);

/* One input, both implementations, both guard placements. A size at which the contract forbids reading (below 64 bytes or
 * above 1 MiB) gets a pointer into a guard page instead: any read of it faults. */
static void run(const uint8_t *bytes, uint64_t n)
{
    cases++;
    int readable = n >= 64 && n <= ELF_MAX_FILE;
    for (int place = 0; place < 2; place++) {
        uint8_t *p;
        if (!readable) {
            p = place ? front_guard : rear_guard;
        } else {
            p = place == 0 ? data + DATA - n : data; /* flush with the rear guard, then with the front guard */
            memcpy(p, bytes, (size_t)n);
        }
        struct elf_image a, b;
        memset(&a, 0xA5, sizeof a);
        memset(&b, 0xA5, sizeof b);
        int x = elf_validate(p, n, &a);
        int y = elf_validate_reference(p, n, &b);
        if (x != y)
            fail("result code", bytes, n, x, y);
        if (!equal(&a, &b))
            fail("image fields", bytes, n, x, y);
        if (x != ELF_OK && !zero_image(&a))
            fail("image not zero after a failure", bytes, n, x, y);
        if (x < 0 || x >= ELF_NERRORS)
            fail("code out of range", bytes, n, x, y);
        if (place == 0) {
            hist[x]++;
            note_signature(x, &a);
            phase_hist[x]++;
            if (in_random)
                hist_random[x]++;
        }
    }
}

/* ---- boundary values and field tables ---------------------------------------------------------------------------- */

static const uint64_t V[] = {
    0, 1, 2, 3, 4, 7, 8, 0x10, 0x38, 0x40, 0xff, 0x100, 0xfff, 0x1000, 0x1001, 0x2000, 0x200000, 0x3fffff, 0x400000,
    0x400001, 0x401000, 0x7fffffff, 0x3fffffff, 0x40000000, 0x40000001, 0xffffffffu, 0x100000000ull,
    (1ull << 40) - 1, 1ull << 40, (1ull << 40) + 1, 1ull << 41, 1ull << 62, 1ull << 63, ~0ull - 7, ~0ull - 1, ~0ull,
};
#define NV (sizeof V / sizeof V[0])

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    while (n--)
        v = v << 8 | p[n];
    return v;
}

static void put(uint8_t *p, uint64_t off, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; i++)
        p[off + i] = (uint8_t)(v >> (8 * i));
}

struct field {
    unsigned off, width;
};
static const struct field HDR[] = {
    {0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1}, {9, 1}, {10, 1}, {11, 1}, {12, 1}, {13, 1},
    {14, 1}, {15, 1}, {16, 2}, {18, 2}, {20, 4}, {24, 8}, {32, 8}, {40, 8}, {48, 4}, {52, 2}, {54, 2}, {56, 2}, {58, 2},
    {60, 2}, {62, 2},
};
static const struct field PH[] = {{0, 4}, {4, 4}, {8, 8}, {16, 8}, {24, 8}, {32, 8}, {40, 8}, {48, 8}};
static const struct field NOTE[] = {{0, 4}, {4, 4}, {8, 4}, {12, 1}, {13, 1}, {14, 1}, {15, 1}, {16, 4}, {20, 4}};

static uint8_t work[ELF_MAX_FILE + 64];

struct base {
    const char *name;
    uint8_t *bytes;
    size_t n;
    uint64_t phoff, phnum, note_off, note_size; /* note_size 0: none found */
};

static void locate(struct base *b)
{
    b->phoff = le(b->bytes + 32, 8);
    b->phnum = le(b->bytes + 56, 2);
    b->note_off = b->note_size = 0;
    for (uint64_t i = 0; i < b->phnum; i++) {
        uint64_t ph = b->phoff + i * 56;
        if (ph + 56 <= b->n && le(b->bytes + ph, 4) == 4) {
            b->note_off = le(b->bytes + ph + 8, 8);
            b->note_size = le(b->bytes + ph + 32, 8);
        }
    }
}

static void sweep_fields(const struct base *b)
{
    memcpy(work, b->bytes, b->n);
    for (unsigned f = 0; f < sizeof HDR / sizeof HDR[0]; f++) {
        for (unsigned v = 0; v < NV; v++) {
            put(work, HDR[f].off, HDR[f].width, V[v]);
            run(work, b->n);
            memcpy(work + HDR[f].off, b->bytes + HDR[f].off, HDR[f].width);
        }
    }
    for (uint64_t i = 0; i < b->phnum; i++) {
        uint64_t ph = b->phoff + i * 56;
        if (ph + 56 > b->n)
            break;
        for (unsigned f = 0; f < sizeof PH / sizeof PH[0]; f++)
            for (unsigned v = 0; v < NV; v++) {
                put(work, ph + PH[f].off, PH[f].width, V[v]);
                run(work, b->n);
                memcpy(work + ph + PH[f].off, b->bytes + ph + PH[f].off, PH[f].width);
            }
    }
    if (b->note_size >= 24 && b->note_off + 24 <= b->n)
        for (unsigned f = 0; f < sizeof NOTE / sizeof NOTE[0]; f++)
            for (unsigned v = 0; v < NV; v++) {
                put(work, b->note_off + NOTE[f].off, NOTE[f].width, V[v]);
                run(work, b->n);
                memcpy(work + b->note_off + NOTE[f].off, b->bytes + b->note_off + NOTE[f].off, NOTE[f].width);
            }
}

static void sweep_pairs(const struct base *b)
{
    static const unsigned pairs[][2] = {{8, 32}, {16, 40}, {32, 40}, {8, 16}, {0, 4}, {40, 48}, {16, 48}};
    memcpy(work, b->bytes, b->n);
    for (uint64_t i = 0; i < b->phnum; i++) {
        uint64_t ph = b->phoff + i * 56;
        if (ph + 56 > b->n)
            break;
        for (unsigned q = 0; q < sizeof pairs / sizeof pairs[0]; q++) {
            unsigned fa = pairs[q][0], fb = pairs[q][1];
            unsigned wa = fa == 0 || fa == 4 ? 4 : 8, wb = fb == 4 ? 4 : 8;
            for (unsigned x = 0; x < NV; x++)
                for (unsigned y = 0; y < NV; y++) {
                    put(work, ph + fa, wa, V[x]);
                    put(work, ph + fb, wb, V[y]);
                    run(work, b->n);
                    memcpy(work + ph + fa, b->bytes + ph + fa, wa);
                    memcpy(work + ph + fb, b->bytes + ph + fb, wb);
                }
        }
    }
    /* e_entry against e_phoff, and e_phoff against e_phnum */
    for (unsigned x = 0; x < NV; x++)
        for (unsigned y = 0; y < NV; y++) {
            put(work, 24, 8, V[x]);
            put(work, 32, 8, V[y]);
            run(work, b->n);
            memcpy(work + 24, b->bytes + 24, 16);
        }
    for (unsigned x = 0; x < NV; x++)
        for (unsigned y = 0; y <= 10; y++) {
            put(work, 32, 8, V[x]);
            put(work, 56, 2, y);
            run(work, b->n);
            memcpy(work + 32, b->bytes + 32, 8);
            memcpy(work + 56, b->bytes + 56, 2);
        }
    /* note: size fields against the segment size */
    if (b->note_size >= 24 && b->note_off + 24 <= b->n)
        for (unsigned x = 0; x < NV; x++)
            for (unsigned y = 0; y < NV; y++) {
                put(work, b->note_off, 4, V[x]);
                put(work, b->note_off + 4, 4, V[y]);
                run(work, b->n);
                memcpy(work + b->note_off, b->bytes + b->note_off, 8);
            }
}

static void sweep_truncation(const struct base *b)
{
    memcpy(work, b->bytes, b->n);
    memset(work + b->n, 0, 64);
    for (uint64_t n = 0; n <= b->n + 40; n++)
        run(work, n);
    /* limits of the size rule, over a zero-padded image */
    static uint8_t big[ELF_MAX_FILE + 8];
    memset(big, 0, sizeof big);
    memcpy(big, b->bytes, b->n);
    for (uint64_t n = ELF_MAX_FILE - 2; n <= ELF_MAX_FILE + 2; n++)
        run(big, n);
    static const uint64_t huge[] = {1ull << 32, 1ull << 40, 1ull << 63, ~0ull};
    for (unsigned i = 0; i < sizeof huge / sizeof huge[0]; i++)
        run(b->bytes, huge[i]);
}


/* ---- exact boundaries of the arithmetic rules ------------------------------------------------------------------- */

static void set_ph(uint8_t *w, uint64_t ph, uint64_t off, uint64_t vaddr, uint64_t filesz, uint64_t memsz)
{
    put(w, ph + 8, 8, off);
    put(w, ph + 16, 8, vaddr);
    put(w, ph + 32, 8, filesz);
    put(w, ph + 40, 8, memsz);
}

/* Sums that a rule compares with a limit, at the limit and one either side; values that satisfy the rules before them. */
static void sweep_boundaries(const struct base *b)
{
    const uint64_t END = 0x40000000ull, size = b->n;
    for (uint64_t i = 0; i < b->phnum; i++) {
        uint64_t ph = b->phoff + i * 56;
        if (ph + 56 > b->n || le(b->bytes + ph, 4) != 1)
            continue;
        memcpy(work, b->bytes, b->n);
        uint64_t off0 = le(b->bytes + ph + 8, 8), va0 = le(b->bytes + ph + 16, 8), fs0 = le(b->bytes + ph + 32, 8),
                 ms0 = le(b->bytes + ph + 40, 8);
        /* off + filesz against the file size */
        for (uint64_t off = 0; off <= size + PAGE; off += PAGE)
            for (int d = -2; d <= 2; d++) {
                int64_t fs = (int64_t)(size - off) + d;
                if (fs < 0)
                    continue;
                set_ph(work, ph, off, va0, (uint64_t)fs, (uint64_t)fs + 0x100);
                run(work, size);
                set_ph(work, ph, off, va0, (uint64_t)fs, (uint64_t)fs); /* filesz == memsz */
                run(work, size);
            }
        /* vaddr + memsz against the end of the user range, vaddr against its start */
        static const uint64_t vas[] = {0x3ff000, 0x400000, 0x401000, 0x1000000, 0x3fffe000, 0x3ffff000, 0x40000000};
        for (unsigned v = 0; v < sizeof vas / sizeof vas[0]; v++)
            for (int d = -2; d <= 2; d++) {
                int64_t ms = (int64_t)(END - vas[v]) + d;
                if (ms <= 0 || ms >= (1ll << 40))
                    continue;
                uint64_t fs = fs0 < (uint64_t)ms ? fs0 : (uint64_t)ms;
                set_ph(work, ph, off0, vas[v], fs, (uint64_t)ms);
                run(work, size);
            }
        /* the image size rule: total pages 255, 256, 257 */
        for (uint64_t pages = 250; pages <= 262; pages++)
            for (int d = -1; d <= 1; d++) {
                uint64_t ms = pages * PAGE + (uint64_t)(d + 1) - 1;
                set_ph(work, ph, off0, va0, fs0 < ms ? fs0 : ms, ms ? ms : 1);
                run(work, size);
            }
        set_ph(work, ph, off0, va0, fs0, ms0);
        /* the entry point against the executable segment's file part */
        if (le(b->bytes + ph + 4, 4) & 1) {
            static const int64_t e[] = {-1, 0, 1, -2};
            for (unsigned k = 0; k < 4; k++) {
                put(work, 24, 8, (uint64_t)((int64_t)va0 + e[k]));
                run(work, size);
                put(work, 24, 8, (uint64_t)((int64_t)(va0 + fs0) + e[k]));
                run(work, size);
                put(work, 24, 8, (uint64_t)((int64_t)(va0 + ms0) + e[k]));
                run(work, size);
            }
            put(work, 24, 8, le(b->bytes + 24, 8));
        }
    }
    /* order and overlap of consecutive loads: the next vaddr against the page-rounded end of the previous one */
    uint64_t loads[8], nl = 0;
    for (uint64_t i = 0; i < b->phnum && nl < 8; i++) {
        uint64_t ph = b->phoff + i * 56;
        if (ph + 56 <= b->n && le(b->bytes + ph, 4) == 1)
            loads[nl++] = ph;
    }
    for (uint64_t j = 1; j < nl; j++) {
        memcpy(work, b->bytes, b->n);
        uint64_t pa = loads[j - 1], pb = loads[j], pva = le(work + pa + 16, 8), pms = le(work + pa + 40, 8);
        uint64_t pfs = le(work + pa + 32, 8);
        for (int64_t gap = -2 * (int64_t)PAGE; gap <= 2 * (int64_t)PAGE; gap += PAGE)
            for (int d = -2; d <= 2; d++) {
                uint64_t rounded = (pva + pms + PAGE - 1) & ~(uint64_t)(PAGE - 1);
                put(work, pb + 16, 8, (uint64_t)((int64_t)rounded + gap));
                put(work, pa + 40, 8, (uint64_t)((int64_t)pms + d));
                put(work, pa + 32, 8, pfs < (uint64_t)((int64_t)pms + d) ? pfs : (uint64_t)((int64_t)pms + d));
                run(work, b->n);
            }
        memcpy(work + pa, b->bytes + pa, 56);
        memcpy(work + pb, b->bytes + pb, 56);
    }
    /* the program-header table against the end of the file */
    memcpy(work, b->bytes, b->n);
    for (int64_t d = -24; d <= 24; d += 8) {
        int64_t base = (int64_t)((size - 56 * b->phnum) & ~7ull) + d;
        if (base < 0)
            continue;
        put(work, 32, 8, (uint64_t)base);
        run(work, size);
    }
    put(work, 32, 8, b->phoff);
    /* note segment: offset and size against the file, and every size of the note walk */
    if (b->note_size) {
        for (uint64_t i = 0; i < b->phnum; i++) {
            uint64_t ph = b->phoff + i * 56;
            if (ph + 56 > b->n || le(b->bytes + ph, 4) != 4)
                continue;
            for (uint64_t fs = 0; fs <= 64; fs++) {
                put(work, ph + 32, 8, fs);
                put(work, ph + 40, 8, fs);
                run(work, size);
            }
            for (uint64_t fs = 11; fs <= 25; fs += 1)
                for (int d = -2; d <= 2; d++) {
                    put(work, ph + 32, 8, fs);
                    put(work, ph + 8, 8, (uint64_t)((int64_t)(size - fs) + d));
                    run(work, size);
                }
            memcpy(work + ph, b->bytes + ph, 56);
        }
    }
}

/* ---- structurally built images ------------------------------------------------------------------------------------ */

struct ph {
    uint32_t type, flags;
    uint64_t off, vaddr, filesz, memsz, align;
};
static const struct ph LIB[] = {
    {1, 5, 0x1000, 0x400000, 0x100, 0x100, 0x1000},   /* 0 text R X */
    {1, 4, 0x2000, 0x401000, 0x80, 0x80, 0x1000},     /* 1 rodata R */
    {1, 6, 0x3000, 0x402000, 0x10, 0x2000, 0x1000},   /* 2 data RW, bss tail */
    {4, 4, 0x4000, 0, 0x20, 0x20, 4},                 /* 3 note (filesz patched) */
    {0x6474e551u, 6, 0, 0, 0, 0, 16},                 /* 4 GNU_STACK RW */
    {1, 5, 0x5000, 0x403000, 0x10, 0x10, 0x1000},     /* 5 second text */
    {1, 4, 0x6000, 0x404000, 0x10, 0x10, 0x1000},     /* 6 another R load */
    {3, 4, 0x1000, 0, 0x10, 0x10, 1},                 /* 7 INTERP */
    {2, 6, 0x3000, 0x405000, 0x10, 0x10, 8},          /* 8 DYNAMIC */
    {7, 4, 0x2000, 0x406000, 0x10, 0x10, 8},          /* 9 TLS */
    {0, 0, 0, 0, 0, 0, 0},                            /* 10 NULL */
    {1, 4, 0x7000, 0x40a000, 0x1000, 0x100000, 0x1000}, /* 11 large bss R (1 MiB image) */
};
#define NLIB (sizeof LIB / sizeof LIB[0])

/* Note payloads (the PT_NOTE contents); each is a whole segment's bytes. */
struct note {
    uint8_t bytes[96];
    unsigned n;
};
static struct note NOTES[64];
static unsigned nnotes;

static void add_note(uint32_t namesz, const char *name4, uint32_t descsz, uint32_t type, uint32_t abi, unsigned extra, int twice)
{
    struct note *t = &NOTES[nnotes++];
    memset(t, 0, sizeof *t);
    unsigned n = 0;
    for (int k = 0; k < (twice ? 2 : 1); k++) {
        put(t->bytes, n, 4, namesz);
        put(t->bytes, n + 4, 4, descsz);
        put(t->bytes, n + 8, 4, type);
        memcpy(t->bytes + n + 12, name4, 4);
        put(t->bytes, n + 16, 4, abi);
        n += 20;
    }
    t->n = n + extra;
}

static void build_notes(void)
{
    for (unsigned abi = 0; abi <= 7; abi++)
        add_note(4, "VOS", 4, 1, abi, 0, 0);
    add_note(4, "VOS", 4, 1, 3, 0, 1);   /* duplicate */
    add_note(4, "VOS", 4, 2, 3, 0, 0);   /* other note type: no ABI note */
    add_note(4, "VOT", 4, 1, 3, 0, 0);   /* other name */
    add_note(3, "VOS", 4, 1, 3, 0, 0);   /* namesz 3 */
    add_note(5, "VOS", 4, 1, 3, 0, 0);   /* namesz 5 */
    add_note(4, "VOS", 0, 1, 3, 0, 0);   /* descsz 0 */
    add_note(4, "VOS", 8, 1, 3, 0, 0);   /* descsz 8 */
    add_note(4, "VOS", 4, 1, 3, 3, 0);   /* three trailing bytes */
    add_note(4, "VOS", 4, 1, 3, 12, 0);  /* trailing header with nothing in it */
    add_note(0xfffffff0u, "VOS", 4, 1, 3, 0, 0);
    add_note(4, "VOS", 0xfffffff0u, 1, 3, 0, 0);
    add_note(4, "VOS", 4, 1, 0xfffffff0u, 0, 0);
    add_note(0, "\0\0\0", 0, 0, 0, 0, 0); /* empty name and desc */
}

static unsigned build(uint8_t *img, const unsigned *seq, unsigned k, const struct note *nt, int64_t note_delta, uint64_t entry,
                      uint64_t phoff)
{
    memset(img, 0, 0x8000);
    memcpy(img, "\x7f" "ELF\2\1\1", 7);
    put(img, 16, 2, 2);
    put(img, 18, 2, 62);
    put(img, 20, 4, 1);
    put(img, 24, 8, entry);
    put(img, 32, 8, phoff);
    put(img, 52, 2, 64);
    put(img, 54, 2, 56);
    put(img, 56, 2, k);
    for (unsigned i = 0; i < k; i++) {
        struct ph p = LIB[seq[i]];
        if (p.type == 4) {
            p.filesz = (uint64_t)((int64_t)(nt ? nt->n : 0) + note_delta);
            p.memsz = p.filesz;
        }
        uint8_t *q = img + phoff + 56u * i;
        if (phoff + 56u * (i + 1) > 0x1000)
            break;
        put(q, 0, 4, p.type);
        put(q, 4, 4, p.flags);
        put(q, 8, 8, p.off);
        put(q, 16, 8, p.vaddr);
        put(q, 32, 8, p.filesz);
        put(q, 40, 8, p.memsz);
        put(q, 48, 8, p.align);
    }
    if (nt)
        memcpy(img + 0x4000, nt->bytes, nt->n);
    return 0x8000;
}

static void structural(void)
{
    static uint8_t img[0x8000];
    static const uint64_t entries[] = {0x400000, 0x400080, 0x4000ff, 0x400100, 0x401000, 0, 0x402000};
    unsigned seq[8];
    build_notes();
    /* all sequences of up to three entries, every note variant, a handful of entry points and note-size deltas */
    for (unsigned len = 1; len <= 3; len++) {
        unsigned total = 1;
        for (unsigned i = 0; i < len; i++)
            total *= NLIB;
        for (unsigned code = 0; code < total; code++) {
            unsigned c = code;
            for (unsigned i = 0; i < len; i++) {
                seq[i] = c % NLIB;
                c /= NLIB;
            }
            for (unsigned e = 0; e < 3; e++)
                run(img, build(img, seq, len, &NOTES[3], 0, entries[e], 64));
        }
    }
    /* the canonical five, every permutation, every note variant, every entry, note-size deltas */
    static const unsigned canon[5] = {0, 1, 2, 3, 4};
    unsigned perm[5];
    for (unsigned a = 0; a < 120; a++) {
        unsigned used = 0, code = a;
        for (unsigned i = 0; i < 5; i++) {
            unsigned pick = code % (5 - i), idx = 0;
            code /= (5 - i);
            for (unsigned j = 0; j < 5; j++)
                if (!(used >> j & 1) && idx++ == pick) {
                    perm[i] = canon[j];
                    used |= 1u << j;
                    break;
                }
        }
        for (unsigned nt = 0; nt < nnotes; nt++)
            for (unsigned e = 0; e < sizeof entries / sizeof entries[0]; e++)
                for (int d = -4; d <= 4; d += 4)
                    run(img, build(img, perm, 5, &NOTES[nt], d, entries[e], 64));
    }
    /* canonical set with one entry replaced by every library entry, and with extra entries appended */
    for (unsigned slot = 0; slot < 5; slot++)
        for (unsigned repl = 0; repl < NLIB; repl++) {
            memcpy(seq, canon, sizeof canon);
            seq[slot] = repl;
            for (unsigned nt = 0; nt < nnotes; nt++)
                run(img, build(img, seq, 5, &NOTES[nt], 0, 0x400000, 64));
        }
    for (unsigned extra = 0; extra < NLIB; extra++)
        for (unsigned more = 0; more < NLIB; more++) {
            memcpy(seq, canon, sizeof canon);
            seq[5] = extra;
            seq[6] = more;
            for (unsigned k = 5; k <= 7; k++)
                run(img, build(img, seq, k, &NOTES[3], 0, 0x400000, 64));
        }
    /* program-header table positions and counts */
    for (uint64_t phoff = 0; phoff <= 0x80; phoff += 4)
        for (unsigned k = 0; k <= 9; k++) {
            build(img, canon, k < 5 ? k : 5, &NOTES[3], 0, 0x400000, phoff);
            put(img, 56, 2, k); /* the count in the header, whatever the table really holds */
            run(img, 0x8000);
        }
    /* load flags on every combination, on each load of the canonical set */
    for (unsigned flags = 0; flags < 16; flags++)
        for (unsigned slot = 0; slot < 3; slot++) {
            build(img, canon, 5, &NOTES[3], 0, 0x400000, 64);
            put(img + 64 + 56u * slot, 4, 4, flags);
            run(img, 0x8000);
        }
}

/* ---- random phase ------------------------------------------------------------------------------------------------- */

static uint64_t rng = 0x564f5341314c4631ull;
static uint64_t next(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545f4914f6cdd1dull;
}

#define MAX_SEEDS 160
static uint8_t seeds[MAX_SEEDS][0x8000];
static size_t seed_len[MAX_SEEDS];
static unsigned nseeds;

static unsigned char seen_sig[ELF_NERRORS][ELF_MAX_LOADS + 1][8];
static unsigned long sigs_seen, last_new_case;

static void note_signature(int code, const struct elf_image *im)
{
    unsigned char *c = &seen_sig[code][im->nload < ELF_MAX_LOADS + 1 ? im->nload : ELF_MAX_LOADS][im->abi < 8 ? im->abi : 7];
    if (!*c) {
        *c = 1;
        sigs_seen++;
        last_new_case = cases;
    }
}

static void random_phase(unsigned long minimum, unsigned long cap, unsigned long plateau)
{
    in_random = 1;
    unsigned long start = cases;
    for (;;) {
        unsigned s = (unsigned)(next() % nseeds);
        size_t n = seed_len[s];
        memcpy(work, seeds[s], n);
        uint64_t size = n;
        unsigned muts = 1 + (unsigned)(next() % 4);
        for (unsigned m = 0; m < muts; m++) {
            uint64_t r = next();
            switch (r % 5) {
            case 0: /* bit flip, mostly in the structures */
                work[(r >> 8) % (r >> 40 & 1 ? n : (n < 0x300 ? n : 0x300))] ^= (uint8_t)(1u << (r >> 20 & 7));
                break;
            case 1: /* byte */
                work[(r >> 8) % n] = (uint8_t)(r >> 32);
                break;
            case 2: { /* a table field gets a boundary value */
                const struct field *t = r >> 8 & 1 ? HDR : PH;
                unsigned count = r >> 8 & 1 ? sizeof HDR / sizeof HDR[0] : sizeof PH / sizeof PH[0];
                const struct field *f = &t[(r >> 16) % count];
                uint64_t base = t == HDR ? 0 : le(work + 32, 8) + 56 * ((r >> 24) % 8);
                if (t == HDR || (base < n && base + 56 <= n))
                    put(work, base + f->off, f->width, V[(r >> 32) % NV]);
                break;
            }
            case 3: /* a note field */
                put(work, 0x4000 + NOTE[(r >> 8) % (sizeof NOTE / sizeof NOTE[0])].off, 4, V[(r >> 32) % NV]);
                break;
            default: /* truncation */
                size = (r >> 8) % (n + 8);
                break;
            }
        }
        run(work, size);
        unsigned long done = cases - start;
        /* stop once the minimum is run and no new (outcome, load count, ABI) signature appeared for `plateau` inputs */
        if ((done >= minimum && cases - last_new_case >= plateau) || done >= cap)
            break;
    }
    in_random = 0;
}

/* ---- main ----------------------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    uint8_t *region = mmap(NULL, PAGE + DATA + PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        return 2;
    front_guard = region;
    data = region + PAGE;
    rear_guard = data + DATA;
    if (mprotect(front_guard, PAGE, PROT_NONE) || mprotect(rear_guard, PAGE, PROT_NONE))
        return 2;

    /* messages */
    for (int e = -5; e <= (int)ELF_NERRORS + 8; e++)
        if (strcmp(elf_strerror(e), elf_strerror_reference(e))) {
            fprintf(stderr, "elf_strerror(%d): Rust \"%s\" C \"%s\"\n", e, elf_strerror(e), elf_strerror_reference(e));
            return 1;
        }
    for (int a = 0; a < ELF_NERRORS; a++)
        for (int b = a + 1; b < ELF_NERRORS; b++)
            if (!strcmp(elf_strerror(a), elf_strerror(b)))
                return 1;

    /* the contract's no-read cases: a pointer into a guard page must never be touched */
    for (uint64_t n = 0; n < 64; n++)
        run(rear_guard, n), run(NULL, n);
    {
        struct elf_image im;
        if (elf_validate(rear_guard, 63, &im) != ELF_ESIZE || elf_validate(NULL, ~0ull, &im) != ELF_ESIZE)
            return 1;
    }

    struct base bases[16];
    unsigned nb = 0;
    for (int i = 1; i < argc && nb < 16; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f)
            return 2;
        uint8_t *buf = malloc(ELF_MAX_FILE + 1);
        size_t n = buf ? fread(buf, 1, ELF_MAX_FILE + 1, f) : 0;
        fclose(f);
        if (n == 0 || n > 0x8000) {
            free(buf);
            continue; /* the structural/random phases use images up to 32 KiB; larger bases are skipped here */
        }
        struct elf_image im, ir;
        int rust = elf_validate(buf, n, &im), ref = elf_validate_reference(buf, n, &ir);
        if (rust != ref)
            fail("a base executable", buf, n, rust, ref);
        if (rust != ELF_OK) {
            free(buf);
            continue; /* only valid executables are bases: mutations of them are the interesting inputs */
        }
        bases[nb] = (struct base){argv[i], buf, n, 0, 0, 0, 0};
        locate(&bases[nb]);
        nb++;
    }
    if (!nb) {
        fprintf(stderr, "no valid base executable among the arguments\n");
        return 2;
    }
    for (unsigned i = 0; i < nb; i++) {
        run(bases[i].bytes, bases[i].n);
        sweep_truncation(&bases[i]);
        sweep_fields(&bases[i]);
        sweep_pairs(&bases[i]);
        sweep_boundaries(&bases[i]);
    }
    structural();

    /* every outcome must be reachable by the deterministic phases alone */
    unsigned long deterministic_cases = cases;
    for (unsigned c = 0; c < ELF_NERRORS; c++)
        if (!phase_hist[c]) {
            fprintf(stderr, "outcome %u (%s) not reached by the deterministic phases\n", c, elf_strerror(c));
            return 1;
        }

    /* random seeds: the bases and a spread of structurally built images */
    for (unsigned i = 0; i < nb && nseeds < MAX_SEEDS; i++) {
        memset(seeds[nseeds], 0, 0x8000);
        memcpy(seeds[nseeds], bases[i].bytes, bases[i].n);
        seed_len[nseeds++] = bases[i].n;
    }
    {
        static const unsigned canon[5] = {0, 1, 2, 3, 4};
        for (unsigned nt = 0; nt < nnotes && nseeds < MAX_SEEDS; nt++) {
            seed_len[nseeds] = build(seeds[nseeds], canon, 5, &NOTES[nt], 0, 0x400000, 64);
            nseeds++;
        }
    }
    random_phase(100000, 400000, 50000);

    /* Rust-only hardening outside the C contract: refuse, never fault, never write a bad pointer */
    {
        struct elf_image im;
        uint8_t *good = bases[0].bytes;
        _Alignas(8) uint8_t slot[sizeof(struct elf_image) + 8];
        struct elf_image *mis = (struct elf_image *)(slot + 1);
        if (elf_validate(good, bases[0].n, NULL) != ELF_ESIZE || elf_validate(good, bases[0].n, mis) != ELF_ESIZE ||
            elf_validate(NULL, bases[0].n, &im) != ELF_ESIZE ||
            elf_validate((uint8_t *)&im - 16, 64, &im) != ELF_ESIZE /* overlaps the output */)
            return 1;
    }

    for (unsigned i = 0; i < nb; i++)
        free(bases[i].bytes);
    printf("A1 validator differential: %lu inputs PASS (%lu deterministic, %lu random over %u bases and %u built seeds); "
           "codes, every image field and all %d messages agree; zero image after every failure; guard pages never touched\n",
           cases, deterministic_cases, cases - deterministic_cases, nb, nseeds, ELF_NERRORS);
    printf("outcomes (all inputs):");
    for (unsigned c = 0; c < ELF_NERRORS; c++)
        printf(" %u=%lu", c, hist[c]);
    printf("\nrandom phase stop: %lu signatures (outcome x loads x ABI) seen, the last new one at input %lu of %lu\n", sigs_seen,
           last_new_case, cases);
    printf("outcomes (random phase):");
    for (unsigned c = 0; c < ELF_NERRORS; c++)
        printf(" %u=%lu", c, hist_random[c]);
    printf("\n");
    return 0;
}
