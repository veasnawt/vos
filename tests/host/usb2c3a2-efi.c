/*
 * USB.2-C3a.2 hosted test: the real mb2_parse/boot_reserve, efi_memory.c,
 * resv.c and pmm.c C code against the bare-metal Rust archive, on Multiboot2
 * blocks built from the captured OVMF/SeaBIOS tags and adversarial variants.
 * Expectations come from an independent per-frame model written here.
 * Built with -fsanitize=address,undefined (the Rust code is not instrumented).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../arch/x86_64/bootinfo.c"
#include "../../arch/x86_64/resv.c"
#include "../../arch/x86_64/pmm.c"
#include "../../arch/x86_64/efi_memory.c"

__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }

#define INFO_PHYS   0x300000ULL
#define KSTART      0x100000ULL
#define KEND        0x1da000ULL
#define NF          PMM_MAX_FRAMES
#define WB          (1ULL << 3)

static unsigned checks, group;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } checks++; group++; } while (0)

static uint8_t ovmf6[1024], ovmf17[8192], seabios6[1024];
static uint32_t ovmf6_n, ovmf17_n, seabios6_n;

static uint32_t slurp(const char *dir, const char *name, uint8_t *buf, size_t cap)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    size_t n = fread(buf, 1, cap, f);
    CHECK(feof(f) && n > 16 && n < cap);
    fclose(f);
    return (uint32_t)n;
}

/* ---- Multiboot2 block builder ---- */
static uint8_t blob[65536] __attribute__((aligned(8)));
static uint32_t blen;
static void begin(void) { memset(blob, 0, sizeof blob); blen = 8; }
static void raw(const void *tag, uint32_t size)
{
    CHECK(blen + size + 8 < sizeof blob);
    memcpy(blob + blen, tag, size);
    blen += (size + 7) & ~7u;
}
static void tag(uint32_t type, const void *body, uint32_t n)
{
    uint8_t t[4096];
    CHECK(n + 8 <= sizeof t);
    uint32_t size = 8 + n;
    memcpy(t, &type, 4);
    memcpy(t + 4, &size, 4);
    if (n)
        memcpy(t + 8, body, n);
    raw(t, size);
}
static void systab(void) { uint64_t p = 0x7f9e000; tag(MB2_TAG_EFI64_SYSTAB, &p, 8); }
static void finish(void) { tag(MB2_TAG_END, NULL, 0); memcpy(blob, &blen, 4); }

struct desc { uint32_t type; uint64_t base, pages, attr; };
static uint32_t efi_tag(uint8_t *out, uint32_t ds, uint32_t ver, const struct desc *d, unsigned n)
{
    uint32_t type = 17, size = 16 + n * ds;
    memset(out, 0, size);
    memcpy(out, &type, 4); memcpy(out + 4, &size, 4); memcpy(out + 8, &ds, 4); memcpy(out + 12, &ver, 4);
    for (unsigned i = 0; i < n; i++) {
        uint8_t *e = out + 16 + i * ds;
        memcpy(e, &d[i].type, 4); memcpy(e + 8, &d[i].base, 8); memcpy(e + 24, &d[i].pages, 8); memcpy(e + 32, &d[i].attr, 8);
    }
    return size;
}
struct ent { uint64_t base, len; uint32_t type; };
static uint32_t mmap_tag(uint8_t *out, const struct ent *e, unsigned n)
{
    uint32_t type = 6, size = 16 + 24 * n, es = 24, ver = 0;
    memset(out, 0, size);
    memcpy(out, &type, 4); memcpy(out + 4, &size, 4); memcpy(out + 8, &es, 4); memcpy(out + 12, &ver, 4);
    for (unsigned i = 0; i < n; i++) {
        uint8_t *p = out + 16 + 24 * i;
        memcpy(p, &e[i].base, 8); memcpy(p + 8, &e[i].len, 8); memcpy(p + 16, &e[i].type, 4);
    }
    return size;
}

/* ---- Independent model ---- */
static uint8_t ram6[NF], ok[NF], bad[NF];
static uint64_t rd(const uint8_t *b, unsigned at, unsigned n) { uint64_t v = 0; while (n--) v = v << 8 | b[at + n]; return v; }
static void model(const uint8_t *t6, const uint8_t *t17)
{
    memset(ram6, 0, NF); memset(ok, 0, NF); memset(bad, 0, NF);
    uint32_t size = (uint32_t)rd(t6, 4, 4), es = (uint32_t)rd(t6, 8, 4);
    for (uint32_t at = 16; at + es <= size; at += es) {
        uint64_t base = rd(t6, at, 8), len = rd(t6, at + 8, 8);
        if (rd(t6, at + 16, 4) != 1 || !len || base + len < base)
            continue;
        uint64_t s = ((base < PMM_LOW_LIMIT ? PMM_LOW_LIMIT : base) + 4095) / 4096;
        uint64_t e = (base + len > PMM_LIMIT ? PMM_LIMIT : base + len) / 4096;
        for (uint64_t f = s; f < e; f++)
            ram6[f] = 1;
    }
    if (!t17)
        return;
    uint32_t s17 = (uint32_t)rd(t17, 4, 4), ds = (uint32_t)rd(t17, 8, 4);
    for (uint32_t at = 16; at + ds <= s17; at += ds) {
        uint64_t type = rd(t17, at, 4), base = rd(t17, at + 8, 8), pages = rd(t17, at + 24, 8), attr = rd(t17, at + 32, 8);
        int allowed = (type == 1 || type == 2 || type == 7) && (attr & WB) && !(attr & ((1ULL << 63) | (1ULL << 18)));
        for (uint64_t f = base / 4096; f < base / 4096 + pages && f < NF; f++)
            (allowed ? ok : bad)[f] = 1;
    }
}
static int must_exclude(uint64_t f) { return ram6[f] && (bad[f] || !ok[f]); }

/* ---- PMM instances ---- */
static uint64_t av[PMM_WORDS(NF)], al[PMM_WORDS(NF)], rav[PMM_WORDS(NF)], ral[PMM_WORDS(NF)];
static struct pmm p, ref;
static struct resv_table t, rt;
static struct boot_info bi;
static struct efi_memory_report r;
static int bit(const uint64_t *b, uint64_t f) { return (int)(b[f / 64] >> (f % 64) & 1); }

/* Parse the current blob, reserve as the kernel does, classify, and build both
 * the candidate PMM and a reference PMM without EFI exclusion. */
static int prepare(void)
{
    resv_init(&t); resv_init(&rt);
    CHECK(mb2_parse(blob, INFO_PHYS, &bi) == NULL);
    CHECK(boot_reserve(&bi, KSTART, KEND, &t) == NULL);
    CHECK(boot_reserve(&bi, KSTART, KEND, &rt) == NULL);
    size_t before = t.count;
    memset(&r, 0xa5, sizeof r);
    int status = efi_memory_exclude(&bi, &t, &r);
    CHECK(status == r.status);
    if (status != EFI_MEMORY_APPLIED)
        CHECK(t.count == before);       /* nothing entered unless applied */
    for (size_t i = before; i < t.count; i++)
        CHECK(!strcmp(t.r[i].what, EFI_MEMORY_WHAT) && t.r[i].start % 4096 == 0 && t.r[i].end % 4096 == 0);
    CHECK(pmm_init_in(&p, av, al, NF, &bi, &t) == PMM_OK);
    CHECK(pmm_init_in(&ref, rav, ral, NF, &bi, &rt) == PMM_OK);
    return status;
}

static void begin_group(void) { group = 0; }
static void end_group(const char *what) { printf("  %s: %u checks PASS\n", what, group); }

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    ovmf6_n = slurp(argv[1], "ovmf-tag6.bin", ovmf6, sizeof ovmf6);
    ovmf17_n = slurp(argv[1], "ovmf-tag17.bin", ovmf17, sizeof ovmf17);
    seabios6_n = slurp(argv[1], "seabios-tag6.bin", seabios6, sizeof seabios6);
    static const uint64_t firmware[] = { 0x674a000, 0x674b000, 0x674c000, 0x674d000, 0x6744000, 0x6745000 };

    begin_group();
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(ovmf17, ovmf17_n); finish();
    CHECK(prepare() == EFI_MEMORY_APPLIED && r.error == 0 && r.descriptors == 121 && !r.coalesced);
    model(ovmf6, ovmf17);
    uint64_t want = 0, diffs = 0;
    for (uint64_t f = 0; f < NF; f++) {
        want += must_exclude(f);
        if (bit(av, f) != (bit(rav, f) && !must_exclude(f)))
            diffs++;
    }
    CHECK(want == 9765 && r.frames == want && diffs == 0);
    CHECK(p.usable == ref.usable - want);
    for (unsigned i = 0; i < 6; i++)
        CHECK(!bit(av, firmware[i] / 4096) && bit(rav, firmware[i] / 4096));
    end_group("OVMF map: boot-services frames excluded before the first allocation, exact vs model");

    begin_group();
    uint64_t pa, n = 0, hit = 0;
    while (pmm_alloc_in(&p, &pa) == PMM_OK) {
        n++;
        hit += must_exclude(pa / 4096);
    }
    CHECK(n == p.usable && hit == 0 && p.free == 0);
    for (unsigned i = 0; i < 6; i++)
        CHECK(pmm_reserve_in(&p, firmware[i]) == PMM_EPROTECTED && pmm_frame_state_in(&p, firmware[i]) == PMM_FRAME_UNAVAILABLE);
    CHECK(pmm_check_in(&p) == PMM_OK);
    n = hit = 0;
    while (pmm_alloc_in(&ref, &pa) == PMM_OK)
        hit += must_exclude(pa / 4096);
    CHECK(hit == want);                 /* negative control: without exclusion every one is handed out */
    end_group("allocate until exhausted: no excluded frame; reserving one is refused (reference hands all out)");

    begin_group();
    begin(); raw(seabios6, seabios6_n); finish();
    CHECK(prepare() == EFI_MEMORY_NOT_EFI && t.count == rt.count);
    CHECK(!memcmp(av, rav, sizeof av) && p.usable == ref.usable);
    end_group("SeaBIOS map: not EFI, PMM identical to the e820-only reference");

    begin_group();
    begin(); raw(ovmf6, ovmf6_n); systab(); finish();
    CHECK(prepare() == EFI_MEMORY_REFUSED && r.error == 1);
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(ovmf17, ovmf17_n); tag(MB2_TAG_EFI_BS, NULL, 0); finish();
    CHECK(prepare() == EFI_MEMORY_REFUSED && r.error == 2);
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(ovmf17, ovmf17_n); raw(ovmf17, ovmf17_n); finish();
    CHECK(prepare() == EFI_MEMORY_REFUSED && r.error == 3);
    end_group("UEFI without a map, boot services active, duplicate map: refused, nothing entered");

    begin_group();
    uint8_t bad17[8192];
    memcpy(bad17, ovmf17, ovmf17_n);
    bad17[12] = 2;                                      /* version */
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(bad17, ovmf17_n); finish();
    CHECK(prepare() == EFI_MEMORY_REFUSED && r.error == 6);
    memcpy(bad17, ovmf17, ovmf17_n);
    bad17[8] = 44;                                      /* descriptor size */
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(bad17, ovmf17_n); finish();
    CHECK(prepare() == EFI_MEMORY_REFUSED && r.error == 5);
    memcpy(bad17, ovmf17, ovmf17_n);
    bad17[16 + 48 * 7 + 9] |= 0x08;                     /* descriptor 7 base + 0x800 */
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(bad17, ovmf17_n); finish();
    CHECK(prepare() == EFI_MEMORY_REFUSED && r.error == 8);
    memcpy(bad17, ovmf17, ovmf17_n);
    memset(bad17 + 16 + 48 * 9 + 24, 0xff, 8);          /* descriptor 9 pages overflow */
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(bad17, ovmf17_n); finish();
    CHECK(prepare() == EFI_MEMORY_REFUSED && r.error == 9);
    struct desc none[1] = { { 7, 0x100000, 1, WB } };
    uint32_t sz = efi_tag(bad17, 48, 1, none, 0);       /* no descriptors */
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(bad17, sz); finish();
    CHECK(prepare() == EFI_MEMORY_REFUSED && r.error == 7);
    end_group("malformed EFI maps (version, size, alignment, overflow, empty): refused, nothing entered");

    begin_group();
    begin(); raw(ovmf6, ovmf6_n); systab(); raw(ovmf17, ovmf17_n); finish();
    resv_init(&t); resv_init(&rt);
    CHECK(mb2_parse(blob, INFO_PHYS, &bi) == NULL && boot_reserve(&bi, KSTART, KEND, &t) == NULL &&
          boot_reserve(&bi, KSTART, KEND, &rt) == NULL);
    while (t.count < RESV_MAX - 1)
        CHECK(resv_add(&t, 0x1000, 0x2000, "filler") == RESV_OK);
    CHECK(efi_memory_exclude(&bi, &t, &r) == EFI_MEMORY_APPLIED && r.ranges == 1 && r.coalesced == 1 && t.count == RESV_MAX);
    CHECK(pmm_init_in(&p, av, al, NF, &bi, &t) == PMM_OK && pmm_init_in(&ref, rav, ral, NF, &bi, &rt) == PMM_OK);
    model(ovmf6, ovmf17);
    diffs = 0;
    for (uint64_t f = 0; f < NF; f++)
        if (bit(av, f) && (!bit(rav, f) || must_exclude(f)))
            diffs++;
    CHECK(diffs == 0 && p.usable < ref.usable - want);
    CHECK(efi_memory_exclude(&bi, &t, &r) == EFI_MEMORY_REFUSED && r.error == 12);
    end_group("one free table slot: one widened range (superset); full table: refused");

    begin_group();
    uint8_t t6[1024], t17[1024];
    struct ent ents[] = { { 0, 0x9f000, 1 }, { 0x100000, 0x200000, 1 }, { 0x300000, 0x100000, 1 }, { 0x400000, 0x100000, 2 } };
    struct desc conv[] = { { 7, 0, 4096, WB } };        /* claims 16 MiB, over reserved memory and beyond RAM */
    uint32_t n6 = mmap_tag(t6, ents, 4), n17 = efi_tag(t17, 48, 1, conv, 1);
    begin(); raw(t6, n6); systab(); raw(t17, n17); finish();
    CHECK(prepare() == EFI_MEMORY_APPLIED && r.ranges == 0 && r.frames == 0);
    CHECK(!memcmp(av, rav, sizeof av));
    begin(); raw(t6, n6); raw(t17, n17); finish();     /* an EFI map without a systab tag is honoured */
    CHECK(prepare() == EFI_MEMORY_APPLIED);
    struct desc mixed[] = { { 7, 0, 512, WB }, { 4, 0x200000, 256, WB }, { 7, 0x300000, 256, WB | (1ULL << 63) } };
    n17 = efi_tag(t17, 48, 1, mixed, 3);
    begin(); raw(t6, n6); systab(); raw(t17, n17); finish();
    CHECK(prepare() == EFI_MEMORY_APPLIED && r.ranges == 1 && r.frames == 512);
    model(t6, t17);
    for (uint64_t f = 0; f < NF; f++)
        CHECK(bit(av, f) == (bit(rav, f) && !must_exclude(f)));
    end_group("EFI map can only remove: conventional claims add nothing; BS + runtime RAM removed exactly");

    begin_group();
    struct efi_memory_input in = { 0 };
    struct efi_memory_report rep;
    CHECK(vos_efi_exclusions_v1(&in, add, &t, NULL) == -2);
    CHECK(vos_efi_exclusions_v1(&in, add, &t, (struct efi_memory_report *)((char *)&rep + 4)) == -2);
    CHECK(vos_efi_exclusions_v1(NULL, add, &t, &rep) == -1 && rep.error == 4);
    CHECK(vos_efi_exclusions_v1(&in, NULL, &t, &rep) == -1 && rep.error == 13);
    in.efi_firmware = 1;
    CHECK(vos_efi_exclusions_v1(&in, add, &t, &rep) == -1 && rep.error == 1);
    in.efi_firmware = 0;
    CHECK(vos_efi_exclusions_v1(&in, add, &t, &rep) == 1 && rep.error == 0);
    CHECK(!strcmp(efi_memory_strerror(1), "UEFI without an EFI memory map") && !strcmp(efi_memory_strerror(99), "unknown"));
    end_group("direct ABI: null/misaligned report, null input/sink, policy codes");

    printf("PASS: %u checks; real bootinfo/resv/pmm/efi_memory C with the bare-metal Rust archive\n", checks);
    return 0;
}
