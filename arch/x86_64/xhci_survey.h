/* USB input bring-up, Increment 1: opt-in, read-only survey of the PCI bus tree and every xHCI controller on it
 * (vos.usb-survey=1, or vos.usb-survey=hold to stop afterwards so a physical screen stays readable).
 * PCI configuration reads and xHCI register reads only: no PCI write, no controller register write, no port I/O write,
 * no DMA, no ownership change. Every decision and every line of text lives in Rust (kernel/rust/usb/xhci/survey.rs);
 * this file supplies bounded read adapters and the output hook. Not an application interface. */
#ifndef VOS_X86_64_XHCI_SURVEY_H
#define VOS_X86_64_XHCI_SURVEY_H
#include <stdint.h>

/* Rust: runs the whole survey and prints it. `info`/`info_len` are the Multiboot2 information block (the same on a Legacy BIOS
 * and a UEFI boot), or NULL/0. 0 complete, -1 integrity failure (a lease could not be released or the PCI state of a
 * controller changed). */
int32_t vos_xhci_survey_v2(uint32_t physical_bits, const uint8_t *info, uint32_t info_len);

/* Services for the Rust survey only. Kernel CR3, IF=0, non-IRQ thread context. */
uint32_t xhci_survey_pci32(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off);
/* Lease a read-only window on the memory BAR of a PCI function: `base` must equal the BAR as it reads now; reads are
 * confined to [base, base + limit) with limit at most 64 KiB minus the BAR's offset inside its page (a 1 KiB EHCI BAR is
 * not page aligned). */
int xhci_survey_bar_acquire(uint32_t bus, uint32_t dev, uint32_t fn, uint64_t base, uint32_t limit, uint64_t *token);
/* Lease a 4 KiB read-only window on a DMA-remapping unit's registers: page aligned, inside the chipset MMIO window
 * 0xFE000000-0xFEFFFFFF, never RAM (devmap_check). Offsets are relative to `base`. */
int xhci_survey_mmio_acquire(uint64_t base, uint64_t *token);
/* Copy `len` (at most 16 KiB) bytes of firmware-table memory: only where the memory map says reserved/ACPI memory; below
 * 1 GiB through the kernel's direct map, at or above it through a read-only lease. Never RAM, never a hole. */
int xhci_survey_phys_read(uint64_t pa, uint8_t *out, uint32_t len);
int xhci_survey_bar_read32(uint64_t token, uint32_t off, uint32_t *value);
int xhci_survey_bar_release(uint64_t token);
void xhci_survey_puts(const uint8_t *text, uint32_t len);
uint32_t xhci_survey_i8042_status(void);

struct boot_info;
/* 1 if not requested or completed; 0 for an integrity failure. With vos.usb-survey=hold it does not return. */
int xhci_survey_boot(const struct boot_info *bi);
#endif
