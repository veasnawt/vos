/* USB input bring-up, Increment 1: platform adapter for the read-only xHCI survey. Every decision lives in Rust.
 * There is no write path here: no PCI configuration write, no port output, no store to a leased BAR window. */
#include "xhci_survey.h"
#include "boot_option.h"
#include "console.h"
#include "cpu.h"
#include "devmap.h"
#include "bootinfo.h"
#include "irq.h"
#include "kmap.h"
#include "memlayout.h"
#include "pci.h"
#include "screen.h"
#include "vmm.h"

#define SURVEY_WINDOW 65536u

static struct { uint64_t token; unsigned active; uint32_t offset, limit; } survey;

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

static int lease(uint64_t base, uint32_t limit, uint64_t *token)
{
    uint64_t page = base & ~(uint64_t)(PAGE_SIZE - 1), off = base - page, va;
    if (!limit || off + limit > SURVEY_WINDOW) return DEVMAP_EINVAL;
    struct devmap_scope_region r = {DEVMAP_SCOPE_XHCI_SURVEY, page, DEVMAP_XHCI_SURVEY_BYTES};
    int err = devmap_scope_acquire_region(&r, DEVMAP_UC, token, &va);
    if (err) return err;
    survey.token = *token;
    survey.offset = (uint32_t)off;
    survey.limit = limit;
    return DEVMAP_OK;
}

int xhci_survey_bar_acquire(uint32_t bus, uint32_t dev, uint32_t fn, uint64_t base, uint32_t limit, uint64_t *token)
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
    return lease(base, limit, token);
}

int xhci_survey_mmio_acquire(uint64_t base, uint64_t *token)
{
    if (!token) return DEVMAP_EINVAL;
    *token = 0;
    if (!survey.active || !survey_context() || survey.token) return DEVMAP_ESTATE;
    if ((base & (PAGE_SIZE - 1)) || base < 0xFE000000ULL || base > 0xFEFFF000ULL) return DEVMAP_EINVAL;
    return lease(base, PAGE_SIZE, token);
}

int xhci_survey_bar_read32(uint64_t token, uint32_t off, uint32_t *value)
{
    if (!value) return DEVMAP_EINVAL;
    *value = 0;
    uint64_t va, length;
    if (!survey.active || !survey_context() || !token || token != survey.token ||
        devmap_scope_kind() != DEVMAP_SCOPE_XHCI_SURVEY || devmap_scope_extent(token, &va, &length))
        return DEVMAP_ESTATE;
    if ((off & 3) || off >= survey.limit || (uint64_t)survey.offset + off + 4 > length) return DEVMAP_EINVAL;
    *value = *(const volatile uint32_t *)(uintptr_t)(va + survey.offset + off);
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

int xhci_survey_phys_read(uint64_t pa, uint8_t *out, uint32_t len)
{
    if (!out || !len || len > DEVMAP_FIRMWARE_MAX_BYTES || pa + len < pa) return DEVMAP_EINVAL;
    if (!survey.active || !survey_context() || survey.token) return DEVMAP_ESTATE;
    if (!devmap_firmware_range_ok(pa, len)) return DEVMAP_ERAM; /* not firmware-table memory: refused */
    if (pa + len <= (1ULL << 30)) {
        for (uint64_t p = pa & ~(uint64_t)(PAGE_SIZE - 1); p < pa + len; p += PAGE_SIZE)
            if (kmap_is_guard(p)) return DEVMAP_EINVAL;
        const volatile uint8_t *src = phys_to_virt(pa);
        for (uint32_t i = 0; i < len; i++) out[i] = src[i];
        return DEVMAP_OK;
    }
    if (pa < (1ULL << 30)) return DEVMAP_EINVAL; /* straddles the end of the direct map */
    uint64_t page = pa & ~(uint64_t)(PAGE_SIZE - 1), off = pa - page, span = (off + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1), token, va;
    struct devmap_scope_region r = {DEVMAP_SCOPE_FIRMWARE_TABLE, page, span};
    int err = devmap_scope_acquire_region(&r, DEVMAP_UC, &token, &va);
    if (err) return err;
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)(va + off);
    for (uint32_t i = 0; i < len; i++) out[i] = src[i];
    return devmap_scope_release(token) ? DEVMAP_ESTATE : DEVMAP_OK;
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

int xhci_survey_boot(const struct boot_info *bi)
{
    const char *cmdline = bi ? bi->cmdline : 0;
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
    const uint8_t *info = 0;
    uint32_t info_len = 0;
    /* The Multiboot2 information block: bootinfo has already required it to be in direct-mapped memory. */
    if (bi->info_start && bi->info_end > bi->info_start && bi->info_end <= (1ULL << 30) && bi->info_end - bi->info_start <= (1u << 20)) {
        info = phys_to_virt(bi->info_start);
        info_len = (uint32_t)(bi->info_end - bi->info_start);
    }
    int32_t result = vos_xhci_survey_v2(devmap_maxphys(), info, info_len);
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
