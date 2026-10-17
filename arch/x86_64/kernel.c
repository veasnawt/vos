/*
 * VOS x86_64 kernel entry. Runs in 64-bit long mode in the higher half;
 * boot.S passes the Multiboot2 magic and the physical address of the boot
 * information. All memory is accessed through the higher-half mapping, so
 * nothing here depends on the boot identity map.
 */
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "memlayout.h"
#include "multiboot2.h"
#include "panic.h"

#define MB2_INFO_MAX    0x10000u        /* sanity bound for the info block */

#define BOOT_CS         GDT_KERNEL_CS

#ifdef VOS_TEST
#define TEST_EXIT_PORT  0xF4
#define TEST_EXIT_PASS  0x10            /* QEMU exit status (0x10 << 1) | 1 = 33 */
#define TEST_EXIT_FAIL  0x11            /* QEMU exit status 35 */
#endif

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))

/* Physical bounds of the loaded image, from linker.ld. */
extern char __kernel_phys_start[], __kernel_phys_end[];

/* A physical range in use at boot that must never be handed out as free. */
struct phys_range {
    uint64_t start, end;                /* [start, end), page-aligned outward */
    const char *what;
};

struct boot_info {
    uint64_t info_start, info_end;      /* Multiboot2 information block */
    const char *loader;
    const struct mb2_tag_mmap *mmap;
    uint32_t mmap_entries;
};

static struct phys_range boot_reserved[2];
static size_t boot_reserved_count;
static const char *first_failure;
static volatile uint64_t bss_probe;     /* boot.S zeroes .bss, so this must read 0 */

static void check(int ok, const char *what)
{
    if (ok)
        return;
    serial_puts("boot: check failed: ");
    serial_puts(what);
    serial_puts("\r\n");
    if (!first_failure)
        first_failure = what;
}

static const struct mb2_mmap_entry *mmap_entry(const struct mb2_tag_mmap *m, uint32_t i)
{
    return (const void *)((const uint8_t *)m + sizeof(*m) + (size_t)i * m->entry_size);
}

/* Validate the Multiboot2 information block and record the tags we use. */
static int parse_multiboot2(uint64_t info_phys, struct boot_info *bi)
{
    if (info_phys == 0 || info_phys % MB2_TAG_ALIGN != 0 ||
        info_phys + sizeof(struct mb2_info) > BOOT_MAP_LIMIT)
        return 0;

    const uint8_t *base = phys_to_virt(info_phys);
    uint32_t total = ((const struct mb2_info *)base)->total_size;

    if (total < sizeof(struct mb2_info) + sizeof(struct mb2_tag) ||
        total > MB2_INFO_MAX || info_phys + total > BOOT_MAP_LIMIT)
        return 0;
    bi->info_start = info_phys;
    bi->info_end = info_phys + total;

    uint32_t off = sizeof(struct mb2_info);
    while (off + sizeof(struct mb2_tag) <= total) {
        const struct mb2_tag *tag = (const void *)(base + off);

        if (tag->size < sizeof(*tag) || tag->size > total - off)
            return 0;
        switch (tag->type) {
        case MB2_TAG_END:
            return bi->mmap != NULL;
        case MB2_TAG_LOADER_NAME: {
            const struct mb2_tag_string *s = (const void *)tag;
            uint32_t len = tag->size - sizeof(*tag);

            if (len > 0 && s->string[len - 1] == '\0')
                bi->loader = s->string;
            break;
        }
        case MB2_TAG_MMAP: {
            const struct mb2_tag_mmap *m = (const void *)tag;

            if (tag->size < sizeof(*m) || m->entry_size < sizeof(struct mb2_mmap_entry) ||
                m->entry_size % 8 != 0)
                return 0;
            bi->mmap = m;
            bi->mmap_entries = (tag->size - sizeof(*m)) / m->entry_size;
            break;
        }
        }
        off += (tag->size + MB2_TAG_ALIGN - 1) & ~(uint32_t)(MB2_TAG_ALIGN - 1);
    }
    return 0;                           /* no end tag */
}

static int in_usable_ram(const struct boot_info *bi, uint64_t start, uint64_t end)
{
    for (uint32_t i = 0; i < bi->mmap_entries; i++) {
        const struct mb2_mmap_entry *e = mmap_entry(bi->mmap, i);

        if (e->type == MB2_MMAP_AVAILABLE && start >= e->base && end <= e->base + e->length)
            return 1;
    }
    return 0;
}

/*
 * Record [start, end) as in use, rounded outward to whole pages. The exact
 * range must lie in usable RAM inside the boot mapping.
 */
static int reserve(const struct boot_info *bi, uint64_t start, uint64_t end, const char *what)
{
    if (boot_reserved_count >= ARRAY_SIZE(boot_reserved) || start >= end ||
        end > BOOT_MAP_LIMIT || !in_usable_ram(bi, start, end))
        return 0;

    struct phys_range *r = &boot_reserved[boot_reserved_count++];
    r->start = start & ~(PAGE_SIZE - 1);
    r->end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    r->what = what;
    return 1;
}

void kernel_main(uint32_t mb2_magic, uint32_t mb2_info)
{
    struct boot_info bi = { 0 };
    uint64_t efer = rdmsr(MSR_EFER), rip, rsp;
    uint16_t cs;

    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile ("lea 0(%%rip), %0" : "=r"(rip));
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));

    serial_init();
    vga_puts("Hello from VOS!");
    serial_puts("Hello from VOS!\r\n");

    check(efer & EFER_LMA, "long mode active (EFER.LMA)");
    check(cs == BOOT_CS, "64-bit code segment");
    check(rip >= KERNEL_VMA && rsp >= KERNEL_VMA, "running in the higher half");
    check(bss_probe == 0, ".bss zeroed");
    serial_puts("boot: x86_64 long mode, cs=");
    serial_hex(cs);
    serial_puts(" efer=");
    serial_hex(efer);
    serial_puts(" rip=");
    serial_hex(rip);
    serial_puts("\r\n");

    gdt_init();
    idt_init();
    cpu_init_protections();

    check(mb2_magic == MB2_BOOTLOADER_MAGIC, "Multiboot2 magic");
    int have_info = mb2_magic == MB2_BOOTLOADER_MAGIC && parse_multiboot2(mb2_info, &bi);
    check(have_info, "Multiboot2 information and memory map");

    if (have_info) {
        uint64_t usable = 0;

        for (uint32_t i = 0; i < bi.mmap_entries; i++) {
            const struct mb2_mmap_entry *e = mmap_entry(bi.mmap, i);
            if (e->type == MB2_MMAP_AVAILABLE)
                usable += e->length;
        }
        serial_puts("boot: loader \"");
        serial_puts(bi.loader ? bi.loader : "?");
        serial_puts("\", memory map ");
        serial_dec(bi.mmap_entries);
        serial_puts(" entries, ");
        serial_dec(usable);
        serial_puts(" bytes usable\r\n");
        serial_puts("boot: multiboot2 info at ");
        serial_hex(bi.info_start);
        serial_puts("-");
        serial_hex(bi.info_end);
        serial_puts("\r\n");

        check(reserve(&bi, (uintptr_t)__kernel_phys_start, (uintptr_t)__kernel_phys_end,
                      "kernel image"), "kernel image lies in usable RAM");
        check(reserve(&bi, bi.info_start, bi.info_end, "multiboot2 info"),
              "multiboot2 info lies in usable RAM");
        for (size_t i = 0; i < boot_reserved_count; i++) {
            serial_puts("boot: reserved ");
            serial_hex(boot_reserved[i].start);
            serial_puts("-");
            serial_hex(boot_reserved[i].end);
            serial_puts(" ");
            serial_puts(boot_reserved[i].what);
            serial_puts("\r\n");
        }
    }

    serial_puts(first_failure ? "boot: FAILED\r\n" : "boot: checks passed\r\n");
#ifdef VOS_TEST_CASE_panic
    panic("test case: deliberate panic");
#endif
#ifdef VOS_TEST_CASE_kernel_ud
    __asm__ volatile ("ud2");
#endif
#ifdef VOS_TEST_CASE_kernel_df
    /*
     * Make #GP undeliverable (its gate is not present), then raise one: the
     * failed delivery is a second contributory fault, which becomes #DF on IST1.
     */
    idt_clear_gate(13);
    __asm__ volatile ("mov %0, %%ds" : : "r"((uint16_t)0x99));
#endif
#ifdef VOS_TEST
    serial_puts(first_failure ? "VOS-TEST: FAIL\r\n" : "VOS-TEST: PASS\r\n");
    outl(TEST_EXIT_PORT, first_failure ? TEST_EXIT_FAIL : TEST_EXIT_PASS);
#endif
}
