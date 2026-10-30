/* USB input bring-up, Increment 1: opt-in, read-only survey of the PCI bus tree and every xHCI controller on it
 * (vos.usb-survey=1, or vos.usb-survey=hold to stop afterwards so a physical screen stays readable).
 * PCI configuration reads and xHCI register reads only: no PCI write, no controller register write, no port I/O write,
 * no DMA, no ownership change. Every decision and every line of text lives in Rust (kernel/rust/usb/xhci/survey.rs);
 * this file supplies bounded read adapters and the output hook. Not an application interface. */
#ifndef VOS_X86_64_XHCI_SURVEY_H
#define VOS_X86_64_XHCI_SURVEY_H
#include <stdint.h>

/* Rust: runs the whole survey and prints it. 0 complete, -1 integrity failure (a lease could not be released or the
 * PCI state of a controller changed). */
int32_t vos_xhci_survey_v1(uint32_t physical_bits);

/* Services for the Rust survey only. Kernel CR3, IF=0, non-IRQ thread context. */
uint32_t xhci_survey_pci32(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off);
int xhci_survey_bar_acquire(uint32_t bus, uint32_t dev, uint32_t fn, uint64_t base, uint64_t *token);
int xhci_survey_bar_read32(uint64_t token, uint32_t off, uint32_t *value);
int xhci_survey_bar_release(uint64_t token);
void xhci_survey_puts(const uint8_t *text, uint32_t len);
uint32_t xhci_survey_i8042_status(void);

/* 1 if not requested or completed; 0 for an integrity failure. With vos.usb-survey=hold it does not return. */
int xhci_survey_boot(const char *cmdline);
#endif
