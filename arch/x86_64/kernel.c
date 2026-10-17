/*
 * VOS x86_64 kernel entry. Runs in 64-bit long mode in the higher half;
 * boot.S passes the Multiboot2 magic and the physical address of the boot
 * information. All memory is accessed through the higher-half mapping, so
 * nothing here depends on the boot identity map.
 */
#include <stddef.h>
#include <stdint.h>

#include "bootinfo.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "memlayout.h"
#include "multiboot2.h"
#include "panic.h"
#include "pmm.h"
#include "resv.h"
#ifdef VOS_TEST
#include "selftest.h"
#endif

#define BOOT_CS         GDT_KERNEL_CS

#ifdef VOS_TEST
#define TEST_EXIT_PORT  0xF4
#define TEST_EXIT_PASS  0x10            /* QEMU exit status (0x10 << 1) | 1 = 33 */
#define TEST_EXIT_FAIL  0x11            /* QEMU exit status 35 */
#endif

/* Physical bounds of the loaded image, from linker.ld. */
extern char __kernel_phys_start[], __kernel_phys_end[];

static struct resv_table boot_resv;
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
    const char *err = "bad Multiboot2 magic";
    if (mb2_magic == MB2_BOOTLOADER_MAGIC)
        err = mb2_parse(phys_to_virt(mb2_info), mb2_info, &bi);
    if (err)
        panic(err);

    uint64_t usable = 0;

    for (uint32_t i = 0; i < bi.mmap_entries; i++) {
        const struct mb2_mmap_entry *e = boot_mmap_entry(&bi, i);

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
    serial_puts("boot: kernel command line ");
    if (bi.cmdline) {
        serial_puts("\"");
        serial_puts_safe(bi.cmdline);
        serial_puts("\"\r\n");
    } else {
        serial_puts("absent\r\n");
    }
    serial_puts("boot: modules ");
    serial_dec(bi.module_count);
    serial_puts("\r\n");
    for (uint32_t i = 0; i < bi.module_count; i++) {
        serial_puts("boot: module ");
        serial_dec(i);
        serial_puts(" ");
        serial_hex(bi.modules[i].start);
        serial_puts("-");
        serial_hex(bi.modules[i].end);
        serial_puts(" \"");
        serial_puts_safe(bi.modules[i].cmdline);
        serial_puts("\"\r\n");
    }

    resv_init(&boot_resv);
    err = boot_reserve(&bi, (uintptr_t)__kernel_phys_start, (uintptr_t)__kernel_phys_end,
                       &boot_resv);
    for (size_t i = 0; i < boot_resv.count; i++) {
        serial_puts("boot: reserved ");
        serial_hex(boot_resv.r[i].start);
        serial_puts("-");
        serial_hex(boot_resv.r[i].end);
        serial_puts(" ");
        serial_puts(boot_resv.r[i].what);
        serial_puts("\r\n");
    }
    if (err)
        panic(err);

    int perr = pmm_init(&bi, &boot_resv);
    if (perr != PMM_OK) {
        serial_puts("pmm: initialization failed: ");
        serial_puts(pmm_strerror(perr));
        serial_puts("\r\n");
        panic("pmm: initialization failed");
    }
    serial_puts("pmm: ");
    serial_dec(pmm_kernel()->frames);
    serial_puts(" frames tracked below 1 GiB, ");
    serial_dec(pmm_kernel()->usable);
    serial_puts(" usable (");
    serial_dec(pmm_kernel()->usable * (PAGE_SIZE / 1024));
    serial_puts(" KiB), ");
    serial_dec(pmm_kernel()->ignored_bytes);
    serial_puts(" bytes of RAM above 1 GiB ignored\r\n");

    serial_puts(first_failure ? "boot: FAILED\r\n" : "boot: checks passed\r\n");
#ifdef VOS_TEST
    check(selftest_bootinfo() == 0, "bootinfo/reservation self-test");
#ifdef VOS_TEST_CASE_modules
    check(selftest_modules(&bi, &boot_resv) == 0, "GRUB module delivery");
#endif
#endif
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
