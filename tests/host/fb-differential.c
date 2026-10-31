/* A3: differential of the Rust boot_framebuffer against the frozen C reference (tests/host/reference/bootinfo_ref.c, the accepted
 * arch/x86_64/bootinfo.c verbatim, built here under the names *_reference). Both validate exactly the same tag and must agree on
 * the returned message (by content), on every defined field of struct boot_fb (padding is not compared; the Rust side is asserted
 * to zero it) and on the zeroed output after every failure.
 *
 * Inputs: deterministic sweeps over structurally valid bases (every value of the type, bpp, size and each of the six channel
 * bytes; every width, height and pitch over and past their limits; the address and wrap boundaries of each base; field pairs
 * that interact in one rule; every framebuffer-tag count) and a bounded, seeded random phase with an explicit stopping rule.
 *
 * Memory safety: the tag is copied into a mapping whose neighbours are PROT_NONE guard pages, once flush with the rear guard and
 * once with the front guard, and exactly max(8, min(size, 38)) bytes are readable: the 8-byte header the contract guarantees,
 * and no more than the 38 bytes the C itself can read. A read outside that range by either implementation faults and fails the
 * test. When no tag may be read (no tag recorded, more than one, null) the pointer is a guard page. Hardening outside the C
 * contract (null or misaligned structures, overlapping storage) is checked for the Rust function only and is never run through
 * the C reference, whose behavior there is undefined.
 *
 * usage: fb-differential     exit 0: no mismatch, every outcome produced. */
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

const char *boot_framebuffer_reference(const struct boot_info *, struct boot_fb *);

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

/* The layouts the Rust glue pins, checked against this C compiler. */
_Static_assert(sizeof(struct boot_fb) == 40 && _Alignof(struct boot_fb) == 8, "boot_fb size");
_Static_assert(offsetof(struct boot_fb, kind) == 0 && offsetof(struct boot_fb, addr) == 8 && offsetof(struct boot_fb, size) == 16,
               "boot_fb 1");
_Static_assert(offsetof(struct boot_fb, pitch) == 24 && offsetof(struct boot_fb, width) == 28 && offsetof(struct boot_fb, height) == 32,
               "boot_fb 2");
_Static_assert(sizeof(struct mb2_tag_framebuffer) == 32 && offsetof(struct mb2_tag_framebuffer, addr) == 8, "tag base");
_Static_assert(offsetof(struct mb2_tag_framebuffer, pitch) == 16 && offsetof(struct mb2_tag_framebuffer, width) == 20, "tag 1");
_Static_assert(offsetof(struct mb2_tag_framebuffer, height) == 24 && offsetof(struct mb2_tag_framebuffer, bpp) == 28, "tag 2");
_Static_assert(offsetof(struct mb2_tag_framebuffer, type) == 29 && sizeof(struct mb2_fb_rgb) == 6, "tag 3");
_Static_assert(sizeof(struct boot_info) == 1624 && offsetof(struct boot_info, fb) == 1592 && offsetof(struct boot_info, fb_tags) == 1600,
               "boot_info fields read by the port");

#define PAGE 4096u
#define DATA PAGE
static uint8_t *front_guard, *data, *rear_guard;
static unsigned long cases;
static int in_random;

static const char *MSG[] = {
    "no framebuffer information", "more than one framebuffer tag", "framebuffer tag truncated",
    "EGA text framebuffer other than 80x25 at 0xB8000", "framebuffer is not RGB (indexed colour is not supported)",
    "RGB framebuffer tag without its channel layout", "framebuffer is not 32 bits per pixel",
    "pixel layout is not BGRX (red 16, green 8, blue 0, 8 bits each)", "framebuffer dimensions outside 320x200 to 8192x8192",
    "framebuffer pitch below width x 4, unaligned or too large", "framebuffer larger than 32 MiB",
    "framebuffer address zero, unaligned or wrapping",
};
#define NMSG (sizeof MSG / sizeof MSG[0])
#define NOUT (NMSG + 2) /* 0: EGA success, 1: BGRX32 success, i + 2: MSG[i] */
static unsigned long hist[NOUT], hist_random[NOUT];

static int outcome(const char *s, const struct boot_fb *f)
{
    if (!s)
        return f->kind == BOOT_FB_EGA_TEXT ? 0 : f->kind == BOOT_FB_BGRX32 ? 1 : -1;
    for (unsigned i = 0; i < NMSG; i++)
        if (!strcmp(s, MSG[i]))
            return (int)i + 2;
    return -1;
}

/* Fields only: struct padding ([4, 8) and [36, 40)) is unspecified and never compared. */
static int same(const struct boot_fb *a, const struct boot_fb *b)
{
    return a->kind == b->kind && a->addr == b->addr && a->size == b->size && a->pitch == b->pitch && a->width == b->width &&
           a->height == b->height;
}

static int padding_zero(const struct boot_fb *a)
{
    const uint8_t *p = (const uint8_t *)a;
    for (int i = 4; i < 8; i++)
        if (p[i])
            return 0;
    for (int i = 36; i < 40; i++)
        if (p[i])
            return 0;
    return 1;
}

/* One test input: the tag's fields, how many tags the loader walk recorded, and whether the pointer is null. */
struct T {
    uint32_t size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t bpp, type, rgb[6];
    uint32_t fb_tags;
    int null_fb;
};

static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void encode(const struct T *t, uint8_t *out) /* the 38 bytes the code can read */
{
    memset(out, 0, 38);
    put32(out, 8);
    put32(out + 4, t->size);
    memcpy(out + 8, &t->addr, 8);
    put32(out + 16, t->pitch);
    put32(out + 20, t->width);
    put32(out + 24, t->height);
    out[28] = t->bpp;
    out[29] = t->type;
    memcpy(out + 32, t->rgb, 6);
}

__attribute__((noreturn)) static void fail(const char *what, const struct T *t, const char *rust, const char *ref)
{
    fprintf(stderr, "MISMATCH (%s): size %u type %u bpp %u %ux%u pitch %u addr %#llx tags %u null %d Rust=%s C=%s\n", what, t->size,
            t->type, t->bpp, t->width, t->height, t->pitch, (unsigned long long)t->addr, t->fb_tags, t->null_fb, rust ? rust : "(success)",
            ref ? ref : "(success)");
    exit(1);
}

static void note_signature(int oc, uint32_t size);

/* One input, both implementations, both guard placements. */
static void run(const struct T *t)
{
    static struct boot_info bi;
    static struct boot_fb a, b;
    uint8_t tag[38];
    int readable = t->fb_tags == 1 && !t->null_fb;
    uint32_t n = t->size < 38 ? t->size : 38;

    if (n < 8)
        n = 8;
    encode(t, tag);
    cases++;
    for (int place = 0; place < 2; place++) {
        uint8_t *p = NULL;
        if (readable) {
            p = place == 0 ? data + DATA - n : data; /* flush with the rear guard, then with the front guard */
            memcpy(p, tag, n);
        } else if (!t->null_fb) {
            p = place ? front_guard : rear_guard; /* never to be read */
        }
        memset(&bi, 0, sizeof bi);
        bi.fb = (const struct mb2_tag_framebuffer *)p;
        bi.fb_tags = t->fb_tags;
        memset(&a, 0xA5, sizeof a);
        memset(&b, 0xA5, sizeof b);
        const char *x = boot_framebuffer(&bi, &a);
        const char *y = boot_framebuffer_reference(&bi, &b);
        int ox = outcome(x, &a), oy = outcome(y, &b);
        if (ox < 0 || oy < 0 || ox != oy)
            fail("result", t, x, y);
        if (!same(&a, &b))
            fail("boot_fb fields", t, x, y);
        if (!padding_zero(&a))
            fail("Rust padding not zero", t, x, y);
        if (x && (a.kind != BOOT_FB_NONE || a.addr || a.size || a.pitch || a.width || a.height))
            fail("output not zero after a failure", t, x, y);
        if (place == 0) {
            hist[ox]++;
            if (in_random)
                hist_random[ox]++;
            note_signature(ox, t->size);
        }
    }
}

/* ---- bases and boundary values ------------------------------------------------------------------------------------ */

static const uint8_t BGRX[6] = {16, 8, 8, 8, 0, 8};

static struct T bgrx(uint64_t addr, uint32_t w, uint32_t h, uint32_t pitch)
{
    struct T t = {.size = 38, .addr = addr, .pitch = pitch, .width = w, .height = h, .bpp = 32, .type = 1, .fb_tags = 1};
    memcpy(t.rgb, BGRX, 6);
    return t;
}

static struct T bases[16];
static unsigned nbases;

static void make_bases(void)
{
    bases[nbases++] = bgrx(0x80000000u, 1280, 800, 5120);
    bases[nbases++] = bgrx(0xFD000000u, 1024, 768, 4096);
    bases[nbases++] = bgrx(0x80000000u, 640, 480, 2560);
    bases[nbases++] = bgrx(0x80000000u, 1920, 1080, 7680);
    bases[nbases++] = bgrx(0x4000000000ull, 3840, 2160, 15360);
    bases[nbases++] = bgrx(0x1000, 320, 200, 1280);
    bases[nbases++] = bgrx(0x80000000u, 8192, 1024, 32768); /* exactly 32 MiB */
    bases[nbases++] = bgrx(4, 320, 200, 1280);
    bases[nbases++] = bgrx(0x80000000u, 1280, 800, 5120 + 4 * 16); /* padded pitch */
    struct T e = {.size = 32, .addr = 0xB8000, .pitch = 160, .width = 80, .height = 25, .bpp = 16, .type = 2, .fb_tags = 1};
    bases[nbases++] = e;
    struct T e2 = e; /* an EGA tag with trailing bytes (the extra bytes are ignored) */
    e2.size = 64;
    bases[nbases++] = e2;
    struct T big = bgrx(0x80000000u, 1280, 800, 5120); /* a longer RGB tag */
    big.size = 64;
    bases[nbases++] = big;
}

static const uint64_t A64[] = {0, 1, 2, 3, 4, 5, 8, 0xFFF, 0x1000, 0xB7FFF, 0xB8000, 0xB8001, 0xB8004, 0xFFFFFFFCull, 0xFFFFFFFFull,
                               0x100000000ull, 0x100000004ull, 0x7FFFFFFFFFFFFFFCull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFCull,
                               0xFFFFFFFFFFFFFFFFull};
static const uint32_t U32[] = {0, 1, 2, 3, 4, 7, 8, 15, 16, 24, 31, 32, 33, 36, 37, 38, 39, 63, 64, 160, 199, 200, 201, 319, 320, 321, 639,
                               640, 1280, 1281, 5116, 5120, 5124, 8191, 8192, 8193, 32768, 65535, 65536, 65537, 0x7FFFFFFF, 0x80000000,
                               0xFFFFFFFCu, 0xFFFFFFFFu};
#define NA64 (sizeof A64 / sizeof A64[0])
#define NU32 (sizeof U32 / sizeof U32[0])

enum { F_SIZE, F_ADDR, F_PITCH, F_WIDTH, F_HEIGHT, F_BPP, F_TYPE, NFIELDS };

static void set_field(struct T *t, int f, uint64_t v)
{
    switch (f) {
    case F_SIZE: t->size = (uint32_t)v; break;
    case F_ADDR: t->addr = v; break;
    case F_PITCH: t->pitch = (uint32_t)v; break;
    case F_WIDTH: t->width = (uint32_t)v; break;
    case F_HEIGHT: t->height = (uint32_t)v; break;
    case F_BPP: t->bpp = (uint8_t)v; break;
    default: t->type = (uint8_t)v; break;
    }
}

static void sweep_fields(const struct T *base)
{
    for (int f = 0; f < NFIELDS; f++) {
        if (f == F_ADDR) {
            for (unsigned i = 0; i < NA64; i++) {
                struct T t = *base;
                t.addr = A64[i];
                run(&t);
            }
        } else {
            for (unsigned i = 0; i < NU32; i++) {
                struct T t = *base;
                set_field(&t, f, U32[i]);
                run(&t);
            }
        }
    }
}

/* Pairs of fields that interact in one rule. */
static void sweep_pairs(const struct T *base)
{
    static const int P[][2] = {{F_WIDTH, F_PITCH}, {F_PITCH, F_HEIGHT}, {F_ADDR, F_PITCH}, {F_ADDR, F_HEIGHT}, {F_BPP, F_TYPE},
                               {F_TYPE, F_SIZE}, {F_WIDTH, F_HEIGHT}, {F_BPP, F_SIZE}, {F_ADDR, F_TYPE}};
    for (unsigned k = 0; k < sizeof P / sizeof P[0]; k++)
        for (unsigned i = 0; i < NU32; i++)
            for (unsigned j = 0; j < NU32; j++) {
                struct T t = *base;
                set_field(&t, P[k][0], P[k][0] == F_ADDR ? A64[i % NA64] : U32[i]);
                set_field(&t, P[k][1], P[k][1] == F_ADDR ? A64[j % NA64] : U32[j]);
                run(&t);
            }
}

static void sweep_exhaustive(const struct T *base)
{
    for (uint32_t v = 0; v <= 256; v++) { /* every value of the small fields */
        struct T t = *base;
        t.size = v;
        run(&t);
        t = *base;
        t.type = (uint8_t)v;
        run(&t);
        t = *base;
        t.bpp = (uint8_t)v;
        run(&t);
        if (v < 256)
            for (int c = 0; c < 6; c++) { /* every value of each channel byte */
                t = *base;
                t.rgb[c] = (uint8_t)v;
                run(&t);
            }
    }
    for (uint32_t v = 0; v <= 9000; v++) {
        struct T t = *base;
        t.width = v;
        run(&t);
        t = *base;
        t.height = v;
        run(&t);
    }
    for (uint32_t v = 0; v <= 70000; v++) { /* every pitch past the limit */
        struct T t = *base;
        t.pitch = v;
        run(&t);
    }
    for (uint32_t n = 0; n <= 4; n++) /* every tag count */
        for (int nul = 0; nul < 2; nul++) {
            struct T t = *base;
            t.fb_tags = n;
            t.null_fb = nul;
            run(&t);
        }
    for (uint32_t n = 0xFFFFFFF0u; n != 0; n++) {
        struct T t = *base;
        t.fb_tags = n;
        run(&t);
    }
    /* the wrap boundary of this base: addr + size around 2^64 */
    uint64_t bytes = (uint64_t)base->pitch * base->height;
    for (int d = -9; d <= 9; d++) {
        struct T t = *base;
        t.addr = (0ull - bytes + (uint64_t)(int64_t)d) & ~3ull;
        run(&t);
        t.addr = 0ull - bytes + (uint64_t)(int64_t)d;
        run(&t);
    }
}

/* ---- seeded random phase ------------------------------------------------------------------------------------------ */

static uint64_t rng = 0x564f53334642ull;
static uint64_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

#define SIGS 4096
static uint32_t sig_seen[SIGS];
static unsigned long classes, last_new;

/* The structural class of a result: the outcome and the size class of the tag (below 32, 32 to 37, 38 and above). */
static void note_signature(int oc, uint32_t size)
{
    uint32_t sig = (uint32_t)oc | (size < 32 ? 0u : size < 38 ? 1u : 2u) << 5;
    uint32_t h = (sig * 2654435761u) & (SIGS - 1);
    while (sig_seen[h] && sig_seen[h] != sig + 1)
        h = (h + 1) & (SIGS - 1);
    if (!sig_seen[h]) {
        sig_seen[h] = sig + 1;
        classes++;
        last_new = cases;
    }
}

static int converged;

static void random_phase(unsigned long minimum, unsigned long cap, unsigned long plateau)
{
    unsigned long done = 0;
    in_random = 1;
    for (;;) {
        struct T t = bases[next() % nbases];
        uint32_t edits = next() % 4; /* 0 to 3 edits: unedited bases keep the successes flowing */
        for (uint32_t e = 0; e < edits; e++) {
            int f = (int)(next() % (NFIELDS + 6));
            uint64_t v = next() % 3 == 0 ? next() : f == F_ADDR ? A64[next() % NA64] : U32[next() % NU32];
            if (f < NFIELDS)
                set_field(&t, f, v);
            else
                t.rgb[f - NFIELDS] = next() % 3 == 0 ? (uint8_t)next() : BGRX[f - NFIELDS] + (uint8_t)(next() % 3) - 1;
        }
        switch (next() % 16) {
        case 0: t.fb_tags = 0; break;
        case 1: t.fb_tags = 2 + (uint32_t)(next() % 3); break;
        case 2: t.null_fb = 1; break;
        default: break;
        }
        run(&t);
        done++;
        int every = 1;
        for (unsigned i = 0; i < NOUT; i++)
            if (hist_random[i] < 10)
                every = 0;
        if (done >= minimum && cases - last_new >= plateau && every) {
            converged = 1;
            break;
        }
        if (done >= cap)
            break;
    }
    in_random = 0;
    printf("random phase: %lu inputs; %lu structural classes (outcome x tag-size class), last new at input %lu; %s\n", done, classes,
           last_new, converged ? "stopped on the plateau rule with every outcome seen at least 10 times" : "ended at the cap, NOT converged");
}

/* ---- hardening outside the C contract: the Rust function only ----------------------------------------------------- */

static void hardening(void)
{
    static struct boot_info bi __attribute__((aligned(8)));
    static struct boot_fb out __attribute__((aligned(8)));
    static uint8_t buf[128] __attribute__((aligned(8)));
    const char *none = "no framebuffer information";
    struct T good = bgrx(0x80000000u, 1280, 800, 5120);
    struct T t = good;
    uint8_t *tag = data;

    encode(&good, tag);
    memset(&bi, 0, sizeof bi);
    bi.fb = (const struct mb2_tag_framebuffer *)tag;
    bi.fb_tags = 1;
#define REFUSED(call, what)                                                                                                                      \
    do {                                                                                                                                         \
        memset(&out, 0xA5, sizeof out);                                                                                                          \
        const char *r_ = (call);                                                                                                                 \
        if (!r_ || strcmp(r_, none) || ((uint8_t *)&out)[0] != 0xA5 || ((uint8_t *)&out)[39] != 0xA5)                                            \
            fail(what, &t, r_, none);                                                                                                            \
    } while (0)
    REFUSED(boot_framebuffer(NULL, &out), "null bi not refused without writing");
    REFUSED(boot_framebuffer(&bi, NULL), "null fb not refused");
    REFUSED(boot_framebuffer(&bi, (struct boot_fb *)((uint8_t *)&out + 1)), "misaligned fb not refused without writing");
    REFUSED(boot_framebuffer((const struct boot_info *)((uint8_t *)&bi - 1), &out), "misaligned bi not refused");
    /* an output that lies inside the boot_info storage is refused (the C would zero the fields it is about to read) */
    {
        static struct boot_info big __attribute__((aligned(8)));
        big = bi;
        const char *r = boot_framebuffer(&big, (struct boot_fb *)((uint8_t *)&big + 8));
        if (!r || strcmp(r, none) || big.fb != bi.fb || big.fb_tags != 1)
            fail("fb overlapping bi not refused without writing", &t, r, none);
    }
    /* an output that lies on the tag is refused without writing: the tag keeps its bytes */
    {
        memset(buf, 0, sizeof buf);
        encode(&good, buf);
        bi.fb = (const struct mb2_tag_framebuffer *)buf;
        uint8_t keep[sizeof buf];
        memcpy(keep, buf, sizeof buf);
        const char *r = boot_framebuffer(&bi, (struct boot_fb *)(buf + 8));
        if (!r || strcmp(r, none) || memcmp(buf, keep, sizeof buf))
            fail("fb overlapping the tag not refused without writing", &t, r, none);
        bi.fb = (const struct mb2_tag_framebuffer *)tag;
    }
    memset(&out, 0xA5, sizeof out); /* and a good call still works afterwards */
    const char *r = boot_framebuffer(&bi, &out);
    if (r || out.kind != BOOT_FB_BGRX32 || out.addr != 0x80000000u || out.size != 5120ull * 800)
        fail("a valid tag was refused after the hardening checks", &t, r, NULL);
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
    unsigned long start = cases;
    for (unsigned i = 0; i < nbases; i++) {
        sweep_fields(&bases[i]);
        sweep_pairs(&bases[i]);
        sweep_exhaustive(&bases[i]);
    }
    unsigned long deterministic = cases - start;
    unsigned unseen = 0;
    for (unsigned i = 0; i < NOUT; i++)
        if (!hist[i]) {
            fprintf(stderr, "outcome never produced in the deterministic phase: %s\n",
                    i == 0 ? "EGA success" : i == 1 ? "BGRX32 success" : MSG[i - 2]);
            unseen++;
        }
    if (unseen)
        return 3;
    random_phase(100000, 400000, 50000);
    printf("A3 boot_framebuffer differential: %lu inputs PASS (%lu deterministic over %u bases, %lu random); message and every defined "
           "boot_fb field agree, zeroed after every failure, Rust padding zero, guard pages never touched\n",
           cases, deterministic, nbases, cases - deterministic);
    printf("outcomes (all inputs): ega=%lu bgrx=%lu", hist[0], hist[1]);
    for (unsigned i = 0; i < NMSG; i++)
        printf(" e%u=%lu", i + 1, hist[i + 2]);
    printf("\noutcomes (random phase): ega=%lu bgrx=%lu", hist_random[0], hist_random[1]);
    for (unsigned i = 0; i < NMSG; i++)
        printf(" e%u=%lu", i + 1, hist_random[i + 2]);
    printf("\n");
    return 0;
}
