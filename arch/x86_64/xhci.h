/* USB.1: read-only, firmware-configured xHCI capability inspection. */
#ifndef VOS_X86_64_XHCI_H
#define VOS_X86_64_XHCI_H
#include <stddef.h>
#include <stdint.h>
#include "pci.h"

#define XHCI_CAP_BYTES 28u
#define XHCI_LAYOUT_LIMIT (64u * 1024u * 1024u)
#define XHCI_PROBE_LIMIT 4u

enum xhci_status { XHCI_NOT_FOUND, XHCI_OK, XHCI_UNSUPPORTED, XHCI_INVALID };
struct xhci_info {
    enum xhci_status status;
    const char *reason;
    uint16_t vendor, device, command, pci_status, version;
    uint32_t class_revision, bar_low, bar_high;
    uint32_t hcs1, hcs2, hcs3, hcc1, doorbell, runtime, layout_end;
    uint64_t base;
    unsigned caplength, slots, interrupters, ports, address_bits;
    int mapping_error;
};
/* No PCI/MMIO write operation is exposed. All calls are bounded. */
struct xhci_probe_ops {
    void *context;
    unsigned physical_bits;
    uint32_t (*config)(void *, struct pci_addr, unsigned);
    int (*check)(void *, uint64_t, uint64_t);
    int (*map)(void *, uint64_t, size_t, const volatile uint32_t **, size_t *);
};
int xhci_class_matches(uint32_t class_revision);
/* Private Rust exports. Decode requires initialized exclusive output and
 * aligned ordinary copied input RAM (>=28 bytes), no MMIO/output aliases.
 * Probe initializes its output. Ops/context/callbacks remain live and may not
 * mutate/alias borrowed ops/output. Not an application-facing pointer API. */
enum xhci_status xhci_decode_caps(const uint32_t *, size_t, struct xhci_info *);
enum xhci_status xhci_probe(struct pci_addr, const struct xhci_probe_ops *, struct xhci_info *);
void xhci_boot_probe(const char *cmdline);
#endif
