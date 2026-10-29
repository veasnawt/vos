/* USB.2-C1 platform adapter. Every decision lives in Rust; C supplies a
 * bounded bus-0 candidate count, configuration reads of the one selected
 * function, and a read-only scoped BAR lease. There is no write path here:
 * no pci_write*, no store to the BAR window, no semaphore or command change. */
#include "xhci_qualify.h"
#include "xhci.h"
#include "pci.h"
#include "devmap.h"
#include "pmm.h"
#include "vmm.h"
#include "cpu.h"
#include "irq.h"
#include "boot_option.h"
#include "console.h"
#include "efi_memory.h"

_Static_assert(sizeof(struct xhci_contain_report) == 376, "Rust containment report ABI");
_Static_assert(sizeof(struct xhci_qualify_report) == 224 &&
               _Alignof(struct xhci_qualify_report) == 8, "Rust report ABI");
_Static_assert(offsetof(struct xhci_qualify_report, base) == 8 &&
               offsetof(struct xhci_qualify_report, status) == 16, "Rust report ABI");
_Static_assert(offsetof(struct xhci_qualify_report, vendor) == 32 &&
               offsetof(struct xhci_qualify_report, msix_pba) == 124, "Rust report ABI");
_Static_assert(offsetof(struct xhci_qualify_report, version) == 128 &&
               offsetof(struct xhci_qualify_report, legacy_offset) == 168, "Rust report ABI");
_Static_assert(offsetof(struct xhci_qualify_report, usbcmd_before) == 188 &&
               offsetof(struct xhci_qualify_report, mmio_reads) == 204, "Rust report ABI");
_Static_assert(offsetof(struct xhci_qualify_report, usb1_compatible) == 220, "Rust report ABI");

#ifndef XHCI_QUALIFY_READ32
/* One aligned 32-bit uncached load. Hosted tests substitute a model. */
#define XHCI_QUALIFY_READ32(va, pa) (*(const volatile uint32_t *)(uintptr_t)(va))
#endif

static struct {
    struct pci_addr fn;
    unsigned active, retained;
    uint64_t token;
} qual;

static int qualify_context(void)
{
    return !(read_rflags() & RFLAGS_IF) && !irq_in_handler() &&
           (read_cr3() & PTE_ADDR) == vmm_kernel_root();
}

int xhci_qualify_config32(uint32_t offset, uint32_t *value)
{
    if (!value) return DEVMAP_EINVAL;
    *value = 0;
    if (!qual.active || offset >= 256 || offset & 3) return DEVMAP_EINVAL;
    *value = pci_read32(qual.fn, offset);
    return DEVMAP_OK;
}

int xhci_qualify_bar_acquire(uint64_t base, struct usb_resource *out)
{
    if (!out) return DEVMAP_EINVAL;
    out->token = out->address = 0;
    if (!qual.active || !qualify_context() || qual.token) return DEVMAP_ESTATE;
    /* Bind the descriptor to the selected function's BAR as it reads now,
     * never to a caller value alone: Rust's decoded base must match it. */
    uint32_t low = pci_read32(qual.fn, 0x10), high = 0;
    if (low & 1) return DEVMAP_EINVAL;
    if (((low >> 1) & 3) == 2) high = pci_read32(qual.fn, 0x14);
    else if ((low >> 1) & 3) return DEVMAP_EINVAL;
    if ((((uint64_t)high << 32) | (low & ~15u)) != base) return DEVMAP_ESTATE;
    struct devmap_scope_region r = {DEVMAP_SCOPE_XHCI_BAR, base, DEVMAP_XHCI_BAR_BYTES};
    int err = devmap_scope_acquire_region(&r, DEVMAP_UC, &out->token, &out->address);
    if (err) return err;
    qual.token = out->token; qual.retained = 0;
    return DEVMAP_OK;
}

int xhci_qualify_bar_read32(uint64_t token, uint32_t offset, uint32_t *value)
{
    if (!value) return DEVMAP_EINVAL;
    *value = 0;
    uint64_t va, length;
    if (!qual.active || !qualify_context() || !token || token != qual.token ||
        devmap_scope_kind() != DEVMAP_SCOPE_XHCI_BAR ||
        devmap_scope_extent(token, &va, &length))
        return DEVMAP_ESTATE;
    if (offset & 3 || offset >= length) return DEVMAP_EINVAL;
    *value = XHCI_QUALIFY_READ32(va + offset, 0);
    return DEVMAP_OK;
}

int xhci_qualify_bar_release(uint64_t token)
{
    if (!token || token != qual.token || devmap_scope_kind() != DEVMAP_SCOPE_XHCI_BAR)
        return DEVMAP_ESTATE;
    int err = devmap_scope_release(token); /* rechecks context, registry, PTs */
    if (!err) qual.token = 0;
    return err;
}

void xhci_qualify_bar_retain(uint64_t token)
{
    /* Diagnostic only: the devmap registry keeps the lease; nothing is freed. */
    if (token && token == qual.token) qual.retained = 1;
}

int xhci_qualify_conflict(const char *cmdline)
{
    return boot_option(cmdline, "vos.usb_probe=1") &&
           (boot_option(cmdline, "vos.usb2-qualify=1") || boot_option(cmdline, "vos.usb2-residue=1") ||
            boot_option(cmdline, "vos.usb2-contain=1"));
}

/* USB.2-C3a registry: firmware-owned frames reserved for the whole boot.
 * Separate from the C2 DMA pool; no release path exists. */
static struct {
    uint64_t frame[XHCI_RESIDUE_FRAMES];
    uint8_t role[XHCI_RESIDUE_FRAMES];
    unsigned count, session, ran;
    unsigned frozen;                    /* USB.2-C3b.1: no reservation may be made */
    const struct boot_info *bi;         /* USB.2-C3a.3: C3a.2's inputs, for one session */
    const struct resv_table *resv;
} residue;

void xhci_residue_boot_memory(const struct boot_info *bi, const struct resv_table *t)
{
    residue.bi = bi;
    residue.resv = t;
}

static int residue_owned(uint64_t frame)
{
    for (unsigned i = 0; i < residue.count; i++)
        if (residue.frame[i] == frame) return 1;
    return 0;
}

int xhci_residue_page(uint64_t frame)
{
    if (frame & (PAGE_SIZE - 1)) return 3;
    if (residue_owned(frame)) return 1;
    if (frame >= pmm_kernel()->frames * PAGE_SIZE) return 4;
    switch (pmm_frame_state_in(pmm_kernel(), frame)) {
    case PMM_FRAME_FREE: return 0;
    case PMM_FRAME_ALLOCATED: return 2;
    default:    /* never allocatable: readable only if C3a.2 excluded it as firmware RAM */
        return residue.bi && efi_memory_firmware_frame(residue.bi, residue.resv, frame) == 1 ? 5 : 3;
    }
}

int xhci_residue_reserve(uint64_t frame, uint32_t role)
{
    if (!residue.session || residue.frozen || !qualify_context()) return -4;
    if (residue_owned(frame)) return -3;
    if (residue.count == XHCI_RESIDUE_FRAMES) return -2;  /* fail closed */
    if (pmm_reserve_frame(frame) != PMM_OK) return -4;    /* allocated or protected */
    residue.frame[residue.count] = frame;
    residue.role[residue.count++] = (uint8_t)role;
    return 0;
}

int xhci_residue_read64(uint64_t phys, uint64_t *value)
{
    if (!value) return -1;
    *value = 0;
    uint64_t frame = phys & ~(PAGE_SIZE - 1);
    if (!residue.session || phys & 7 || (!residue_owned(frame) && xhci_residue_page(frame) != 5)) return -1;
    /* RAM this guard owns or the boot excluded: loads only, through the
     * existing write-back kernel alias (no new mapping or alias). */
    *value = *(const volatile uint64_t *)phys_to_virt(phys);
    return 0;
}

struct candidates { unsigned count; struct pci_addr first; };
static int visit(struct pci_addr a, void *context)
{
    struct candidates *c = context;
    if (xhci_class_matches(pci_read32(a, PCI_REVISION))) {
        if (!c->count) c->first = a;
        c->count++;
    }
    return 0; /* never stop early: ambiguity needs the complete bus-0 scan */
}

/* Every report field as name=hex, seven per line; table-driven to keep the
 * C1 image growth small. Interpretation (D-state, semaphore bits) is left to
 * the reader and the native runner; raw values are the evidence. */
#define F(n) {#n, offsetof(struct xhci_qualify_report, n)}
static const struct { char name[20]; uint8_t offset; } fields[] = {
    F(candidates), F(complete), F(vendor), F(device), F(class_revision), F(pci_status),
    F(bar_low), F(bar_high), F(bar_bits), F(pci_caps), F(pm_offset), F(pmcsr_before),
    F(power_state), F(msi_offset), F(msi_control_before), F(msix_offset),
    F(msix_control_before), F(msix_table), F(msix_pba), F(command_before), F(version),
    F(caplength), F(hcs1), F(hcs2), F(hcc1), F(doorbell), F(runtime), F(pagesize),
    F(xecp_headers), F(protocol_ports), F(usb1_compatible), F(legacy_offset),
    F(usblegsup_before), F(usblegctlsts_before), F(usblegsup_after), F(usblegctlsts_after),
    F(usbcmd_before), F(usbsts_before), F(usbcmd_after), F(usbsts_after), F(command_after),
    F(bar_low_after), F(bar_high_after), F(pmcsr_after), F(msi_control_after),
    F(msix_control_after), F(mmio_reads), F(lease_acquired), F(lease_released),
};
#undef F

static void print(const struct xhci_qualify_report *r)
{
    serial_puts("usb2c1: bus="); serial_dec(qual.fn.bus); serial_puts(" dev=");
    serial_dec(qual.fn.dev); serial_puts(" fn="); serial_dec(qual.fn.fn);
    serial_puts(" base="); serial_hex(r->base);
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        serial_puts(i % 7 ? " " : "\r\nusb2c1: ");
        serial_puts(fields[i].name); serial_puts("=");
        serial_hex(*(const uint32_t *)((const char *)r + fields[i].offset));
    }
    serial_puts("\r\n");
    if (r->mapping_error) {
        serial_puts("usb2c1: mapping_error="); serial_puts(devmap_strerror(r->mapping_error));
        serial_puts("\r\n");
    }
}

int xhci_qualify_boot(const char *cmdline)
{
    if (!boot_option(cmdline, "vos.usb2-qualify=1")) return 1; /* no effects or output */
    serial_puts("usb2c1: read-only xHCI qualification; QEMU 1b36:000d 16 KiB profile; PCI bus 0 only\r\n");
    struct candidates c = {0};
    struct pci_addr unused;
    int stopped = pci_scan_bus0(visit, &c, &unused);
    uint64_t free = pmm_free_count();
    unsigned maps = devmap_count();
    struct xhci_qualify_report r;
    qual.fn = c.first; qual.active = 1;
    int32_t status = vos_xhci_qualify_v1(c.count, !stopped, devmap_maxphys(), &r);
    qual.active = 0;
    if (status == -2) {
        serial_puts("usb2c1: FAIL: invalid report interface\r\n");
        return 0;
    }
    print(&r);
    int restored = !qual.token && !devmap_scope_token() && pmm_free_count() == free &&
                   devmap_count() == maps && !devmap_audit();
    serial_puts(status == 0 ? "usb2c1: QUALIFIED: " : status > 0 ? "usb2c1: REJECTED: " : "usb2c1: FAIL: ");
    serial_puts(r.reason_text ? r.reason_text : "unknown");
    serial_puts("\r\n");
    if (status < 0 || !restored) {
        serial_puts(restored ? "usb2c1: integrity failure; resources restored\r\n"
                             : "usb2c1: integrity failure; lease retained\r\n");
        return 0;
    }
    serial_puts("usb2c1: resources restored; no PCI/xHCI writes, semaphore, reset, run, BME, DMA "
                "or enumeration\r\n");
    serial_puts("usb2c1: DMA, coherency and IOMMU not established; activation unsupported\r\n");
    return 1;
}

#define R(n) {#n, offsetof(struct xhci_residue_report, n)}
static const struct { char name[20]; uint16_t offset; } residue_fields[] = {
    R(usbcmd), R(usbsts), R(config), R(iman0), R(erstsz0), R(findings), R(reserved),
    R(owned_again), R(protected_pages), R(hazards), R(pages), R(interrupters),
    R(interrupter_enables), R(refusal), R(violation), R(plan_len), R(excluded), R(inspection),
};
#undef R

int xhci_residue_boot(const char *cmdline)
{
    if (!boot_option(cmdline, "vos.usb2-residue=1")) return 1; /* no effects or output */
    if (residue.ran++) return 0;
    serial_puts("usb2c3a: firmware DMA residue guard; reads only, no PCI/xHCI writes\r\n");
    struct candidates c = {0};
    struct pci_addr unused;
    int stopped = pci_scan_bus0(visit, &c, &unused);
    uint64_t free = pmm_free_count();
    unsigned maps = devmap_count();
    struct xhci_residue_report r;
    qual.fn = c.first; qual.active = 1; residue.session = 1;
    int32_t status = vos_xhci_residue_v1(c.count, !stopped, devmap_maxphys(), &r);
    qual.active = 0; residue.session = 0; residue.bi = NULL; residue.resv = NULL;
    if (status == -2) {
        serial_puts("usb2c3a: FAIL: invalid report interface\r\n");
        return 0;
    }
    print(&r.q);
    serial_puts("usb2c3a: pmm_allocated_before="); serial_dec(pmm_kernel()->usable - free);
    serial_puts(" crcr="); serial_hex(r.crcr); serial_puts(" dcbaap="); serial_hex(r.dcbaap);
    serial_puts(" erstba0="); serial_hex(r.erstba0); serial_puts(" erdp0="); serial_hex(r.erdp0);
    for (unsigned i = 0; i < sizeof(residue_fields) / sizeof(residue_fields[0]); i++) {
        serial_puts(i % 8 ? " " : "\r\nusb2c3a: ");
        serial_puts(residue_fields[i].name); serial_puts("=");
        serial_hex(*(const uint32_t *)((const char *)&r + residue_fields[i].offset));
    }
    serial_puts("\r\nusb2c3a: plan=");
    for (unsigned i = 0; i < r.plan_len && i < sizeof(r.plan); i++) { serial_hex(r.plan[i]); serial_puts(" "); }
    for (unsigned i = 0; i < residue.count; i++) {
        serial_puts("\r\nusb2c3a: reserved frame="); serial_hex(residue.frame[i]);
        serial_puts(" role="); serial_dec(residue.role[i]);
    }
    serial_puts("\r\nusb2c3a: reuse_protected="); serial_dec(r.inspection & 1);
    serial_puts(" inspection_complete="); serial_dec(r.inspection >> 1 & 1);
    serial_puts("; firmware memory is never released\r\n");
    int restored = !qual.token && !devmap_scope_token() && devmap_count() == maps && !devmap_audit() &&
                   pmm_free_count() + residue.count == free && residue.count == r.reserved;
    if (r.q.status < 0 || !restored) {
        serial_puts("usb2c3a: FAIL: integrity (lease, mapping or reservation accounting)\r\n");
        return 0;
    }
    serial_puts(r.status == 0 ? "usb2c3a: PLAN VALID (dry run, not executed); residue complete and protected\r\n"
                : r.status > 0 ? "usb2c3a: REFUSED: residue incomplete or controller unqualified; protected pages kept\r\n"
                               : "usb2c3a: REFUSED: HAZARD: firmware frame already allocated; activation refused\r\n");
    return 1;
}

/* ---- USB.2-C3b.1 containment dry-run: reports only, writes nothing ---- */
static unsigned contain_ran;

/* Packed names in bit/index order; the report fields are contiguous u32s. */
static const char contain_fields[] =
    "state\0refusals\0uefi\0command\0status\0usbcmd\0usbsts\0findings\0excluded\0irq_off\0plan_len\0trace_len";
static const char contain_refusals[] =
    "not-uefi\0unqualified\0profile\0pci-command\0pci-status\0pcie-unknown\0pcie-busy\0interrupts\0running\0"
    "usbcmd\0usbsts\0ports\0slots\0scratchpads\0residue\0memory\0changed\0plan\0unreadable";
static const char contain_kinds[] = "?\0revalidate\0pci-write\0mmio-write\0reset-poll\0verify";
static const char contain_results[] = "ok\0CHANGED\0NOT-ISSUED\0NOT-RUN";

static const char *nth(const char *list, unsigned n)
{
    while (n--) while (*list++) ;
    return list;
}

int xhci_contain_boot(const char *cmdline)
{
    if (!boot_option(cmdline, "vos.usb2-contain=1")) return 1; /* no effects or output */
    if (contain_ran++) return 0;
    serial_puts("usb2c3b1: containment DRY-RUN (read-only rehearsal); this build has no PCI or xHCI write path\r\n");
    struct candidates c = {0};
    struct pci_addr unused;
    int stopped = pci_scan_bus0(visit, &c, &unused);
    uint64_t free = pmm_free_count();
    unsigned maps = devmap_count(), owned = residue.count;
    const struct boot_info *bi = residue.bi;
    uint32_t uefi = bi && bi->efi && bi->efi_mmap_tags == 1 && !bi->efi_bs_active;
    struct xhci_contain_report r;
    qual.fn = c.first; qual.active = 1; residue.session = 1; residue.frozen = 1;
    int32_t status = vos_xhci_contain_v1(c.count, !stopped, devmap_maxphys(), uefi, &r);
    qual.active = 0; residue.session = 0; residue.frozen = 0; residue.bi = NULL; residue.resv = NULL;
    if (status == -2) {
        serial_puts("usb2c3b1: FAIL: invalid report interface\r\n");
        return 0;
    }
    print(&r.q);
    const uint32_t *f = &r.state;
    const char *name = contain_fields;
    for (unsigned i = 0; i < 12; i++, name += 1) {
        serial_puts(i % 8 ? " " : "\r\nusb2c3b1: ");
        serial_puts(name); serial_puts("="); serial_hex(f[i]);
        while (*name) name++;
    }
    for (unsigned i = 0; i < r.trace_len && i < 6; i++) {
        const uint32_t *t = r.trace[i];
        serial_puts("\r\nusb2c3b1: trace="); serial_dec(t[0] >> 24);
        serial_puts(" "); serial_puts(nth(contain_kinds, t[0] & 0xff));
        serial_puts(" "); serial_puts(nth(contain_results, t[0] >> 8 & 0xff));
        serial_puts(" width="); serial_dec(t[0] >> 16 & 0xff);
        serial_puts(" offset="); serial_hex(t[1]); serial_puts(" old="); serial_hex(t[2]);
        serial_puts(" new="); serial_hex(t[3]);
    }
    serial_puts("\r\n");
    int restored = !qual.token && !devmap_scope_token() && devmap_count() == maps && !devmap_audit() &&
                   pmm_free_count() == free && residue.count == owned && pmm_check_in(pmm_kernel()) == PMM_OK;
    if (r.q.status < 0 || !restored) {
        serial_puts("usb2c3b1: FAIL: integrity (lease, mapping, reservation or PMM state)\r\n");
        return 0;
    }
    if (status == 0) {
        serial_puts("usb2c3b1: DRY-RUN COMPLETE: eligible; every proposed write is listed above and NONE was issued\r\n");
        return 1;
    }
    serial_puts("usb2c3b1: REFUSED:");
    for (unsigned b = 0; b < 19; b++)
        if (r.refusals >> b & 1) { serial_puts(" "); serial_puts(nth(contain_refusals, b)); }
    serial_puts("; nothing written\r\n");
    return 1;
}
