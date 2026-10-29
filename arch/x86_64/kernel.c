/*
 * VOS x86_64 kernel entry. Runs in 64-bit long mode in the higher half;
 * boot.S passes the Multiboot2 magic and the physical address of the boot
 * information. All memory is accessed through the higher-half mapping, so
 * nothing here depends on the boot identity map.
 */
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "app_payload.h"
#ifdef VOS_EXEC_VERIFY
#include "exec_verify.h"
#include "handoff_verify.h"
#endif
#include "bootinfo.h"
#include "console.h"
#include "cpu.h"
#include "devmap.h"
#include "efi_memory.h"
#include "boot_option.h"
#include "xhci.h"
#include "xhci_qualify.h"
#include "user_console.h"
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "input_console.h"
#include "vsh_boot.h"
#include "net_boot.h"
#include "kmap.h"
#include "memlayout.h"
#include "multiboot2.h"
#include "panic.h"
#include "pmm.h"
#include "resv.h"
#include "rust_kernel.h"
#include "usb_resources.h"
#include "usb_dma.h"
#include "screen.h"
#include "desktop.h"
#include "gui_verify.h"
#include "thread.h"
#include "virtio_blk.h"
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
    /* U1: nothing is written to any screen before screen_init decides what the display is. */
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
    /* USB.2-C3a.2: firmware memory leaves the allocatable set here, before
     * pmm_init and therefore before the first allocation. */
    struct efi_memory_report efi = { 0 };
    int efi_status = err ? EFI_MEMORY_REFUSED : efi_memory_exclude(&bi, &boot_resv, &efi);
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
    if (efi_status == EFI_MEMORY_APPLIED) {
        serial_puts("efi: ");
        serial_dec(efi.frames);
        serial_puts(" frames excluded in ");
        serial_dec(efi.ranges);
        serial_puts(" ranges from ");
        serial_dec(efi.descriptors);
        serial_puts(" descriptors, coalesced ");
        serial_dec(efi.coalesced);
        serial_puts("\r\n");
    } else if (efi_status == EFI_MEMORY_NOT_EFI) {
        serial_puts("efi: no EFI map (BIOS): e820 protection only\r\n");
    } else {
        serial_puts("efi: refusing to boot: ");
        serial_puts(efi_memory_strerror(efi.error));
        serial_puts("\r\n");
        panic("efi: firmware memory unclassified");
    }

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
    if (efi_status == EFI_MEMORY_APPLIED) {
        serial_puts("efi: ");
        serial_dec(pmm_kernel()->usable);
        serial_puts(" usable frames remain (floor ");
        serial_dec(EFI_MEMORY_FLOOR_FRAMES);
        serial_puts(")\r\n");
        if (pmm_kernel()->usable < EFI_MEMORY_FLOOR_FRAMES)
            panic("efi: usable memory below floor");
    }

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
    /* U1: the device-memory window (framebuffer) and the PAT write-combining entry. */
    int derr = devmap_init(&bi);
    serial_puts("devmap: ");
    if (derr == DEVMAP_OK) {
        serial_puts("window at ");
        serial_hex(DEVMAP_VA_BASE);
        serial_puts(", ");
        serial_dec(devmap_maxphys());
        serial_puts("-bit physical addresses, write-combining ");
        serial_puts(devmap_wc_available() ? "via PAT entry 4" : "unavailable (no PAT): UC only");
    } else {
        serial_puts("not available: ");
        serial_puts(devmap_strerror(derr));
    }
    serial_puts("\r\n");
    /* USB.2-C1: USB.1's permanent probe mapping and the qualification lease
     * are mutually exclusive. Refuse the pair before either reads the BAR. */
    int usb_conflict = xhci_qualify_conflict(bi.cmdline);
    /* USB.2-C3a: opt-in firmware DMA residue guard. It runs here, right after
     * the device window exists and before screen_init or any later PMM user,
     * and verifies every firmware frame is still unallocated as it reserves it. */
    if (!usb_conflict) {
        xhci_residue_boot_memory(&bi, &boot_resv);      /* USB.2-C3a.3: excluded firmware RAM */
        check(xhci_residue_boot(bi.cmdline), "USB.2-C3a firmware residue guard integrity");
        xhci_residue_boot_memory(&bi, &boot_resv);      /* USB.2-C3b.1: the same inputs for its own session */
        check(xhci_contain_boot(bi.cmdline), "USB.2-C3b.1 containment dry-run integrity");
#ifdef VOS_CONTAIN_TEST
        xhci_residue_boot_memory(&bi, &boot_resv);      /* USB.2-C3b.2 (QEMU test images only) */
        check(xhci_contain_execute_boot(bi.cmdline), "USB.2-C3b.2 controlled containment integrity");
#endif
    }
    /* U1: the screen console (framebuffer, VGA text when positively known, or none). */
    screen_init(&bi);
    user_console_set_diagnostics(boot_option(bi.cmdline, "vos.input_diag=1"));
    check(!usb_conflict, "vos.usb_probe=1 is exclusive with vos.usb2-qualify=1 and vos.usb2-residue=1");
    if (!usb_conflict)
        xhci_boot_probe(bi.cmdline);

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

    /* R0-B: CPU protections, W^X, diagnostics and the kernel CR3 are established.
     * Runtime inputs and a separate unsigned-C oracle keep this a real ABI call. */
    uint64_t rust_seed = (uint64_t)mb2_magic ^ rip;
    uint64_t rust_salt = rsp ^ read_cr3();
    uint64_t rust_result = vos_rust_probe(rust_seed, rust_salt);
#ifdef VOS_TEST_CASE_rust_result
    serial_puts("rust: test-only result corruption after Rust return\r\n");
    rust_result ^= 1;
#endif
    if (rust_result != rust_probe_expected(rust_seed, rust_salt))
        panic("Rust kernel probe result mismatch");
    serial_puts("rust: kernel probe PASS; fn="); serial_hex((uint64_t)vos_rust_probe);
    serial_puts(" seed="); serial_hex(rust_seed); serial_puts(" salt="); serial_hex(rust_salt);
    serial_puts(" result="); serial_hex(rust_result); serial_puts("\r\n");
#ifdef VOS_TEST_CASE_rust_panic
    serial_puts("rust: invoking controlled Rust panic\r\n");
    vos_rust_panic_test();
#endif

    /* 8259 + PIT, IRQ0 unmasked. IF stays 0 in the kernel; ring 3 runs with IF = 1. */
    irq_init();
    /* This code becomes the boot thread (N2.2); user programs keep running synchronously on it. */
    sched_init();
    if (!usb_resources_verify(bi.cmdline))
        panic("USB.2-B resource ownership verification failed");
    /* Opt-in, read-only; a fail-closed rejection is a diagnostic, not a fault. */
    if (!usb_conflict)
        check(xhci_qualify_boot(bi.cmdline), "USB.2-C1 read-only xHCI qualification integrity");
    /* Opt-in, software-only DMA ownership exercise; no device access. */
    check(usb_dma_boot(bi.cmdline), "USB.2-C2 DMA ownership exercise");
    /* N3.1: the first virtio-blk device on PCI bus 0, read-only and polled; its absence is not an error. */
    vblk_init();

    /* The common payload validates every source before any application runs.
     * Legacy boot/test profiles retain their existing hello path. */
#ifndef VOS_TEST
    int payload_ready = 1;
#ifdef VOS_EXEC_VERIFY
    if (exec_verify_requested(bi.cmdline)) {
        payload_ready=exec_verify_prepare(&bi,&boot_resv);
        check(payload_ready,"execution verification sources");
    } else
#endif
    if (app_payload_requested(bi.cmdline)) {
        payload_ready = app_payload_prepare(&bi, &boot_resv);
        check(payload_ready, "common native application catalog");
    }
    if (payload_ready)
#endif
        check(app_run_boot_module(&bi), "boot application ran and exited with status 0");

    serial_puts(first_failure ? "boot: FAILED\r\n" : "boot: checks passed\r\n");
    screen_status(first_failure ? SCREEN_FAIL : SCREEN_PASS, first_failure ? "boot: FAILED" : "boot: checks passed");
#ifdef VOS_TEST
    check(selftest_bootinfo() == 0, "bootinfo/reservation self-test");
    check(selftest_pmm_synthetic() == 0, "pmm synthetic self-test");
    check(selftest_pmm_kernel(&bi, &boot_resv) == 0, "pmm kernel self-test");
    check(selftest_vmm() == 0, "vmm self-test");
    check(selftest_kmap(&bi) == 0, "kmap self-test");
    check(selftest_irq() == 0, "irq self-test");
    check(selftest_sched() == 0, "scheduler self-test");
    check(selftest_ipc() == 0, "ipc self-test");
    check(selftest_user() == 0, "user self-test");
    check(selftest_elf(&bi) == 0, "elf self-test");
    check(selftest_vblk(&bi) == 0, "vblk self-test");
    check(selftest_blk(&bi) == 0, "block layer self-test");
    check(selftest_ext2(&bi) == 0, "ext2 reader self-test");
    check(selftest_file(&bi) == 0, "internal file layer self-test");
    check(selftest_screen(&bi) == 0, "screen console self-test");
    check(selftest_io_scope() == 0, "bounded storage scope self-test"); STORAGE_STRESS_C_DISPATCH(&bi)
#ifdef VOS_TEST_CASE_accounting
    if (first_failure)
        panic("accounting prerequisite self-test failed");
    extern int selftest_accounting(const struct boot_info *);
    check(selftest_accounting(&bi) == 0, "N3.6 accounting smoke tests");
#endif
#ifdef VOS_TEST_CASE_storage_stress_b
    if (first_failure) panic("Gate B prerequisite self-test failed");
    extern int selftest_storage_stress_b(const struct boot_info *);
    check(selftest_storage_stress_b(&bi) == 0, "N3.6 selected Gate B stress");
#endif
#ifdef VOS_TEST_CASE_storage_stress
    if (first_failure)
        panic("storage stress prerequisite self-test failed");
    extern int selftest_storage_stress(const struct boot_info *);
    check(selftest_storage_stress(&bi) == 0, "N3.6 Gate A canary only");
#endif
#if defined(VOS_TEST_CASE_suite) || defined(VOS_TEST_CASE_irq)
    check(selftest_suite(&bi) == 0, "isolation suite");
#endif
#ifdef VOS_TEST_CASE_ipc
    check(selftest_ipc_scenarios(&bi) == 0, "IPC scenarios");
#endif
#ifdef VOS_TEST_CASE_stress
    check(selftest_stress(&bi) == 0, "N2.5 stress tests");
#endif
#ifdef VOS_TEST_CASE_mp
    check(selftest_mp(&bi) == 0, "multi-process tests");
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
    selftest_sched_case(&bi, first_failure != 0); STORAGE_STRESS_D_DISPATCH(&bi)
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
    screen_status(first_failure ? SCREEN_FAIL : SCREEN_PASS, first_failure ? "VOS-TEST: FAIL" : "VOS-TEST: PASS");
    outl(TEST_EXIT_PORT, first_failure ? TEST_EXIT_FAIL : TEST_EXIT_PASS);
#else
#ifdef VOS_EXEC_VERIFY
    if (!first_failure && exec_verify_requested(bi.cmdline)) {
        check(exec_verify_run(),"ABI v4 execution verification");
#ifdef VOS_HANDOFF_VERIFY
        if(!first_failure && handoff_verify_requested(bi.cmdline))check(handoff_verify_run(&bi),"exclusive console handoff");
#endif
        if (!first_failure) check(vsh_run(&bi),"unchanged vsh after execution verification");
    } else
#endif
    if (!first_failure && net_requested(bi.cmdline)) {
        check(net_boot_run(bi.cmdline), "native network adapter");
        screen_status(first_failure ? SCREEN_FAIL : SCREEN_PASS,first_failure ? "Network check failed" : "N5 adapter increment");
    } else if (!first_failure && app_payload_requested(bi.cmdline)) {
        if(desktop_requested(bi.cmdline)) {
            int visible=screen_desktop_begin();
            serial_puts(visible ? "desktop: scene ready; kernel-owned BGRX surface, bounded console viewport\r\n" :
                                 "desktop: unavailable; existing console fallback\r\n");
        }
        if(gui_boot_requested(bi.cmdline))check(gui_boot_run(&bi),"native GUI surface/event application");
        /* Reuse the trusted catalog, explicit grants and execution broker. */
        check(vsh_run(&bi), "common payload native vsh");
        screen_status(first_failure ? SCREEN_FAIL : SCREEN_PASS,
                      first_failure ? "Common payload failed" : "Common payload: shell reclaimed, catalog retained");
    } else if (!first_failure && rixie_requested(bi.cmdline)) {
        check(rixie_run(&bi), "Rixie Core v0");
        screen_status(first_failure ? SCREEN_FAIL : SCREEN_PASS,first_failure ? "Rixie failed" : "Rixie session complete");
    } else if (!first_failure && rust_demo_requested(bi.cmdline)) {
        check(rust_demo_run(&bi), "native Rust SDK example");
        screen_status(first_failure ? SCREEN_FAIL : SCREEN_PASS,first_failure ? "Rust SDK example failed" : "Rust SDK example complete");
    } else if (!first_failure && vsh_requested(bi.cmdline)) {
        check(vsh_run(&bi), "native vsh");
        if (first_failure) screen_status(SCREEN_FAIL, "native vsh failed");
    } else if (!first_failure && input_console_requested(bi.cmdline))
        input_console_run();
#endif
}
