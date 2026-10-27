#include "xhci.h"
#include "boot_option.h"
#include "devmap.h"
#include "console.h"

static uint32_t config(void *ctx, struct pci_addr a, unsigned off)
{
    (void)ctx;
    return pci_read32(a, off);
}
static int check_range(void *ctx, uint64_t pa, uint64_t len)
{
    (void)ctx;
    return devmap_check(pa, len);
}
static int map_header(void *ctx, uint64_t pa, size_t len,
                      const volatile uint32_t **out, size_t *available)
{
    (void)ctx;
    void *va = 0;
    int err = devmap_map(pa, len, DEVMAP_UC, "USB.1 xHCI capabilities", &va, 0);
    *out = va; *available = err ? 0 : len;
    return err;
}
struct scan { unsigned count, ok, unsupported, invalid; struct xhci_probe_ops ops; };
static int visit(struct pci_addr a, void *context)
{
    struct scan *s = context;
    if (!xhci_class_matches(pci_read32(a, 8))) return 0;
    struct xhci_info r;
    enum xhci_status status = xhci_probe(a, &s->ops, &r);
    s->count++;
    serial_puts("usb: xhci bus="); serial_dec(a.bus);
    serial_puts(" dev="); serial_dec(a.dev); serial_puts(" fn="); serial_dec(a.fn);
    serial_puts(" vendor="); serial_hex(r.vendor); serial_puts(" device="); serial_hex(r.device);
    serial_puts(" command="); serial_hex(r.command); serial_puts(" status="); serial_hex(r.pci_status);
    serial_puts(" bar="); serial_hex(r.base); serial_puts(" bits="); serial_dec(r.address_bits);
    serial_puts("\r\nusb: ");
    if (status == XHCI_OK) {
        s->ok++;
        serial_puts("inspected version="); serial_hex(r.version);
        serial_puts(" caplength="); serial_dec(r.caplength);
        serial_puts(" ports="); serial_dec(r.ports); serial_puts(" slots="); serial_dec(r.slots);
        serial_puts(" interrupters="); serial_dec(r.interrupters);
        serial_puts(" dboff="); serial_hex(r.doorbell); serial_puts(" rtsoff="); serial_hex(r.runtime);
        serial_puts(" hcs1="); serial_hex(r.hcs1); serial_puts(" hcs2="); serial_hex(r.hcs2);
        serial_puts(" hcs3="); serial_hex(r.hcs3); serial_puts(" hcc1="); serial_hex(r.hcc1);
    } else {
        if (status == XHCI_UNSUPPORTED) s->unsupported++; else s->invalid++;
        serial_puts(status == XHCI_UNSUPPORTED ? "unsupported: " : "invalid: ");
        serial_puts(r.reason);
        if (r.mapping_error) { serial_puts(" ("); serial_puts(devmap_strerror(r.mapping_error)); serial_puts(")"); }
    }
    serial_puts("\r\n");
    if (status == XHCI_OK) {
        int same = pci_read16(a, PCI_COMMAND) == r.command &&
                   pci_read32(a, 0x10) == r.bar_low &&
                   (r.address_bits != 64 || pci_read32(a, 0x14) == r.bar_high);
        serial_puts(same ? "usb: PCI command and BAR unchanged\r\n" :
                          "usb: WARNING PCI command or BAR changed during inspection\r\n");
    }
    return s->count == XHCI_PROBE_LIMIT;
}
void xhci_boot_probe(const char *cmdline)
{
    if (!boot_option(cmdline, "vos.usb_probe=1")) return;
    struct scan s = {.ops = {0, devmap_maxphys(), config, check_range, map_header}};
    struct pci_addr stopped;
    serial_puts("usb: USB.1 read-only probe; PCI bus 0 only; other buses NOT SCANNED\r\n");
    int limited = pci_scan_bus0(visit, &s, &stopped);
    if (!s.count) serial_puts("usb: not found on bus 0; other PCI topologies unsupported\r\n");
    serial_puts("usb: summary inspected="); serial_dec(s.ok);
    serial_puts(" unsupported="); serial_dec(s.unsupported); serial_puts(" invalid="); serial_dec(s.invalid);
    serial_puts(limited ? "; controller limit reached (scan incomplete)\r\n" : "; bus-0 scan complete\r\n");
    serial_puts("usb: no reset, ownership transfer, DMA, PCI writes or USB transfers\r\n");
}
