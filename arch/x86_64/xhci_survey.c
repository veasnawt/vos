/* USB input bring-up, Increment 1: platform adapter for the read-only xHCI survey. Every decision lives in Rust.
 * There is no write path here: no PCI configuration write, no port output, no store to a leased BAR window. */
#include "xhci_survey.h"
#include "boot_option.h"
#include "console.h"
#include "cpu.h"
#include "devmap.h"
#include "irq.h"
#include "pci.h"
#include "screen.h"
#include "vmm.h"

static struct { uint64_t token; unsigned active; } survey;

static int survey_context(void)
{
    return !(read_rflags() & RFLAGS_IF) && !irq_in_handler() && (read_cr3() & PTE_ADDR) == vmm_kernel_root();
}

uint32_t xhci_survey_pci32(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off)
{
    if (!survey.active || bus > 255 || dev > 31 || fn > 7 || off > 252 || (off & 3))
        return 0xFFFFFFFFu;
    return pci_read32((struct pci_addr){(uint8_t)bus, (uint8_t)dev, (uint8_t)fn}, off);
}

int xhci_survey_bar_acquire(uint32_t bus, uint32_t dev, uint32_t fn, uint64_t base, uint64_t *token)
{
    if (!token) return DEVMAP_EINVAL;
    *token = 0;
    if (!survey.active || !survey_context() || survey.token || bus > 255 || dev > 31 || fn > 7)
        return DEVMAP_ESTATE;
    /* Bind the lease to the function's BAR as it reads now, never to a caller value alone. */
    struct pci_addr a = {(uint8_t)bus, (uint8_t)dev, (uint8_t)fn};
    uint32_t low = pci_read32(a, 0x10), high = 0;
    if (low & 1) return DEVMAP_EINVAL;
    if (((low >> 1) & 3) == 2) high = pci_read32(a, 0x14);
    else if ((low >> 1) & 3) return DEVMAP_EINVAL;
    if ((((uint64_t)high << 32) | (low & ~15u)) != base) return DEVMAP_ESTATE;
    struct devmap_scope_region r = {DEVMAP_SCOPE_XHCI_SURVEY, base, DEVMAP_XHCI_SURVEY_BYTES};
    uint64_t va;
    int err = devmap_scope_acquire_region(&r, DEVMAP_UC, token, &va);
    if (err) return err;
    survey.token = *token;
    return DEVMAP_OK;
}

int xhci_survey_bar_read32(uint64_t token, uint32_t off, uint32_t *value)
{
    if (!value) return DEVMAP_EINVAL;
    *value = 0;
    uint64_t va, length;
    if (!survey.active || !survey_context() || !token || token != survey.token ||
        devmap_scope_kind() != DEVMAP_SCOPE_XHCI_SURVEY || devmap_scope_extent(token, &va, &length))
        return DEVMAP_ESTATE;
    if ((off & 3) || off >= length) return DEVMAP_EINVAL;
    *value = *(const volatile uint32_t *)(uintptr_t)(va + off);
    return DEVMAP_OK;
}

int xhci_survey_bar_release(uint64_t token)
{
    if (!token || token != survey.token || devmap_scope_kind() != DEVMAP_SCOPE_XHCI_SURVEY)
        return DEVMAP_ESTATE;
    int err = devmap_scope_release(token); /* rechecks context, registry and page tables */
    if (!err) survey.token = 0;
    return err;
}

void xhci_survey_puts(const uint8_t *text, uint32_t len)
{
    for (uint32_t i = 0; text && i < len; i++)
        serial_putc((char)text[i]);
}

/* The legacy keyboard controller's status register: reading it has no side effect (the data port, which would
 * consume a byte, is never touched). */
uint32_t xhci_survey_i8042_status(void)
{
    return inb(0x64);
}

int xhci_survey_boot(const char *cmdline)
{
    int run = boot_option(cmdline, "vos.usb-survey=1"), hold = boot_option(cmdline, "vos.usb-survey=hold");
    if (!run && !hold) return 1;
    if (run && hold) {
        serial_puts("usb: survey: conflicting vos.usb-survey options; not run\r\n");
        return 1;
    }
    if (boot_option(cmdline, "vos.usb_probe=1") || boot_option(cmdline, "vos.usb2-qualify=1") ||
        boot_option(cmdline, "vos.usb2-residue=1") || boot_option(cmdline, "vos.usb2-contain=1") ||
        boot_option(cmdline, "vos.usb2c2-dma=1")) {
        serial_puts("usb: survey is exclusive with the other vos.usb options; not run\r\n");
        return 1;
    }
    if (!survey_context()) {
        serial_puts("usb: survey: wrong execution context; not run\r\n");
        return 1;
    }
    screen_restart_log(); /* a physical machine has no serial port: the report must be readable on the display */
    survey.active = 1;
    int32_t result = vos_xhci_survey_v1(devmap_maxphys());
    survey.active = 0;
    if (result) {
        serial_puts("usb: survey: INTEGRITY FAILURE, the kernel state may be inconsistent\r\n");
        return 0;
    }
    if (hold) {
        serial_puts("usb: survey complete. Nothing was written. The machine is halted: power it off.\r\n");
        for (;;)
            cpu_halt();
    }
    serial_puts("usb: survey complete (boot continues; this text scrolls away, use vos.usb-survey=hold to keep it)\r\n");
    return 1;
}
