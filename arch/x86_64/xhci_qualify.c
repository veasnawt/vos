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
    return boot_option(cmdline, "vos.usb_probe=1") && boot_option(cmdline, "vos.usb2-qualify=1");
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
