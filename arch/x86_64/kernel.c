/*
 * VOS x86_64 kernel entry. Runs in 64-bit long mode in the higher half;
 * boot.S passes the Multiboot2 magic and the physical address of the boot
 * information. All memory is accessed through the higher-half mapping, so
 * nothing here depends on the boot identity map.
 */
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "bootinfo.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "kmap.h"
#include "memlayout.h"
#include "multiboot2.h"
#include "panic.h"
#include "pmm.h"
#include "resv.h"
#include "thread.h"
#include "vmm.h"
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

    int verr = vmm_init();
    if (verr != VMM_OK) {
        serial_puts("vmm: initialization failed: ");
        serial_puts(vmm_diagnostic());
        serial_puts("\r\n");
        panic("vmm: initialization failed");
    }

    /* Every pointer kept from boot must use the kernel alias, not the identity map. */
    int bi_high = (uint64_t)bi.mmap >= KERNEL_VMA && (!bi.loader || (uint64_t)bi.loader >= KERNEL_VMA) &&
                  (!bi.cmdline || (uint64_t)bi.cmdline >= KERNEL_VMA);
    for (uint32_t i = 0; i < bi.module_count; i++)
        bi_high &= (uint64_t)bi.modules[i].cmdline >= KERNEL_VMA;
    if (!bi_high)
        panic("vmm: boot information is reached through the identity map");
#ifdef VOS_TEST
    /* Evidence for kernel-lowaccess: this low address translates before removal. */
    serial_puts("vmm: identity probe 0x100000 = ");
    serial_hex(*(volatile uint32_t *)PMM_LOW_LIMIT);
    serial_puts("\r\n");
#endif
    verr = vmm_remove_identity();
    if (verr != VMM_OK) {
        serial_puts("vmm: identity map removal refused: ");
        serial_puts(vmm_diagnostic());
        serial_puts("\r\n");
        panic("vmm: identity map removal failed");
    }
    serial_puts("vmm: identity map removed\r\n");
#ifdef VOS_TEST_CASE_kernel_lowaccess
    /* Immediately, before anything else can flush the TLB: proves the CR3 reload too. */
    selftest_vmm_fault_lowaccess();
#endif

    /* Kernel W^X: text R-X, rodata R, everything else NX, stack guard pages unmapped. */
    if (kmap_init(&bi) != 0) {
        const struct kmap_fail *f = kmap_failure();

        serial_puts("kmap: refused: ");
        serial_puts(f->what ? f->what : "?");
        serial_puts(": ");
        serial_puts(f->problem ? f->problem : "?");
        serial_puts(" at ");
        serial_hex(f->va);
        serial_puts("\r\n");
        panic("kmap: kernel W^X map failed");
    }
    struct vmm_audit audit;
    if (vmm_audit_space(vmm_kernel_root(), 1, &audit) != VMM_OK) {
        serial_puts("vmm: audit: ");
        serial_puts(audit.first_error);
        serial_puts(" at ");
        serial_hex(audit.first_error_va);
        serial_puts("\r\n");
        panic("vmm: kernel page tables failed the audit");
    }
    serial_puts("vmm: audit passed: kernel alias ");
    serial_dec(audit.leaves_upper);
    serial_puts(" x 4 KiB + ");
    serial_dec(audit.large_upper);
    serial_puts(" x 2 MiB supervisor pages, ");
    serial_dec(audit.exec_supervisor);
    serial_puts(" ring-0 executable (all kernel text, none writable), ");
    serial_dec(audit.tables_lower);
    serial_puts(" lower-half tables\r\n");

    /* 8259 + PIT, IRQ0 unmasked. IF stays 0 in the kernel; ring 3 runs with IF = 1. */
    irq_init();
    /* This code becomes the boot thread (N2.2); user programs keep running synchronously on it. */
    sched_init();

    /* The first native application: GRUB module "hello", run in ring 3. */
    check(app_run_boot_module(&bi), "boot application ran and exited with status 0");

    serial_puts(first_failure ? "boot: FAILED\r\n" : "boot: checks passed\r\n");
#ifdef VOS_TEST
    check(selftest_bootinfo() == 0, "bootinfo/reservation self-test");
    check(selftest_pmm_synthetic() == 0, "pmm synthetic self-test");
    check(selftest_pmm_kernel(&bi, &boot_resv) == 0, "pmm kernel self-test");
    check(selftest_vmm() == 0, "vmm self-test");
    check(selftest_kmap(&bi) == 0, "kmap self-test");
    check(selftest_irq() == 0, "irq self-test");
    check(selftest_sched() == 0, "scheduler self-test");
    check(selftest_user() == 0, "user self-test");
    check(selftest_elf(&bi) == 0, "elf self-test");
#if defined(VOS_TEST_CASE_suite) || defined(VOS_TEST_CASE_irq)
    check(selftest_suite(&bi) == 0, "isolation suite");
#endif
#ifdef VOS_TEST_CASE_smep_exec
    selftest_smep_exec();
#endif
#ifdef VOS_TEST_CASE_smap_touch
    selftest_smap_touch();
#endif
#ifdef VOS_TEST_CASE_modules
    check(selftest_modules(&bi, &boot_resv) == 0, "GRUB module delivery");
#endif
#ifdef VOS_TEST_CASE_pmm_exhaust
    check(selftest_pmm_exhaust(&bi) == 0, "pmm exhaustion");
#endif
#endif
#ifdef VOS_TEST
    selftest_kmap_case(first_failure != 0);
#endif
#ifdef VOS_TEST_CASE_panic
    panic("test case: deliberate panic");
#endif
#ifdef VOS_TEST_CASE_kernel_ud
    __asm__ volatile ("ud2");
#endif
#ifdef VOS_TEST_CASE_kernel_irq_vector
    __asm__ volatile ("int $0x20");     /* an IRQ vector in ring 0 breaks the IF policy */
#endif
#ifdef VOS_TEST_CASE_kernel_int80
    __asm__ volatile ("int $0x80");     /* ring 0 must never use the user gate */
#endif
#ifdef VOS_TEST_CASE_vmm_stale_tlb
    selftest_vmm_fault_stale_tlb();
#endif
#ifdef VOS_TEST_CASE_vmm_nx
    selftest_vmm_fault_nx();
#endif
#ifdef VOS_TEST_CASE_vmm_readonly
    selftest_vmm_fault_readonly();
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
