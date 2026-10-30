/* USB input bring-up, Increment 2: hosted tests of the read-only firmware-table lease.
 *
 * The real PMM, the real devmap.c (path overridable with -DDEVMAP_C for the mutation controls) and the real survey adapter
 * xhci_survey.c run against a modelled page-table RAM, a modelled firmware memory map and a CPU that reports 40 physical
 * address bits. Nothing here can write PCI configuration (any such call aborts) and no lease is ever dereferenced: the
 * hosted harness cannot map kernel virtual addresses, so the byte-copy through a lease is NOT exercised here (the QEMU
 * native cases with -m 2048 do that); everything up to the dereference, and the direct-map copy below 1 GiB, is.
 *
 * What is asserted about the lease (DEVMAP_SCOPE_FIRMWARE_TABLE):
 *   - accepted only where every page is reserved, ACPI-reclaim or ACPI-NVS, at or above 1 GiB, page aligned, 1..4 pages,
 *     below MAXPHYADDR, and overlapping no mapping; each rejection returns its own code and leaves nothing behind;
 *   - every leaf is present, NOT writable, NX, supervisor-only, uncached, and equals devmap_expected_pte exactly, which is
 *     what devmap_audit and the VMM audit (arch/x86_64/vmm.c, device-leaf rule, replicated below) compare against;
 *   - a leaf made writable behind the registry's back is detected by both audits and release refuses until it is repaired;
 *   - release restores the registry, the page tables and the frame count exactly. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VOS_X86_64_CPU_H
#define VOS_X86_64_MEMLAYOUT_H
#define PAGE_SIZE 4096ULL
#define BOOT_MAP_LIMIT 0x40000000ULL
#define KERNEL_VMA 0xffffffff80000000ULL
#define RFLAGS_IF (1ULL << 9)
static unsigned char ram[2*1024*1024] __attribute__((aligned(4096)));
static uint64_t cr3, flags, pat = 0x0007040600070406ULL, invalidations;
static int in_irq;
static void *phys_to_virt(uint64_t pa) { assert(pa < sizeof(ram)); return ram + pa; }
static uint64_t read_cr3(void) { return cr3; }
static uint64_t read_rflags(void) { return flags; }
static void invlpg(uint64_t va) { (void)va; invalidations++; }
static uint64_t rdmsr(unsigned msr) { (void)msr; return pat; }
static void wrmsr(unsigned msr, uint64_t v) { (void)msr; pat = v; }
static void cpuid(unsigned a, unsigned c, uint32_t *x, uint32_t *b, uint32_t *y, uint32_t *d)
{ (void)c; *x = a == 0x80000000 ? 0x80000008 : a == 0x80000008 ? 40 : 0; *b = *y = 0; *d = 1u << 16; }
static void cpu_halt(void) { abort(); }
static uint8_t inb(uint16_t port) { (void)port; abort(); }
static unsigned guard_page_hits;
static uint64_t guard_page;
int kmap_is_guard(uint64_t pa) { if (pa == guard_page) guard_page_hits++; return pa == guard_page; }
#ifndef DEVMAP_C
#define DEVMAP_C "../../arch/x86_64/devmap.c"
#endif
#include "../../arch/x86_64/pmm.c"
#include DEVMAP_C
#include "../../arch/x86_64/xhci_survey.c"

static uint64_t root;
uint64_t vmm_kernel_root(void) { return root; }
int irq_in_handler(void) { return in_irq; }
const struct mb2_mmap_entry *boot_mmap_entry(const struct boot_info *bi, uint32_t i)
{ return (const void *)((const uint8_t *)bi->mmap + sizeof(*bi->mmap) + i * bi->mmap->entry_size); }
__attribute__((noreturn)) void panic(const char *s) { fprintf(stderr, "unexpected panic: %s\n", s); abort(); }
void serial_puts(const char *s) { (void)s; }
void serial_hex(uint64_t v) { (void)v; }
void serial_dec(uint64_t v) { (void)v; }
void serial_putc(char c) { (void)c; }
void screen_restart_log(void) { }
int32_t vos_xhci_survey_v2(uint32_t bits, const uint8_t *info, uint32_t len) { (void)bits; (void)info; (void)len; return 0; }
uint32_t pci_read32(struct pci_addr a, unsigned off) { (void)a; (void)off; return 0xffffffffu; }
uint16_t pci_read16(struct pci_addr a, unsigned off) { (void)a; (void)off; return 0xffffu; }
uint8_t pci_read8(struct pci_addr a, unsigned off) { (void)a; (void)off; return 0xffu; }
void pci_write8(struct pci_addr a, unsigned off, uint8_t v) { (void)a; (void)off; (void)v; abort(); }
void pci_write16(struct pci_addr a, unsigned off, uint16_t v) { (void)a; (void)off; (void)v; abort(); }
void pci_write32(struct pci_addr a, unsigned off, uint32_t v) { (void)a; (void)off; (void)v; abort(); }

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

#define GIB 0x40000000ULL
#define A_PA   0x100000000ULL   /* ACPI reclaim, 64 KiB */
#define B_PA   0x100010000ULL   /* ACPI NVS, 16 KiB */
#define C_PA   0x100020000ULL   /* reserved, 16 KiB */
#define D_PA   0x100100000ULL   /* RAM, 1 MiB */
#define E_PA   0x100200000ULL   /* reserved 2 pages ... */
#define F_PA   0x100300000ULL   /* type 5, bad memory */
#define G_PA   0x100400000ULL   /* reclaim 1 page directly followed by RAM */
#define H_PA   0x100500000ULL   /* reserved, 2 pages; the second page is also claimed by a type-5 (bad memory) entry */
#define TOP_PA 0x10000000000ULL /* 1 << 40: reserved, beyond MAXPHYADDR (40 bits) */
#define LOW_FW 0x180000ULL      /* reclaim below 1 GiB, inside the modelled RAM array */
#define LOW2   0x3f000000ULL    /* reclaim below 1 GiB */
#define STRADDLE 0x3fff0000ULL  /* reclaim 0x3fff0000-0x40001000: crosses 1 GiB */

static uint64_t free_frames;
static unsigned maps;
static int restored(void)
{
    return !devmap_scope_token() && !devmap_scope_kind() && pmm_free_count() == free_frames && devmap_count() == maps &&
           !devmap_audit();
}

/* The device-leaf rule of vmm_audit_space (arch/x86_64/vmm.c): only memory-type bits beyond the ordinary leaf bits, exactly
 * what the device registry recorded, never user-accessible. Returns the audit's complaint or NULL. */
static const char *vmm_device_leaf(uint64_t va, uint64_t e1)
{
    uint64_t want = devmap_expected_pte(va);
    if (e1 & ~(PTE_P | PTE_W | PTE_U | PTE_A | PTE_D | PTE_G | PTE_NX | PTE_ADDR | PTE_PCD | PTE_PWT | PTE_PAT4K))
        return "device leaf with unsupported bits";
    if (!want || (e1 & ~(PTE_A | PTE_D)) != want)
        return "device leaf differs from the device registry";
    if (e1 & PTE_U)
        return "user-accessible device page";
    return NULL;
}

/* pte_slot is NULL where no page table covers the address; either way nothing is mapped there. */
static int unmapped(uint64_t va) { const uint64_t *e = pte_slot(va, 0); return !e || !*e; }

static int acquire(unsigned kind, uint64_t pa, uint64_t len, uint64_t *token, uint64_t *va)
{
    struct devmap_scope_region r = {kind, pa, len};
    return devmap_scope_acquire_region(&r, DEVMAP_UC, token, va);
}

/* A rejected request: the exact code, no token, and every counter exactly where it was. */
static void rejected(uint64_t pa, uint64_t len, int code, const char *why)
{
    uint64_t token = 7, va = 7, generation = scope.generation;
    int err = acquire(DEVMAP_SCOPE_FIRMWARE_TABLE, pa, len, &token, &va);
    if (err != code) fprintf(stderr, "rejected(%#llx,%#llx): got %d want %d (%s)\n", (unsigned long long)pa, (unsigned long long)len, err, code, why);
    CHECK(err == code);
    CHECK(!token && !va && scope.generation == generation && restored());
}

/* An accepted lease: every property of the mapping, then a clean release. */
static void accepted(uint64_t pa, uint64_t len)
{
    uint64_t token = 0, va = 0, sva = 0, slen = 0, generation = scope.generation;
    CHECK(acquire(DEVMAP_SCOPE_FIRMWARE_TABLE, pa, len, &token, &va) == DEVMAP_OK);
    CHECK(token && va && va % PAGE_SIZE == 0 && devmap_scope_token() == token && scope.generation == generation + 1);
    CHECK(devmap_scope_kind() == DEVMAP_SCOPE_FIRMWARE_TABLE);
    CHECK(devmap_scope_extent(token, &sva, &slen) == DEVMAP_OK && sva == va && slen == len);
    CHECK(devmap_count() == maps + 1 && map[maps].pa == pa && map[maps].end == pa + len && map[maps].va == va && map[maps].ro == 1);
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t e = *pte_slot(va + off, 0);
        CHECK((e & PTE_P) && !(e & PTE_W) && (e & PTE_NX) && !(e & PTE_U));
        CHECK((e & PTE_ADDR) == pa + off);
        CHECK((e & ~PTE_ADDR) == (PTE_P | PTE_NX | uc_bits));            /* nothing else: no W, no U, no G, uncached */
        CHECK(e == devmap_expected_pte(va + off));
        CHECK(vmm_device_leaf(va + off, e) == NULL);
    }
    CHECK(unmapped(va + len));                                           /* the page after the lease stays unmapped */
    CHECK(!devmap_audit());
    uint64_t used = used_bytes;
    /* a writable leaf behind the registry's back: both audits notice, and release refuses until it is repaired */
    uint64_t *slot = pte_slot(va, 0), saved = *slot;
    *slot |= PTE_W;
    CHECK(devmap_audit() != NULL && !strcmp(devmap_audit(), "device window page differs from the registry"));
    CHECK(vmm_device_leaf(va, *slot) != NULL);
    CHECK(devmap_scope_release(token) == DEVMAP_ESTATE && devmap_scope_token() == token && used_bytes == used);
    *slot = saved;
    *slot |= PTE_U;                                                       /* likewise a user-accessible leaf */
    CHECK(devmap_audit() != NULL && vmm_device_leaf(va, *slot) != NULL && devmap_scope_release(token) == DEVMAP_ESTATE);
    *slot = saved;
    *slot &= ~PTE_NX;                                                     /* and an executable one */
    CHECK(devmap_audit() != NULL && devmap_scope_release(token) == DEVMAP_ESTATE);
    *slot = saved;
    CHECK(!devmap_audit() && vmm_device_leaf(va, *slot) == NULL);
    /* the wrong token, the wrong CR3, interrupts on, an IRQ context: release is refused and nothing changes */
    CHECK(devmap_scope_release(token + 1) == DEVMAP_ESTATE && devmap_scope_release(0) == DEVMAP_ESTATE);
    cr3 = root + 4096; CHECK(devmap_scope_release(token) == DEVMAP_ESTATE); cr3 = root;
    flags = RFLAGS_IF; CHECK(devmap_scope_release(token) == DEVMAP_ESTATE); flags = 0;
    in_irq = 1; CHECK(devmap_scope_release(token) == DEVMAP_ESTATE); in_irq = 0;
    CHECK(devmap_scope_token() == token && !devmap_audit());
    /* a second lease while this one is live */
    uint64_t t2 = 9, v2 = 9;
    CHECK(acquire(DEVMAP_SCOPE_FIRMWARE_TABLE, A_PA + 0x8000, PAGE_SIZE, &t2, &v2) == DEVMAP_ESTATE && !t2 && !v2);
    CHECK(devmap_scope_release(token) == DEVMAP_OK);
    CHECK(restored() && used_bytes == used - len);
    for (uint64_t off = 0; off <= len; off += PAGE_SIZE) CHECK(unmapped(va + off));
}

static void direct_map_range_ok(void)
{
    /* devmap_firmware_range_ok on its own: the memory-map rule, with no mapping made */
    CHECK(devmap_firmware_range_ok(A_PA, 1) && devmap_firmware_range_ok(A_PA, 0x10000) && devmap_firmware_range_ok(A_PA + 0xffff, 1));
    CHECK(devmap_firmware_range_ok(A_PA + 0xffff, 2));                    /* the next entry (NVS) follows directly: contiguous firmware memory is fine */
    CHECK(!devmap_firmware_range_ok(A_PA, 0) && !devmap_firmware_range_ok(A_PA, (1ULL << 20) + 1));
    CHECK(!devmap_firmware_range_ok(UINT64_MAX - 0xfff, 0x2000) && !devmap_firmware_range_ok(UINT64_MAX, 1));
    CHECK(devmap_firmware_range_ok(B_PA, 0x4000) && devmap_firmware_range_ok(C_PA, 0x4000));
    CHECK(!devmap_firmware_range_ok(B_PA + 0x3fff, 2));                   /* NVS then unlisted */
    CHECK(!devmap_firmware_range_ok(D_PA, 1) && !devmap_firmware_range_ok(0x100000, 1));                          /* RAM */
    CHECK(!devmap_firmware_range_ok(E_PA, 0x2000) && devmap_firmware_range_ok(E_PA, 0x1000));                    /* RAM claims page 2 */
    CHECK(!devmap_firmware_range_ok(F_PA, 1));                            /* bad memory */
    CHECK(devmap_firmware_range_ok(H_PA, 0x1000));                        /* the page bad memory does not touch stays eligible */
    CHECK(!devmap_firmware_range_ok(H_PA, 0x2000) && !devmap_firmware_range_ok(H_PA + 0x1000, 0x1000) &&
          !devmap_firmware_range_ok(H_PA + 0xfff, 2) && !devmap_firmware_range_ok(H_PA + 0x1fff, 1));    /* bad memory claims page 2 */
    CHECK(!devmap_firmware_range_ok(G_PA, 0x2000) && devmap_firmware_range_ok(G_PA, 0x1000));                    /* last page is RAM */
    CHECK(!devmap_firmware_range_ok(0x7000000000ULL, 1));                 /* unlisted hole */
    CHECK(devmap_firmware_range_ok(TOP_PA, 1));                           /* listed; MAXPHYADDR is the lease's check */
}

static void adapter(void)
{
    /* The survey adapter: every refusal comes before any access; the direct-map copy below 1 GiB is exact. */
    uint8_t buf[200], canary[200];
    memset(buf, 0xa5, sizeof buf); memcpy(canary, buf, sizeof buf);
    survey.active = 0; survey.token = 0;
    CHECK(xhci_survey_phys_read(LOW_FW, buf, 64) == DEVMAP_ESTATE && !memcmp(buf, canary, sizeof buf));       /* no session */
    survey.active = 1;
    CHECK(xhci_survey_phys_read(LOW_FW, NULL, 64) == DEVMAP_EINVAL);
    CHECK(xhci_survey_phys_read(LOW_FW, buf, 0) == DEVMAP_EINVAL);
    CHECK(xhci_survey_phys_read(LOW_FW, buf, DEVMAP_FIRMWARE_MAX_BYTES + 1) == DEVMAP_EINVAL);
    CHECK(xhci_survey_phys_read(UINT64_MAX - 15, buf, 32) == DEVMAP_EINVAL);
    CHECK(xhci_survey_phys_read(0x100000, buf, 16) == DEVMAP_ERAM);                                           /* RAM */
    CHECK(xhci_survey_phys_read(0x300000, buf, 16) == DEVMAP_ERAM);                                           /* unlisted */
    CHECK(xhci_survey_phys_read(D_PA, buf, 16) == DEVMAP_ERAM && xhci_survey_phys_read(F_PA, buf, 16) == DEVMAP_ERAM);
    CHECK(xhci_survey_phys_read(STRADDLE + 0xf000, buf, 0x2000) == DEVMAP_EINVAL);                            /* straddles 1 GiB */
    CHECK(xhci_survey_phys_read(TOP_PA, buf, 16) == DEVMAP_ERANGE);                                           /* lease: beyond MAXPHYADDR */
    CHECK(xhci_survey_phys_read(E_PA + 0x1ff0, buf, 0x20) == DEVMAP_ERAM);                                    /* ends in the RAM page */
    CHECK(xhci_survey_phys_read(H_PA + 0xff0, buf, 0x20) == DEVMAP_ERAM);                                     /* crosses into the bad-memory overlap */
    CHECK(xhci_survey_phys_read(H_PA + 0x1000, buf, 16) == DEVMAP_ERAM);                                      /* wholly in the overlap */
    CHECK(!memcmp(buf, canary, sizeof buf) && restored());
    survey.token = 1; CHECK(xhci_survey_phys_read(LOW_FW, buf, 16) == DEVMAP_ESTATE); survey.token = 0;       /* a BAR lease is live */
    cr3 = root + 4096; CHECK(xhci_survey_phys_read(LOW_FW, buf, 16) == DEVMAP_ESTATE); cr3 = root;
    flags = RFLAGS_IF; CHECK(xhci_survey_phys_read(LOW_FW, buf, 16) == DEVMAP_ESTATE); flags = 0;
    in_irq = 1; CHECK(xhci_survey_phys_read(LOW_FW, buf, 16) == DEVMAP_ESTATE); in_irq = 0;
    CHECK(!memcmp(buf, canary, sizeof buf));
    /* below 1 GiB, through the direct map: exactly len bytes, not one more */
    for (unsigned i = 0; i < 0x100; i++) ram[LOW_FW + i] = (uint8_t)(i ^ 0x5a);
    CHECK(xhci_survey_phys_read(LOW_FW + 3, buf, 100) == DEVMAP_OK);
    for (unsigned i = 0; i < 100; i++) CHECK(buf[i] == (uint8_t)((i + 3) ^ 0x5a));
    for (unsigned i = 100; i < sizeof buf; i++) CHECK(buf[i] == 0xa5);
    guard_page = LOW_FW; guard_page_hits = 0;
    memset(buf, 0xa5, sizeof buf);
    CHECK(xhci_survey_phys_read(LOW_FW + 3, buf, 100) == DEVMAP_EINVAL && guard_page_hits && !memcmp(buf, canary, sizeof buf));
    guard_page = ~0ULL;
    /* a firmware lease token is not usable as a BAR window, and the survey's own windows keep their bounds */
    uint64_t token = 0, va = 0; uint32_t value = 0xdead;
    CHECK(acquire(DEVMAP_SCOPE_FIRMWARE_TABLE, A_PA, PAGE_SIZE, &token, &va) == DEVMAP_OK);
    survey.token = token;
    CHECK(xhci_survey_bar_read32(token, 0, &value) == DEVMAP_ESTATE && value == 0);
    survey.token = 0;
    CHECK(devmap_scope_release(token) == DEVMAP_OK && restored());
    uint64_t t = 5;
    CHECK(xhci_survey_mmio_acquire(0xFDFFF000ULL, &t) == DEVMAP_EINVAL && !t);
    CHECK(xhci_survey_mmio_acquire(0xFF000000ULL, &t) == DEVMAP_EINVAL && !t);
    CHECK(xhci_survey_mmio_acquire(0xFED90800ULL, &t) == DEVMAP_EINVAL && !t);
    CHECK(xhci_survey_mmio_acquire(0x00100000ULL, &t) == DEVMAP_EINVAL && !t);
    CHECK(xhci_survey_mmio_acquire(0xFE000000ULL, &t) == DEVMAP_OK && t && devmap_scope_kind() == DEVMAP_SCOPE_XHCI_SURVEY);
    CHECK(xhci_survey_bar_release(t) == DEVMAP_OK && restored());
    CHECK(xhci_survey_mmio_acquire(0xFEFFF000ULL, &t) == DEVMAP_OK && xhci_survey_bar_release(t) == DEVMAP_OK && restored());
    survey.active = 0;
}

int main(void)
{
    struct { struct mb2_tag_mmap header; struct mb2_mmap_entry entries[16]; } mm = { .header = {.entry_size = sizeof(struct mb2_mmap_entry)} };
    const struct { uint64_t base, length; uint32_t type; } map_in[] = {
        {0x100000, 0x80000, MB2_MMAP_AVAILABLE},   {LOW_FW, 0x10000, 3},   {A_PA, 0x10000, 3},   {B_PA, 0x4000, 4},
        {C_PA, 0x4000, 2},   {D_PA, 0x100000, MB2_MMAP_AVAILABLE},   {E_PA, 0x2000, 2},   {E_PA + 0x1000, 0x1000, MB2_MMAP_AVAILABLE},
        {F_PA, 0x1000, 5},   {G_PA, 0x1000, 3},   {G_PA + 0x1000, 0x1000, MB2_MMAP_AVAILABLE},   {STRADDLE, 0x11000, 3},
        {LOW2, 0x10000, 3},   {TOP_PA, 0x10000, 2},
        {H_PA, 0x2000, 2},   {H_PA + 0x1000, 0x1000, 5},
    };
    unsigned n = sizeof map_in / sizeof map_in[0];
    for (unsigned i = 0; i < n; i++) mm.entries[i] = (struct mb2_mmap_entry){.base = map_in[i].base, .length = map_in[i].length, .type = map_in[i].type};
    struct boot_info bi = {.mmap = &mm.header, .mmap_entries = n};
    CHECK(pmm_init(&bi, NULL) == 0);
    uint64_t pdpt;
    CHECK(pmm_alloc_frame(&root) == 0 && pmm_alloc_frame(&pdpt) == 0);
    ((uint64_t *)phys_to_virt(root))[511] = pdpt | PTE_P | PTE_W;
    cr3 = root;
    CHECK(devmap_firmware_range_ok(A_PA, 1) == 0);                        /* before devmap_init: no memory map yet */
    CHECK(devmap_init(&bi) == 0);
    guard_page = ~0ULL;
    free_frames = pmm_free_count(); maps = devmap_count();
    CHECK(maps == 0 && restored());

    direct_map_range_ok();

    /* accepted: the extremes of what is allowed */
    accepted(A_PA, 0x4000);                    /* the largest lease */
    accepted(A_PA + 0xf000, 0x1000);           /* the last page of an entry */
    accepted(A_PA + 0xf000, 0x2000);           /* across two adjacent firmware entries (reclaim, then NVS) */
    accepted(B_PA, 0x1000);                    /* NVS */
    accepted(C_PA + 0x2000, 0x2000);           /* reserved */
    accepted(E_PA, 0x1000);                    /* the reserved page no RAM entry claims */
    accepted(G_PA, 0x1000);                    /* the page right before RAM */
    accepted(H_PA, 0x1000);                    /* the first page of a reserved entry whose second page is also bad memory */
    CHECK(restored());

    /* rejected: each condition on its own, with its own code */
    rejected(D_PA, 0x1000, DEVMAP_EINVAL, "RAM above 1 GiB");
    rejected(0x7000000000ULL, 0x1000, DEVMAP_EINVAL, "unlisted hole");
    rejected(G_PA, 0x2000, DEVMAP_EINVAL, "second page is RAM");
    rejected(B_PA + 0x3000, 0x2000, DEVMAP_EINVAL, "second page is past the last entry before an unlisted gap");
    rejected(E_PA, 0x2000, DEVMAP_EINVAL, "a page claimed by reserved and by RAM");
    rejected(E_PA + 0x1000, 0x1000, DEVMAP_EINVAL, "RAM overlaps reserved");
    rejected(F_PA, 0x1000, DEVMAP_EINVAL, "bad memory");
    rejected(H_PA, 0x2000, DEVMAP_EINVAL, "two pages across a reserved/bad-memory overlap");
    rejected(H_PA + 0x1000, 0x1000, DEVMAP_EINVAL, "a reserved page that bad memory also claims");
    rejected(LOW2, 0x1000, DEVMAP_EINVAL, "below 1 GiB");
    rejected(LOW_FW, 0x1000, DEVMAP_EINVAL, "below 1 GiB, inside the direct map");
    rejected(STRADDLE + 0xf000, 0x2000, DEVMAP_EINVAL, "straddles 1 GiB");
    rejected(GIB - 0x1000, 0x1000, DEVMAP_EINVAL, "last page below 1 GiB");
    rejected(A_PA + 0x800, 0x1000, DEVMAP_EINVAL, "unaligned address");
    rejected(A_PA, 0x1800, DEVMAP_EINVAL, "length not a page multiple");
    rejected(A_PA, 0, DEVMAP_EINVAL, "empty");
    rejected(A_PA, 0x5000, DEVMAP_EINVAL, "above 16 KiB");
    rejected(A_PA, 0x10000, DEVMAP_EINVAL, "64 KiB");
    rejected(0, 0x1000, DEVMAP_EINVAL, "address 0");
    rejected(0xfed90000ULL, 0x1000, DEVMAP_EINVAL, "MMIO is not firmware-table memory");
    rejected(TOP_PA, 0x1000, DEVMAP_ERANGE, "beyond MAXPHYADDR");
    rejected(UINT64_MAX - 0xfff, 0x1000, DEVMAP_EINVAL, "wraps");

    /* the request itself */
    uint64_t token = 3, va = 3;
    struct devmap_scope_region r = {DEVMAP_SCOPE_FIRMWARE_TABLE, A_PA, 0x1000};
    CHECK(devmap_scope_acquire_region(&r, DEVMAP_WC, &token, &va) == DEVMAP_EINVAL && !token && !va && restored());
    CHECK(devmap_scope_acquire_region(&r, 99, &token, &va) == DEVMAP_EINVAL && restored());
    CHECK(devmap_scope_acquire_region(NULL, DEVMAP_UC, &token, &va) == DEVMAP_EINVAL && restored());
    CHECK(devmap_scope_acquire_region(&r, DEVMAP_UC, NULL, &va) == DEVMAP_EINVAL && devmap_scope_acquire_region(&r, DEVMAP_UC, &token, NULL) == DEVMAP_EINVAL);
    r.kind = 5; CHECK(devmap_scope_acquire_region(&r, DEVMAP_UC, &token, &va) == DEVMAP_EINVAL && restored());
    r.kind = DEVMAP_SCOPE_XHCI_SURVEY; r.length = 0x1000;                  /* a firmware address under another kind */
    CHECK(devmap_scope_acquire_region(&r, DEVMAP_UC, &token, &va) == DEVMAP_EINVAL && restored());
    r.kind = DEVMAP_SCOPE_FIRMWARE_TABLE;
    cr3 = root + 4096; CHECK(devmap_scope_acquire_region(&r, DEVMAP_UC, &token, &va) == DEVMAP_ESTATE && restored()); cr3 = root;
    flags = RFLAGS_IF; CHECK(devmap_scope_acquire_region(&r, DEVMAP_UC, &token, &va) == DEVMAP_ESTATE && restored()); flags = 0;
    in_irq = 1; CHECK(devmap_scope_acquire_region(&r, DEVMAP_UC, &token, &va) == DEVMAP_ESTATE && restored()); in_irq = 0;
    /* page-table allocation failure: exact rollback */
    devmap_scope_budget(0);
    CHECK(devmap_scope_acquire_region(&r, DEVMAP_UC, &token, &va) != DEVMAP_OK && !token && restored());
    devmap_scope_budget(-1);
    CHECK(restored());

    /* a mapping that overlaps the range makes the lease refuse, and the mapping stays exactly as it was */
    void *keep = NULL;
    CHECK(!devmap_map(C_PA, 0x1000, DEVMAP_UC, "existing mapping", &keep, NULL));
    free_frames = pmm_free_count(); maps = devmap_count();
    uint64_t keep_pte = devmap_expected_pte((uint64_t)(uintptr_t)keep);
    rejected(C_PA, 0x2000, DEVMAP_EOVERLAP, "overlaps an existing mapping");
    rejected(C_PA, 0x1000, DEVMAP_EOVERLAP, "the same page as an existing mapping");
    CHECK(devmap_expected_pte((uint64_t)(uintptr_t)keep) == keep_pte && (keep_pte & PTE_W));
    accepted(C_PA + 0x1000, 0x1000);                                       /* the neighbouring page is still fine */

    adapter();
    CHECK(restored());
    printf("PASS: %u checks; real PMM/devmap/xhci_survey C: firmware lease acceptance and every rejection, read-only NX supervisor "
           "UC leaves equal to the registry, audit and VMM device-leaf tamper detection, restoration, adapter refusals; zero PCI writes\n", checks);
    return 0;
}
