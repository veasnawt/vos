#include "selftest.h"
#include "console.h"
#include "lib.h"
#include "memlayout.h"

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

static int streq(const char *a, const char *b)
{
    return a && b && strlen(a) == strlen(b) && memcmp(a, b, strlen(a)) == 0;
}

static int contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; hay && *hay; hay++)
        if (memcmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

/* ---- synthetic Multiboot2 info blocks ---- */

static uint8_t blk[4096] __attribute__((aligned(8)));
static uint32_t blk_len;

static void blk_begin(void)
{
    memset(blk, 0, sizeof(blk));
    blk_len = 8;
}

/*
 * Append a tag header (caller fills the payload). A declared size that does not
 * fit in the buffer only stores the header: that is how the "tag larger than
 * the block" case is made.
 */
static uint32_t *blk_tag(uint32_t type, uint32_t size)
{
    static uint32_t scratch[64];
    uint32_t rounded = (size + 7) & ~7u;
    uint32_t *t;

    if (blk_len + 8 > sizeof(blk))
        return scratch;
    t = (uint32_t *)(blk + blk_len);
    t[0] = type;
    t[1] = size;
    blk_len += blk_len + rounded <= sizeof(blk) ? rounded : 8;
    return t;
}

static void blk_str(uint32_t type, const char *s, int terminate)
{
    uint32_t n = strlen(s) + (terminate ? 1 : 0);
    uint32_t *t = blk_tag(type, 8 + n);

    memcpy(&t[2], s, n);
}

static void blk_module(uint32_t start, uint32_t end, const char *cmd)
{
    uint32_t n = strlen(cmd) + 1;
    uint32_t *t = blk_tag(3, 16 + n);

    t[2] = start;
    t[3] = end;
    memcpy(&t[4], cmd, n);
}

struct me { uint64_t base, len; uint32_t type; };

/* Memory-map tag with the given entry version and `extra` trailing bytes. */
static void blk_mmap_raw(const struct me *e, int n, uint32_t version, uint32_t extra)
{
    uint32_t *t = blk_tag(6, 16 + 24 * n + extra);

    t[2] = 24;
    t[3] = version;
    for (int i = 0; i < n; i++) {
        uint64_t *p = (uint64_t *)&t[4 + 6 * i];
        p[0] = e[i].base;
        p[1] = e[i].len;
        ((uint32_t *)p)[4] = e[i].type;
    }
}

static void blk_mmap(const struct me *e, int n)
{
    blk_mmap_raw(e, n, 0, 0);
}

static void blk_end(void)
{
    blk_tag(0, 8);
    ((uint32_t *)blk)[0] = blk_len;
}

/* Standard map: low RAM, BIOS hole, RAM [1 MiB, 128 MiB). */
static const struct me std_map[] = {
    { 0, 0x9FC00, 1 }, { 0x9FC00, 0x60400, 2 }, { 0x100000, 0x7F00000, 1 },
};
#define N_STD (int)(sizeof(std_map) / sizeof(std_map[0]))

#define FAKE_INFO   0x300000ULL
#define K_START     0x100000ULL
#define K_END       0x10E000ULL

static struct boot_info bi;

static const char *parse(void)
{
    return mb2_parse(blk, FAKE_INFO, &bi);
}

static const char *reserve(struct resv_table *t)
{
    resv_init(t);
    return boot_reserve(&bi, K_START, K_END, t);
}

/* Build: std map + one module + optional cmdline, then parse and reserve. */
static const char *module_case(uint32_t s, uint32_t e, struct resv_table *t)
{
    const char *err;

    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_module(s, e, "m");
    blk_end();
    if ((err = parse()))
        return err;
    return reserve(t);
}

static void test_parse(void)
{
    struct resv_table t;

    /* Valid block: command line, two modules, memory map. */
    blk_begin();
    blk_str(1, "vos.test=1", 1);
    blk_mmap(std_map, N_STD);
    blk_module(0x400000, 0x401000, "mod.a=1");
    blk_module(0x500000, 0x500010, "");
    blk_end();
    expect(parse() == NULL, "valid block parses");
    expect(streq(bi.cmdline, "vos.test=1"), "kernel command line parsed");
    expect(bi.module_count == 2, "two modules parsed");
    expect(bi.modules[0].start == 0x400000 && bi.modules[0].end == 0x401000, "module range");
    expect(streq(bi.modules[0].cmdline, "mod.a=1") && streq(bi.modules[1].cmdline, ""),
           "module command lines stay separate from the kernel's");
    expect(reserve(&t) == NULL, "valid modules reserve");
    expect(resv_overlaps(&t, 0x400000, 0x401000), "module 0 reserved");
    expect(resv_overlaps(&t, 0x500000, 0x501000), "module 1 reserved (rounded out)");
    expect(!resv_overlaps(&t, 0x401000, 0x500000), "gap between modules not reserved");
    expect(resv_overlaps(&t, 0, 0x100000) && resv_overlaps(&t, 0xFF000, 0x100000),
           "first 1 MiB reserved");
    expect(resv_overlaps(&t, K_START, K_END), "kernel image reserved");
    expect(resv_overlaps(&t, FAKE_INFO, FAKE_INFO + 1), "info block reserved");
    expect(!resv_overlaps(&t, 0x1000000, 0x2000000), "unrelated RAM not reserved");

    /* No module, no command line. */
    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_end();
    expect(parse() == NULL && bi.module_count == 0 && bi.cmdline == NULL,
           "no module / no command line is accepted");
    expect(reserve(&t) == NULL && t.count == 3, "base reservations only: low, kernel, info");

    /* Malformed tags. */
    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_tag(3, 12);                      /* truncated module header */
    blk_end();
    expect(parse() != NULL, "truncated module tag rejected");

    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_tag(3, 17);                      /* header + 1 byte, but the byte is not NUL */
    ((uint32_t *)(blk + blk_len - 24))[2] = 0x400000;
    ((uint32_t *)(blk + blk_len - 24))[3] = 0x401000;
    blk[blk_len - 8] = 'x';
    blk_end();
    expect(parse() != NULL, "unterminated module command line rejected");

    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_str(1, "abc", 0);
    blk_end();
    expect(parse() != NULL, "unterminated kernel command line rejected");

    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_str(1, "a", 1);
    blk_str(1, "b", 1);
    blk_end();
    expect(parse() != NULL, "duplicate command line tag rejected");

    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_tag(3, 8000);                    /* tag larger than the whole block */
    blk_end();
    expect(parse() != NULL, "tag size beyond the info block rejected");

    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_tag(7, 4);                       /* smaller than a tag header */
    blk_end();
    expect(parse() != NULL, "tag smaller than its header rejected");

    blk_begin();
    blk_mmap(std_map, N_STD);
    for (int i = 0; i < BOOT_MAX_MODULES + 1; i++)
        blk_module(0x400000 + 0x1000 * i, 0x401000 + 0x1000 * i, "m");
    blk_end();
    expect(parse() != NULL, "too many modules rejected");

    blk_begin();
    blk_module(0x400000, 0x401000, "m");
    blk_end();
    expect(parse() != NULL, "block without memory map rejected");

    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_end();
    blk_len -= 8;                        /* drop the end tag from the walk */
    ((uint32_t *)blk)[0] = blk_len;
    expect(parse() != NULL, "block without end tag rejected");

    /* Memory-map tag validation. */
    blk_begin();
    blk_mmap_raw(std_map, N_STD, 1, 0);
    blk_end();
    expect(parse() != NULL, "memory map entry_version 1 rejected");
    blk_begin();
    blk_mmap_raw(std_map, N_STD, 0, 8);
    blk_end();
    expect(parse() != NULL, "memory map with a trailing partial entry rejected");
    blk_begin();
    blk_mmap_raw(std_map, 0, 0, 0);
    blk_end();
    expect(parse() != NULL, "empty memory map rejected");
    blk_begin();
    blk_mmap_raw(std_map, N_STD, 0, 0);
    blk_end();
    expect(parse() == NULL && bi.mmap_entries == N_STD, "well-formed memory map accepted");
    expect(boot_mmap_entry(&bi, 2)->base == 0x100000 && boot_mmap_entry(&bi, 2)->type == 1,
           "memory map entries read back");

    expect(mb2_parse(blk, FAKE_INFO + 4, &bi) != NULL, "misaligned info address rejected");
    expect(mb2_parse(blk, 0, &bi) != NULL, "null info address rejected");
    expect(mb2_parse(blk, BOOT_MAP_LIMIT, &bi) != NULL, "info address above 1 GiB rejected");

    /* Invalid module ranges: the parser alone must reject them (reserve_ram also would). */
    static const struct { uint32_t s, e; const char *what; } bad[] = {
        { 0x400000, 0x400000, "parser rejects an empty module" },
        { 0x401000, 0x400000, "parser rejects a reversed module" },
        { 0x400000, 0x40000001, "parser rejects a module ending above 1 GiB" },
        { 0xFFFFF000, 0xFFFFFFFF, "parser rejects a module near 4 GiB" },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        blk_begin();
        blk_mmap(std_map, N_STD);
        blk_module(bad[i].s, bad[i].e, "m");
        blk_end();
        expect(parse() != NULL, bad[i].what);
    }
    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_module(0x3FFFF000, 0x40000000, "m");
    blk_end();
    expect(parse() == NULL, "parser accepts a module ending exactly at 1 GiB");
    expect(reserve(&t) != NULL, "...but 1 GiB is not RAM in this map, so reserve refuses it");

    /* Invalid module ranges (whole path). */
    expect(module_case(0x400000, 0x400000, &t) != NULL, "empty module rejected");
    expect(module_case(0x401000, 0x400000, &t) != NULL, "reversed module rejected");
    expect(module_case(0x400000, 0x40000001, &t) != NULL, "module end above 1 GiB rejected");
    expect(module_case(0x3FFFF000, 0x40000000, &t) != NULL, "module in the last page: not RAM");
    expect(module_case(0xFFFFF000, 0xFFFFFFFF, &t) != NULL, "module near 4 GiB rejected");
}

static void test_conflicts(void)
{
    struct resv_table t;

    expect(module_case(0x400000, 0x401000, &t) == NULL, "baseline module accepted");
    /* U1: low RAM is a legitimate loader location (GRUB on UEFI); see test_low_placement. */
    expect(module_case(0x80000, 0x81000, &t) == NULL && resv_overlaps(&t, 0x80000, 0x81000),
           "module in low RAM below 1 MiB accepted and reserved");
    expect(module_case(0xFF000, 0x101000, &t) != NULL, "module straddling 1 MiB rejected");
    expect(module_case(0x108000, 0x109000, &t) != NULL, "module inside the kernel span rejected");
    expect(module_case(0x10D000, 0x10F000, &t) != NULL, "module overlapping kernel end rejected");
    expect(module_case(0x300000, 0x301000, &t) != NULL, "module overlapping info rejected");
    expect(module_case(0x7FFF000, 0x8001000, &t) != NULL, "module running past RAM rejected");
    expect(module_case(0x9000000, 0x9001000, &t) != NULL, "module outside any RAM rejected");

    /* Two modules overlapping each other. */
    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_module(0x400000, 0x402000, "a");
    blk_module(0x401000, 0x403000, "b");
    blk_end();
    expect(parse() == NULL && reserve(&t) != NULL, "overlapping modules rejected");
    expect(t.count == 4 && resv_overlaps(&t, 0x400000, 0x402000) &&
           !resv_overlaps(&t, 0x402000, 0x404000),
           "reservations before the failure are kept, the rejected module adds none");

    /* Contradictory map: a reserved entry inside the available range. */
    static const struct me contra[] = {
        { 0, 0x9FC00, 1 }, { 0x100000, 0x7F00000, 1 }, { 0x500000, 0x100000, 2 },
    };
    blk_begin();
    blk_mmap(contra, 3);
    blk_module(0x400000, 0x401000, "ok");
    blk_module(0x4FF000, 0x501000, "touches reserved");
    blk_end();
    expect(parse() == NULL && reserve(&t) != NULL, "module touching a reserved entry rejected");
    blk_begin();
    blk_mmap(contra, 3);
    blk_module(0x400000, 0x401000, "ok");
    blk_end();
    expect(parse() == NULL && reserve(&t) == NULL, "module clear of the reserved entry accepted");

    /* Overflowing entries: a reserved entry running past 2^64 and an available one. */
    static const struct me ovf[] = {
        { 0x100000, 0x7F00000, 1 }, { 0xFFFFFFFFFFFFF000ULL, 0x2000, 2 },
        { 0xFFFFFFFFFFFFF000ULL, 0x2000, 1 },
    };
    blk_begin();
    blk_mmap(ovf, 3);
    blk_module(0x400000, 0x401000, "m");
    blk_end();
    expect(parse() == NULL && reserve(&t) == NULL, "overflowing entries do not break the check");
    static const struct me ovf2[] = {
        { 0x100000, 0x7F00000, 1 }, { 0x6000000, 0xFFFFFFFFFFFFFFFFULL, 2 },
    };
    blk_begin();
    blk_mmap(ovf2, 2);
    blk_module(0x6100000, 0x6101000, "m");
    blk_end();
    expect(parse() == NULL && reserve(&t) != NULL, "reserved entry running to 2^64 protects memory");

    /* Kernel image / info outside usable RAM. */
    static const struct me no_k[] = { { 0x200000, 0x7E00000, 1 } };
    blk_begin();
    blk_mmap(no_k, 1);
    blk_end();
    expect(parse() == NULL && reserve(&t) != NULL, "kernel image outside usable RAM rejected");
}

/* ---- U1: loader objects below 1 MiB (GRUB x86_64-efi under OVMF) ---- */

static const char *parse_at(uint64_t phys)
{
    return mb2_parse(blk, phys, &bi);
}

/* An info block at `info` with one module [s, e) over `map`, then reserve with the standard kernel. */
static const char *low_case(const struct me *map, int n, uint64_t info, uint32_t s, uint32_t e,
                            struct resv_table *t)
{
    const char *err;

    blk_begin();
    blk_mmap(map, n);
    if (s < e)
        blk_module(s, e, "m");
    blk_end();
    if ((err = parse_at(info)))
        return err;
    return reserve(t);
}

static int says(const char *got, const char *want)
{
    if (got && streq(got, want))
        return 1;
    serial_puts("selftest:   boot_reserve said: ");
    serial_puts(got ? got : "accepted");
    serial_puts("\r\n");
    return 0;
}

static void test_low_placement(void)
{
    /* The OVMF map below 16 MiB as GRUB reports it: page 0 to 640 KiB and 1 MiB up are RAM. */
    static const struct me ovmf[] = { { 0, 0xA0000, 1 }, { 0x100000, 0x7F00000, 1 } };
    /* A wrong map that calls the whole first MiB RAM. */
    static const struct me liar[] = { { 0, 0x100000, 1 }, { 0x100000, 0x7F00000, 1 } };
    struct resv_table t;
    const char *r;

    r = low_case(ovmf, 2, 0x68000, 0x64000, 0x67348, &t);
    expect(r == NULL && resv_overlaps(&t, 0x64000, 0x67348) && resv_overlaps(&t, 0x68000, 0x68000 + blk_len) &&
           bi.module_count == 1 && bi.info_start == 0x68000 && bi.modules[0].start == 0x64000,
           "OVMF layout (module 0x64000-0x67348, info 0x68000) accepted; both reserved before the PMM");
    expect(low_case(ovmf, 2, 0x68000, 0x68000 + 0x400, 0x69000, &t) == NULL && blk_len < 0x400,
           "a module sharing the info block's page without overlapping its bytes is accepted");
    expect(says(low_case(ovmf, 2, 0x68000, 0x68000, 0x69000, &t), "module overlaps the multiboot2 info"),
           "a low module overlapping the info block's bytes is rejected");
    expect(says(low_case(ovmf, 2, 0x68000, 0x800, 0x1000, &t), "module in physical page 0"),
           "a module inside physical page 0 is rejected");
    expect(says(low_case(ovmf, 2, 0x68000, 0xF00, 0x2000, &t), "module in physical page 0"),
           "a module reaching into physical page 0 is rejected");
    expect(says(low_case(ovmf, 2, 0x800, 0, 0, &t), "multiboot2 info in physical page 0"),
           "an info block in physical page 0 is rejected");
    expect(says(low_case(liar, 2, 0x68000, 0xB8000, 0xB9000, &t), "module in the legacy hole (0xA0000-1 MiB)"),
           "a module in the legacy hole is rejected even when the map calls it RAM");
    expect(says(low_case(ovmf, 2, 0x68000, 0x9F000, 0xA1000, &t), "module in the legacy hole (0xA0000-1 MiB)"),
           "a module running into the legacy hole is rejected");
    expect(says(low_case(liar, 2, 0xC0000, 0, 0, &t), "multiboot2 info in the legacy hole (0xA0000-1 MiB)"),
           "an info block in the legacy hole is rejected");
    /* std_map: RAM to 0x9FC00, the EBDA 0x9FC00-0xA0000 reserved. Page rounding reaches it. */
    expect(says(low_case(std_map, N_STD, 0x68000, 0x9F000, 0x9F400, &t), "module not in usable RAM"),
           "a module whose page reaches the reserved EBDA is rejected");
    expect(says(low_case(std_map, N_STD, 0x9FB00, 0, 0, &t), "multiboot2 info not in usable RAM"),
           "an info block whose page reaches the reserved EBDA is rejected");
    expect(says(low_case(std_map, N_STD, K_END - 0x100, 0, 0, &t),
                "multiboot2 info shares a page with the kernel image"),
           "an info block sharing the kernel's last page is rejected");
    expect(says(low_case(ovmf, 2, 0x68000, 0x10D000, 0x10D800, &t), "module overlaps the kernel image"),
           "a module inside the kernel's pages is rejected");

    /* Two modules sharing one low page, byte-disjoint: accepted; overlapping by one byte: rejected. */
    blk_begin();
    blk_mmap(ovmf, 2);
    blk_module(0x50000, 0x50800, "a");
    blk_module(0x50800, 0x51000, "b");
    blk_end();
    expect(parse_at(0x68000) == NULL && reserve(&t) == NULL && resv_overlaps(&t, 0x50000, 0x51000),
           "two low modules sharing a page without overlapping bytes are accepted");
    blk_begin();
    blk_mmap(ovmf, 2);
    blk_module(0x50000, 0x50801, "a");
    blk_module(0x50800, 0x51000, "b");
    blk_end();
    expect(parse_at(0x68000) == NULL && says(reserve(&t), "modules overlap"), "two modules overlapping by one byte are rejected");

    /* Bounds and overflow: the 1 GiB limit, and an info block whose end would pass it. */
    static const struct me big[] = { { 0x100000, 0x7FF00000, 1 } };
    blk_begin();
    blk_mmap(big, 1);
    blk_module(0x3FFFF000, 0x40000000, "top");
    blk_end();
    expect(parse_at(0x68000) == NULL && reserve(&t) != NULL, "a module ending exactly at 1 GiB but outside RAM is rejected");
    blk_begin();
    blk_mmap(big, 1);
    blk_module(0x3FFFF000, 0x40000001, "past");
    blk_end();
    expect(parse_at(0x200000) != NULL, "a module ending past 1 GiB is rejected at parse time");
    blk_begin();
    blk_mmap(big, 1);
    blk_end();
    expect(parse_at(0x40000000 - 8) != NULL, "an info block running past 1 GiB is rejected at parse time");

    /* The kernel image itself must start at or above 1 MiB. */
    blk_begin();
    blk_mmap(ovmf, 2);
    blk_end();
    resv_init(&t);
    expect(parse_at(0x68000) == NULL && says(boot_reserve(&bi, 0x80000, 0x90000, &t), "kernel image below 1 MiB"),
           "a kernel image below 1 MiB is rejected");
}

/* ---- U1: framebuffer information ---- */

/* A framebuffer tag; color = NULL leaves out the RGB channel bytes; size 0 = natural size. */
static void blk_fb(uint64_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp, uint8_t type,
                   const uint8_t *color, uint32_t size)
{
    uint32_t n = size ? size : 32 + (color ? 6 : 0);
    uint32_t *t = blk_tag(8, n);
    uint8_t *b = (uint8_t *)t;

    if (n < 32)
        return;
    *(uint64_t *)(b + 8) = addr;
    t[4] = pitch;
    t[5] = w;
    t[6] = h;
    b[28] = bpp;
    b[29] = type;
    if (color && n >= 38)
        memcpy(b + 32, color, 6);
}

static const uint8_t bgrx[6] = { 16, 8, 8, 8, 0, 8 }, rgbx[6] = { 0, 8, 8, 8, 16, 8 };

static const char *fb_case(uint64_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp, uint8_t type,
                           const uint8_t *color, uint32_t size, struct boot_fb *fb)
{
    const char *err;

    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_fb(addr, pitch, w, h, bpp, type, color, size);
    blk_end();
    if ((err = parse()))
        return err;
    return boot_framebuffer(&bi, fb);
}

static void test_framebuffer(void)
{
    struct boot_fb fb;

    expect(fb_case(0x80000000, 5120, 1280, 800, 32, 1, bgrx, 0, &fb) == NULL && fb.kind == BOOT_FB_BGRX32 &&
           fb.addr == 0x80000000 && fb.size == 5120ULL * 800 && fb.width == 1280 && fb.height == 800,
           "framebuffer: OVMF GOP 1280x800 BGRX at 0x80000000 (as measured) accepted");
    expect(fb_case(0xFD000000, 4096, 1024, 768, 32, 1, bgrx, 0, &fb) == NULL && fb.size == 4096ULL * 768,
           "framebuffer: SeaBIOS VBE 1024x768 at 0xFD000000 accepted");
    expect(fb_case(0x4000000000ULL, 15360, 3840, 2160, 32, 1, bgrx, 0, &fb) == NULL,
           "framebuffer: 3840x2160 above 4 GiB fits the 32 MiB limit");
    expect(fb_case(0xB8000, 160, 80, 25, 16, 2, NULL, 0, &fb) == NULL && fb.kind == BOOT_FB_EGA_TEXT,
           "framebuffer: 80x25 EGA text at 0xB8000 is recognized as text mode");
    expect(fb_case(0xA0000, 160, 80, 25, 16, 2, NULL, 0, &fb) != NULL && fb.kind == BOOT_FB_NONE &&
           fb_case(0xB8000, 264, 132, 25, 16, 2, NULL, 0, &fb) != NULL,
           "framebuffer: EGA text elsewhere or in another size is refused");

    /* Missing, duplicate and malformed tags. */
    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_end();
    expect(parse() == NULL && streq(boot_framebuffer(&bi, &fb), "no framebuffer information") &&
           fb.kind == BOOT_FB_NONE, "framebuffer: none given is reported, not an error of the boot");
    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_fb(0x80000000, 5120, 1280, 800, 32, 1, bgrx, 0);
    blk_fb(0x90000000, 5120, 1280, 800, 32, 1, bgrx, 0);
    blk_end();
    expect(parse() == NULL && streq(boot_framebuffer(&bi, &fb), "more than one framebuffer tag"),
           "framebuffer: two tags are refused");
    expect(streq(fb_case(0x80000000, 5120, 1280, 800, 32, 1, bgrx, 30, &fb), "framebuffer tag truncated") &&
           streq(fb_case(0x80000000, 5120, 1280, 800, 32, 1, NULL, 0, &fb),
                 "RGB framebuffer tag without its channel layout"),
           "framebuffer: a truncated tag or an RGB tag without channel bytes is refused");
    expect(fb_case(0x80000000, 1280, 1280, 800, 8, 0, NULL, 0, &fb) != NULL, "framebuffer: indexed colour is refused");
    expect(fb_case(0x80000000, 3840, 1280, 800, 24, 1, bgrx, 0, &fb) != NULL &&
           fb_case(0x80000000, 2560, 1280, 800, 16, 1, bgrx, 0, &fb) != NULL,
           "framebuffer: 24 and 16 bits per pixel are refused");
    expect(streq(fb_case(0x80000000, 5120, 1280, 800, 32, 1, rgbx, 0, &fb),
                 "pixel layout is not BGRX (red 16, green 8, blue 0, 8 bits each)"),
           "framebuffer: RGBX (an unverified layout) is refused");
    expect(fb_case(0x80000000, 5120, 0, 800, 32, 1, bgrx, 0, &fb) != NULL &&
           fb_case(0x80000000, 5120, 1280, 0, 32, 1, bgrx, 0, &fb) != NULL &&
           fb_case(0x80000000, 36000, 9000, 800, 32, 1, bgrx, 0, &fb) != NULL &&
           fb_case(0x80000000, 1200, 300, 800, 32, 1, bgrx, 0, &fb) != NULL,
           "framebuffer: zero, too large or too small dimensions are refused");
    expect(fb_case(0x80000000, 5116, 1280, 800, 32, 1, bgrx, 0, &fb) != NULL &&
           fb_case(0x80000000, 5122, 1280, 800, 32, 1, bgrx, 0, &fb) != NULL &&
           fb_case(0x80000000, 0xFFFFFFFC, 1280, 800, 32, 1, bgrx, 0, &fb) != NULL,
           "framebuffer: a pitch below width x 4, unaligned or huge is refused");
    expect(streq(fb_case(0x80000000, 16384, 4096, 4096, 32, 1, bgrx, 0, &fb), "framebuffer larger than 32 MiB") &&
           fb_case(0x80000000, 65536, 8192, 8192, 32, 1, bgrx, 0, &fb) != NULL,
           "framebuffer: more than 32 MiB is refused (pitch x height computed in 64 bits)");
    expect(fb_case(0, 5120, 1280, 800, 32, 1, bgrx, 0, &fb) != NULL &&
           fb_case(0xFFFFFFFFFFFFF000ULL, 5120, 1280, 800, 32, 1, bgrx, 0, &fb) != NULL &&
           fb_case(0x80000002, 5120, 1280, 800, 32, 1, bgrx, 0, &fb) != NULL,
           "framebuffer: a zero, wrapping or unaligned address is refused");

    /* The firmware kind: an EFI system-table tag marks a UEFI boot. */
    blk_begin();
    blk_mmap(std_map, N_STD);
    uint32_t *t = blk_tag(12, 16);
    *(uint64_t *)&t[2] = 0x3f9ec018;
    blk_end();
    int efi = parse() == NULL && bi.efi;
    blk_begin();
    blk_mmap(std_map, N_STD);
    blk_end();
    expect(efi && parse() == NULL && !bi.efi, "an EFI system-table tag marks a UEFI boot; its absence a BIOS boot");
}

static void test_table(void)
{
    struct resv_table t;
    struct phys_range copy[RESV_MAX];

    resv_init(&t);
    for (unsigned i = 0; i < RESV_MAX; i++)
        expect(resv_add(&t, 0x1000000 + 0x10000 * i, 0x1000000 + 0x10000 * i + 1, "x") == RESV_OK,
               "fill the table");
    memcpy(copy, t.r, sizeof(copy));
    expect(resv_add(&t, 0x2000000, 0x2001000, "overflow") == RESV_FULL, "17th entry refused");
    expect(t.count == RESV_MAX && memcmp(copy, t.r, sizeof(copy)) == 0,
           "full table is left unchanged");

    resv_init(&t);
    expect(resv_add(&t, 0x1800, 0x2800, "u") == RESV_OK && t.r[0].start == 0x1000 &&
           t.r[0].end == 0x3000, "unaligned range rounds outward");
    expect(resv_add(&t, 5, 5, "e") == RESV_BAD_RANGE, "empty range refused");
    expect(resv_add(&t, 9, 5, "r") == RESV_BAD_RANGE, "reversed range refused");
    expect(resv_add(&t, 0x1000, UINT64_MAX, "o") == RESV_BAD_RANGE, "rounding overflow refused");
    expect(t.count == 1, "refused ranges are not recorded");
    expect(resv_add(&t, 0x2000, 0x4000, "ov") == RESV_OK && resv_overlaps(&t, 0x3000, 0x3001) &&
           !resv_overlaps(&t, 0x4000, 0x5000), "overlapping entries form a union");
    expect(!resv_overlaps(&t, 0x3000, 0x3000), "empty query never overlaps");
}

int selftest_bootinfo(void)
{
    failures = cases = 0;
    test_parse();
    test_conflicts();
    test_low_placement();
    test_framebuffer();
    test_table();
    serial_puts("selftest: bootinfo ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}

/* Expected state when GRUB boots tests/x86_64/modules.grub.cfg. */
static const char fixture[] = "VOS-N1.2-TEST-MODULE\n";

int selftest_modules(const struct boot_info *b, const struct resv_table *t)
{
    failures = cases = 0;
    expect(b->module_count == 2, "GRUB delivered two modules");
    expect(contains(b->cmdline, "vos.test=1") && !contains(b->cmdline, "mod.arg"),
           "kernel command line carries only the kernel argument");
    for (uint32_t i = 0; i < b->module_count && i < 2; i++) {
        const struct boot_module *m = &b->modules[i];

        expect(contains(m->cmdline, i == 0 ? "mod.arg=first" : "mod.arg=second") &&
               !contains(m->cmdline, "vos.test"), "module command line is its own");
        expect(m->end - m->start == sizeof(fixture) - 1, "module length equals the fixture");
        expect(m->start % PAGE_SIZE == 0, "module is page-aligned (alignment tag honoured)");
        expect(m->start >= 0x100000 && m->end <= BOOT_MAP_LIMIT, "module in the supported range");
        expect(resv_overlaps(t, m->start, m->end) && resv_overlaps(t, m->end - 1, m->end) &&
               resv_overlaps(t, m->start, m->start + 1), "module range fully reserved");
        expect(memcmp(phys_to_virt(m->start), fixture, sizeof(fixture) - 1) == 0,
               "module contents match the fixture");
    }
    expect(resv_overlaps(t, 0, 0x100000), "first 1 MiB reserved");
    expect(resv_overlaps(t, b->info_start, b->info_end), "info block still reserved");
    serial_puts("selftest: modules ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
