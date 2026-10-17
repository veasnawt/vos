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

static uint8_t blk[1024] __attribute__((aligned(8)));
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

static void blk_mmap(const struct me *e, int n)
{
    uint32_t *t = blk_tag(6, 16 + 24 * n);

    t[2] = 24;
    t[3] = 0;
    for (int i = 0; i < n; i++) {
        uint64_t *p = (uint64_t *)&t[4 + 6 * i];
        p[0] = e[i].base;
        p[1] = e[i].len;
        ((uint32_t *)p)[4] = e[i].type;
    }
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
    blk_tag(3, 4000);                    /* tag larger than the whole block */
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
    expect(module_case(0x80000, 0x81000, &t) != NULL, "module below 1 MiB rejected");
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
