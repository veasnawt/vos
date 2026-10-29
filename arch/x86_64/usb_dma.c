/* USB.2-C2 platform adapter. Ownership decisions live in the Rust pool; C
 * supplies PMM frames, CPU zeroing through the kernel RAM alias and release.
 * It zeroes or frees only frames it handed out (allow-list), never publishes
 * an address to a device and touches no PCI or controller register. */
#include "usb_dma.h"
#include "pmm.h"
#include "vmm.h"
#include "cpu.h"
#include "irq.h"
#include "lib.h"
#include "boot_option.h"
#include "console.h"

_Static_assert(sizeof(struct usb_dma_report) == 104 && _Alignof(struct usb_dma_report) == 8, "Rust report ABI");

static struct { uint64_t frame[USB_DMA_FRAMES]; unsigned count; } held;
static struct { int at[3]; unsigned calls[3]; } faults = {{-1, -1, -1}, {0, 0, 0}};

static int usb_dma_context(void)
{
    return !(read_rflags() & RFLAGS_IF) && !irq_in_handler() &&
           (read_cr3() & PTE_ADDR) == vmm_kernel_root();
}
static int injected(unsigned op)
{
    return faults.at[op] >= 0 && faults.calls[op]++ == (unsigned)faults.at[op];
}
static int find(uint64_t frame)
{
    for (unsigned i = 0; i < held.count; i++)
        if (held.frame[i] == frame) return (int)i;
    return -1;
}

void usb_dma_faults(int alloc_at, int zero_at, int free_at)
{
    faults.at[0] = alloc_at; faults.at[1] = zero_at; faults.at[2] = free_at;
    faults.calls[0] = faults.calls[1] = faults.calls[2] = 0;
}
unsigned usb_dma_frames_held(void) { return held.count; }

int usb_dma_frame_alloc(uint64_t *frame)
{
    if (!frame) return USB_DMA_EINVAL;
    *frame = 0;
    if (!usb_dma_context() || held.count == USB_DMA_FRAMES) return USB_DMA_ESTATE;
    if (injected(0)) return USB_DMA_EFAULT;
    uint64_t pa;
    if (pmm_alloc_frame(&pa) != PMM_OK) return USB_DMA_ENOMEM;
    held.frame[held.count++] = pa;
    *frame = pa;
    return 0;
}

int usb_dma_frame_zero(uint64_t frame)
{
    if (!usb_dma_context() || find(frame) < 0) return USB_DMA_ESTATE;
    if (injected(1)) return USB_DMA_EFAULT;
    /* CPU writes through the supervisor RAM alias; no device involvement. */
    volatile uint64_t *p = phys_to_virt(frame);
    for (unsigned i = 0; i < PAGE_SIZE / 8; i++) p[i] = 0;
    for (unsigned i = 0; i < PAGE_SIZE / 8; i++)
        if (p[i]) return USB_DMA_EFAULT;
    return 0;
}

int usb_dma_frame_free(uint64_t frame)
{
    int i = find(frame);
    if (!usb_dma_context() || i < 0) return USB_DMA_ESTATE;
    if (injected(2)) return USB_DMA_EFAULT;
    if (pmm_free_check_in(pmm_kernel(), frame) != PMM_OK || pmm_free_frame(frame) != PMM_OK)
        return USB_DMA_ESTATE;
    held.frame[i] = held.frame[--held.count];
    held.frame[held.count] = 0;
    return 0;
}

static void field(const char *name, uint64_t v)
{
    serial_puts(" "); serial_puts(name); serial_puts("="); serial_hex(v);
}

int usb_dma_boot(const char *cmdline)
{
    if (!boot_option(cmdline, "vos.usb2c2-dma=1")) return 1; /* no effects or output */
    serial_puts("usb2c2: software-only DMA ownership exercise; no device access or publication\r\n");
    uint64_t before = pmm_free_count();
    struct usb_dma_report r;
    int32_t status = vos_usb_dma_exercise_v1(&r);
    uint64_t after = pmm_free_count();
    if (status == -2) { serial_puts("usb2c2: FAIL: interface or repeated run\r\n"); return 0; }
    serial_puts("usb2c2:");
    for (unsigned i = 0; i < 4; i++) { serial_puts(" set_frame="); serial_hex(r.set_frames[i]); }
    field("retained_frame", r.retained_frame);
    serial_puts("\r\nusb2c2:");
    field("allocated", r.allocated); field("prepared", r.prepared); field("released", r.released);
    field("quarantined", r.quarantined); field("active_now", r.active_now);
    field("retained_now", r.retained_now); field("allocation_failures", r.allocation_failures);
    field("rollback_failures", r.rollback_failures); field("violations", r.violations);
    serial_puts("\r\nusb2c2:");
    field("budget_refused", r.budget_refused); field("nondma_refused", r.nondma_refused);
    field("wrong_state_refused", r.wrong_state_refused); field("injected_steps", r.injected_steps);
    field("retained_state", r.retained_state); field("frames_held", usb_dma_frames_held());
    field("pmm_before", before); field("pmm_after", after);
    serial_puts("\r\n");
    /* Exactly the one deliberately quarantined frame stays owned for the boot. */
    int ok = status == 0 && after + 1 == before && usb_dma_frames_held() == 1 &&
             r.retained_now == 1 && r.active_now == 0 && pmm_check_in(pmm_kernel()) == PMM_OK;
    if (!ok) {
        serial_puts("usb2c2: FAIL: stage "); serial_dec((uint64_t)status); serial_puts("\r\n");
        return 0;
    }
    serial_puts("usb2c2: PASS; unpublished pages released exactly; one deliberate quarantine retained "
                "for the boot; no PCI/xHCI access, DMA publication or device ownership transfer\r\n");
    return 1;
}
