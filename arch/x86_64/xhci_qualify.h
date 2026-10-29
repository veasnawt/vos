/* USB.2-C1: opt-in, read-only xHCI qualification of the QEMU 1b36:000d
 * profile (vos.usb2-qualify=1). PCI configuration reads and xHCI register
 * reads only: no PCI write, controller register write, ownership semaphore,
 * halt/reset, DMA, worker or USB enumeration. Not an application interface. */
#ifndef VOS_X86_64_XHCI_QUALIFY_H
#define VOS_X86_64_XHCI_QUALIFY_H
#include <stddef.h>
#include <stdint.h>
#include "usb_resources.h"

/* Filled by vos_xhci_qualify_v1 (Rust); layout asserted on both sides. */
struct xhci_qualify_report {
    const char *reason_text;
    uint64_t base;
    int32_t status;             /* 0 qualified, 1 rejected, -1 integrity failure */
    uint32_t reason, candidates, complete;
    uint32_t vendor, device, class_revision, command_before, command_after, pci_status;
    uint32_t bar_low, bar_high, bar_low_after, bar_high_after, bar_bits, pci_caps;
    uint32_t pm_offset, pmcsr_before, pmcsr_after, power_state;
    uint32_t msi_offset, msi_control_before, msi_control_after;
    uint32_t msix_offset, msix_control_before, msix_control_after, msix_table, msix_pba;
    uint32_t version, caplength, hcs1, hcs2, hcc1, doorbell, runtime, pagesize;
    uint32_t xecp_headers, protocol_ports;
    uint32_t legacy_offset, usblegsup_before, usblegsup_after, usblegctlsts_before, usblegctlsts_after;
    uint32_t usbcmd_before, usbcmd_after, usbsts_before, usbsts_after;
    uint32_t mmio_reads, lease_acquired, lease_released;
    int32_t mapping_error;
    uint32_t usb1_compatible;
};
int32_t vos_xhci_qualify_v1(uint32_t candidates, uint32_t complete, uint32_t physical_bits,
                            struct xhci_qualify_report *out);

/* Both USB paths requested: neither may run. Checked before USB.1 maps. */
int xhci_qualify_conflict(const char *cmdline);
/* 1 if not requested, qualified, or rejected fail-closed with every resource
 * restored; 0 for an integrity failure (state changed, lease retained). */
int xhci_qualify_boot(const char *cmdline);

/* Session-scoped services for the Rust qualifier only. Kernel CR3, IF=0,
 * non-IRQ thread context; DEVMAP_* results. Reads are the only access. */
int xhci_qualify_config32(uint32_t offset, uint32_t *value);
int xhci_qualify_bar_acquire(uint64_t base, struct usb_resource *out);
int xhci_qualify_bar_read32(uint64_t token, uint32_t offset, uint32_t *value);
int xhci_qualify_bar_release(uint64_t token);
void xhci_qualify_bar_retain(uint64_t token);
#endif
