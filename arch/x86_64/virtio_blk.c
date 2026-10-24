#include "virtio_blk.h"
#include "console.h"
#include "irq.h"
#include "io_scope.h"
#include "lib.h"
#include "memlayout.h"
#include "pmm.h"
#include "thread.h"
#include "vmm.h"
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
#include "storage_stress.h"
#endif

/* PCI identity (virtio 1.2, 4.1.2). */
#define VIRTIO_VENDOR           0x1AF4
#define VIRTIO_ID_BLK_MODERN    0x1042      /* 0x1040 + device type 2 */
#define VIRTIO_ID_BLK_TRANS     0x1001      /* transitional; subsystem ID = device type */
#define VIRTIO_TYPE_BLK         2

/* Capability types and layout (4.1.4). */
#define PCI_CAP_VNDR            0x09
#define CAP_COMMON              1
#define CAP_NOTIFY              2
#define CAP_ISR                 3
#define CAP_DEVICE              4
#define CAP_PCI_CFG             5
#define CAP_MAX_VISITS          48          /* (256 - 64) / 4: more means a loop */

/* Common configuration (4.1.4.3). */
#define C_DFSELECT      0x00
#define C_DF            0x04
#define C_GFSELECT      0x08
#define C_GF            0x0C
#define C_MSIX          0x10
#define C_NUMQ          0x12
#define C_STATUS        0x14
#define C_GENERATION    0x15
#define C_Q_SELECT      0x16
#define C_Q_SIZE        0x18
#define C_Q_MSIX        0x1A
#define C_Q_ENABLE      0x1C
#define C_Q_NOFF        0x1E
#define C_Q_DESC        0x20
#define C_Q_AVAIL       0x28
#define C_Q_USED        0x30
#define COMMON_MIN_LEN  0x38

/* Device status (2.1). */
#define S_ACKNOWLEDGE   1
#define S_DRIVER        2
#define S_DRIVER_OK     4
#define S_FEATURES_OK   8
#define S_NEEDS_RESET   0x40
#define S_FAILED        0x80

/* Split virtqueue (2.7). */
#define DESC_F_NEXT         1
#define DESC_F_WRITE        2
#define AVAIL_F_NO_INTERRUPT 1

/* ISR status (4.1.4.5): reading it returns and clears these bits and deasserts INTx. */
#define ISR_QUEUE       1
#define ISR_CONFIG      2

/* virtio-blk (5.2). */
#define T_IN            0
#define ST_OK           0
#define ST_IOERR        1
#define ST_UNSUPP       2
#define ST_UNWRITTEN    0xFF                /* our marker: the device never wrote the status */

#define MSI_NO_VECTOR   0xFFFF
#define RESET_POLLS     1000000u
#define GENERATION_TRIES 8

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

struct vq_used_elem {
    uint32_t id, len;
};

struct req_header {
    uint32_t type, reserved;
    uint64_t sector;
};

/* Ring frame layout: descriptors, available ring, used ring (all alignment rules met). */
#define RING_DESC_OFF   0
#define RING_AVAIL_OFF  (VBLK_QUEUE_SIZE * 16)
#define RING_USED_OFF   1024
/* Header frame layout: one 16-byte header per slot, then one status byte per slot. */
#define HDR_OFF(i)      ((i) * 16u)
#define STATUS_OFF(i)   (512u + (i))

_Static_assert(RING_AVAIL_OFF + 6 + 2 * VBLK_QUEUE_SIZE <= RING_USED_OFF, "available ring overlaps used ring");
_Static_assert(RING_USED_OFF + 6 + 8 * VBLK_QUEUE_SIZE <= PAGE_SIZE, "used ring exceeds the ring frame");
_Static_assert(3 * VBLK_SLOTS <= VBLK_QUEUE_SIZE, "not enough descriptors for the slots");
_Static_assert(VBLK_MAX_SECTORS * BLK_SECTOR_SIZE <= PAGE_SIZE, "request larger than a data frame");

#define POOL_RING   0
#define POOL_HDR    1
#define POOL_DATA   2       /* + slot */

static struct vblk_info info;
static struct vpci_caps caps;
static uint32_t notify_off;                 /* offset of queue 0's notify register in its BAR */
static int pool_ready;

/* Queue state. */
static volatile struct vq_desc *desc;
static volatile uint16_t *avail;            /* [0] flags, [1] idx, [2..] ring */
static volatile uint16_t *used_hdr;         /* [0] flags, [1] idx */
static volatile struct vq_used_elem *used_ring;
static uint16_t next_avail, last_used;
static int inflight[VBLK_SLOTS], done[VBLK_SLOTS];
static uint32_t slot_count[VBLK_SLOTS], slot_len[VBLK_SLOTS];
static struct thread *slot_owner[VBLK_SLOTS];
static uint64_t slot_ticket[VBLK_SLOTS]; /* scalar only; never a scope or caller-buffer pointer */
/* N3.4: waiters block on their slot's queue; an abandoned slot has no owner and waits for the device. */
static struct wait_queue slot_wq[VBLK_SLOTS];
static int abandoned[VBLK_SLOTS];
static uint64_t abandoned_at[VBLK_SLOTS];
static uint64_t poll_limit = VBLK_POLL_LIMIT, last_spins;
static uint64_t yield_polls = VBLK_YIELD_POLLS, wait_ticks = VBLK_WAIT_TICKS, recheck_ticks = VBLK_RECHECK_TICKS;
static unsigned skip_notify;
static int prefer_poll;

static void barrier(void)
{
    __asm__ volatile ("mfence" : : : "memory");
}

const char *vblk_state_name(int s)
{
    static const char *const n[] = { "absent", "failed", "ready", "broken", "stopped" };
    return s >= 0 && s <= VBLK_STOPPED ? n[s] : "?";
}

/* ---- pure checks ---- */

static int region_has(const struct vpci_region *r, unsigned bar, uint32_t off, unsigned len)
{
    return r->present && r->bar == bar && off >= r->offset && (uint64_t)off + len <= (uint64_t)r->offset + r->length;
}

int vblk_window_ok(const struct vpci_caps *c, unsigned bar, uint32_t off, unsigned len)
{
    if ((len != 1 && len != 2 && len != 4) || off % len)
        return 0;
    return region_has(&c->common, bar, off, len) || region_has(&c->notify, bar, off, len) ||
           region_has(&c->device, bar, off, len) || region_has(&c->isr, bar, off, len);
}

const char *vpci_parse_caps(uint8_t (*rd8)(void *ctx, unsigned off), uint32_t (*rd32)(void *ctx, unsigned off),
                            void *ctx, struct vpci_caps *c)
{
    uint8_t visited[256 / 8] = { 0 };
    unsigned ptr;

    memset(c, 0, sizeof(*c));
    if (!((rd8(ctx, PCI_STATUS) | (unsigned)rd8(ctx, PCI_STATUS + 1) << 8) & PCI_STATUS_CAP_LIST))
        return "no capability list";
    ptr = rd8(ctx, PCI_CAP_PTR) & 0xFC;
    while (ptr) {
        if (ptr < 0x40 || ptr > 0xFC)
            return "capability pointer outside the device-specific area";
        if (visited[ptr / 8] & (1u << (ptr % 8)) || ++c->seen > CAP_MAX_VISITS)
            return "capability list loops";
        visited[ptr / 8] |= (uint8_t)(1u << (ptr % 8));

        unsigned next = rd8(ctx, ptr + 1) & 0xFC;
        if (rd8(ctx, ptr) == PCI_CAP_VNDR) {
            unsigned len = rd8(ctx, ptr + 2), type = rd8(ctx, ptr + 3), bar = rd8(ctx, ptr + 4);
            struct vpci_region *r = type == CAP_COMMON ? &c->common : type == CAP_NOTIFY ? &c->notify :
                                    type == CAP_DEVICE ? &c->device : type == CAP_ISR ? &c->isr : NULL;

            /* Reserved BAR values and unknown types are ignored (4.1.4); the first of each type wins. */
            if (bar <= 5 && type == CAP_PCI_CFG && !c->cfg_cap) {
                if (len < 20 || ptr + 20 > 256)
                    return "PCI configuration access capability too short";
                c->cfg_cap = ptr;
            } else if (bar <= 5 && r && !r->present) {
                uint32_t off, length;

                if (len < 16 || ptr + 16 > 256 || (type == CAP_NOTIFY && (len < 20 || ptr + 20 > 256)))
                    return "virtio capability too short";
                off = rd32(ctx, ptr + 8);
                length = rd32(ctx, ptr + 12);
                if ((uint64_t)off + length > 0xFFFFFFFFULL)
                    return "virtio structure beyond 4 GiB of its BAR";
                if ((type == CAP_NOTIFY && off % 2) || ((type == CAP_COMMON || type == CAP_DEVICE) && off % 4))
                    return "virtio structure misaligned";   /* the ISR status is a single byte */
                if (type == CAP_COMMON && length < COMMON_MIN_LEN)
                    return "common configuration too short";
                if (type == CAP_NOTIFY && length < 2)
                    return "notification structure too short";
                if (type == CAP_DEVICE && length < 8)
                    return "device configuration too short";
                if (type == CAP_ISR && length < 1)
                    return "ISR status structure empty";
                r->present = 1;
                r->bar = (uint8_t)bar;
                r->offset = off;
                r->length = length;
                if (type == CAP_NOTIFY)
                    c->notify_mult = rd32(ctx, ptr + 16);
            }
        }
        ptr = next;
    }
    if (!c->common.present)
        return "no common configuration capability";
    if (!c->notify.present)
        return "no notification capability";
    if (!c->device.present)
        return "no device configuration capability";
    if (!c->cfg_cap)
        return "no PCI configuration access capability";
    return NULL;
}

const char *vblk_choose_features(uint64_t offered, uint64_t *accept)
{
    *accept = 0;
    if (!(offered & (1ULL << VIRTIO_F_VERSION_1)))
        return "VIRTIO_F_VERSION_1 not offered (legacy-only device)";
    if (offered & (1ULL << VIRTIO_F_ACCESS_PLATFORM))
        return "device requires VIRTIO_F_ACCESS_PLATFORM (no IOMMU support)";
    *accept = (1ULL << VIRTIO_F_VERSION_1) | (offered & (1ULL << VIRTIO_BLK_F_RO));
    return NULL;
}

const char *vblk_choose_queue_size(uint16_t device_max, uint16_t *size)
{
    *size = 0;
    if (device_max == 0)
        return "queue 0 unavailable";
    if (device_max & (device_max - 1))
        return "queue size not a power of two";
    if (device_max > 32768)
        return "queue size above 32768";
    if (device_max < VBLK_QUEUE_SIZE)
        return "queue too small";
    *size = VBLK_QUEUE_SIZE;
    return NULL;
}

int vblk_dma_ok(uint64_t pa, uint64_t len)
{
    if (!pool_ready || len == 0)
        return 0;
    for (unsigned i = 0; i < VBLK_POOL_FRAMES; i++)
        if (pa >= info.pool[i] && len <= PAGE_SIZE && pa - info.pool[i] <= PAGE_SIZE - len)
            return 1;
    return 0;
}

int vblk_buffer_ok(const void *buf, uint64_t len)
{
    uint64_t va = (uint64_t)buf;

    if (!buf || len == 0 || va < KERNEL_VMA || va + len - 1 < va)
        return 0;
    for (uint64_t p = va & ~(PAGE_SIZE - 1); p < va + len && p >= (va & ~(PAGE_SIZE - 1)); p += PAGE_SIZE) {
        struct vmm_mapping m;

        if (vmm_lookup(vmm_kernel_root(), p, &m) != VMM_OK || !(m.flags & VMM_WRITE) || (m.flags & VMM_USER))
            return 0;
        for (unsigned i = 0; i < VBLK_POOL_FRAMES && pool_ready; i++)
            if ((m.pa & ~(PAGE_SIZE - 1)) == info.pool[i])
                return 0;
    }
    return 1;
}

/* ---- transport through the PCI configuration access window ---- */

static int win_error;

static void win_select(const struct vpci_region *r, uint32_t off, unsigned len)
{
    pci_write8(info.pci, caps.cfg_cap + 4, r->bar);
    pci_write32(info.pci, caps.cfg_cap + 8, r->offset + off);
    pci_write32(info.pci, caps.cfg_cap + 12, len);
}

static uint32_t rd(const struct vpci_region *r, uint32_t off, unsigned len)
{
    uint32_t v;

    if (!vblk_window_ok(&caps, r->bar, r->offset + off, len) || (uint64_t)off + len > r->length) {
        win_error = 1;
        return 0;
    }
    win_select(r, off, len);
    v = pci_read32(info.pci, caps.cfg_cap + 16);
    return len == 4 ? v : v & ((1u << (8 * len)) - 1);
}

static void wr(const struct vpci_region *r, uint32_t off, unsigned len, uint32_t v)
{
    if (!vblk_window_ok(&caps, r->bar, r->offset + off, len) || (uint64_t)off + len > r->length) {
        win_error = 1;
        return;
    }
    win_select(r, off, len);
    if (len == 1)
        pci_write8(info.pci, caps.cfg_cap + 16, (uint8_t)v);
    else if (len == 2)
        pci_write16(info.pci, caps.cfg_cap + 16, (uint16_t)v);
    else
        pci_write32(info.pci, caps.cfg_cap + 16, v);
}

static uint8_t status_get(void)
{
    return (uint8_t)rd(&caps.common, C_STATUS, 1);
}

static void status_set(uint8_t s)
{
    wr(&caps.common, C_STATUS, 1, s);
}

/* Reset and wait (bounded) until the device reads back 0 (4.1.4.3.2). */
static int device_reset(void)
{
    status_set(0);
    for (unsigned i = 0; i < RESET_POLLS; i++)
        if (status_get() == 0 && !win_error)
            return 1;
    return 0;
}

static void notify(void)
{
    wr(&caps.notify, notify_off - caps.notify.offset, 2, 0);     /* queue index 0 */
}

/* ---- device discovery ---- */

static uint8_t live_rd8(void *ctx, unsigned off)
{
    return pci_read8(*(struct pci_addr *)ctx, off);
}

static uint32_t live_rd32(void *ctx, unsigned off)
{
    return pci_read32(*(struct pci_addr *)ctx, off);
}

static int is_vblk(struct pci_addr a, void *ctx)
{
    uint16_t id = pci_read16(a, PCI_DEVICE_ID);

    (void)ctx;
    if (pci_read16(a, PCI_VENDOR_ID) != VIRTIO_VENDOR)
        return 0;
    return id == VIRTIO_ID_BLK_MODERN ||
           (id == VIRTIO_ID_BLK_TRANS && pci_read16(a, PCI_SUBSYSTEM_ID) == VIRTIO_TYPE_BLK);
}

static int fail(int state, const char *why)
{
    info.state = state;
    info.why = why;
    return state;
}

/* PCI command: bus mastering on or off; legacy INTx enabled only if intx (N3.4: interrupt mode). */
static void pci_cmd(int master, int intx)
{
    uint16_t cmd = pci_read16(info.pci, PCI_COMMAND);

    cmd = master ? (uint16_t)(cmd | PCI_COMMAND_MASTER) : (uint16_t)(cmd & ~PCI_COMMAND_MASTER);
    cmd = intx ? (uint16_t)(cmd & ~PCI_COMMAND_INTX_OFF) : (uint16_t)(cmd | PCI_COMMAND_INTX_OFF);
    pci_write16(info.pci, PCI_COMMAND, cmd);
}

static void bus_master(int on)
{
    pci_cmd(on, 0);
}

/*
 * Free the DMA pool, but only when the device can no longer write it: the
 * caller confirmed a reset (no virtqueue is live, virtio 1.2 3.3.1), and bus
 * mastering is switched off first, so the device cannot start any DMA.
 */
static void release_pool(void)
{
    if (!pool_ready)
        return;
    bus_master(0);
    for (unsigned i = 0; i < VBLK_POOL_FRAMES; i++)
        pmm_free_frame(info.pool[i]);
    memset(info.pool, 0, sizeof(info.pool));
    pool_ready = info.pool_held = 0;
    info.quarantined = 0;
    info.pool_frees++;
    desc = 0;
    avail = used_hdr = 0;
    used_ring = 0;
}

/* Reset; with the reset confirmed the pool is released, otherwise it stays allocated (quarantined). */
static void reset_and_release(void)
{
    if (device_reset())
        release_pool();
    else if (pool_ready)
        info.quarantined = 1;
}

static void drop_irq(const char *why);
static int collect(void);

/* Give up on the device: FAILED status bit (2.1.2), then reset and release the pool. */
static int give_up(const char *why)
{
    status_set(status_get() | S_FAILED);
    reset_and_release();
    drop_irq("device refused");
    return fail(VBLK_FAILED, why);
}

static int alloc_pool(void)
{
    unsigned n;

    for (n = 0; n < VBLK_POOL_FRAMES; n++)
        if (pmm_alloc_frame(&info.pool[n]) != PMM_OK)
            break;
    if (n == VBLK_POOL_FRAMES) {
        pool_ready = info.pool_held = 1;
        info.pool_allocs++;
        bus_master(1);
        return 1;
    }
    while (n)
        pmm_free_frame(info.pool[--n]);
    memset(info.pool, 0, sizeof(info.pool));
    return 0;
}

/* ---- interrupt line (N3.4) ---- */

static int vblk_top(void *ctx);
static void vblk_bottom(void *ctx);

/* Unregister the interrupt handler (if any) and disable INTx; polling mode from now on. */
static void drop_irq(const char *why)
{
    if (info.irq_handle) {
        irq_unregister(info.irq_handle);
        info.irq_handle = 0;
    }
    if (info.mode == VBLK_MODE_IRQ)
        info.mode_switches++;
    info.mode = VBLK_MODE_POLL;
    info.poll_why = why;
    if (info.state != VBLK_ABSENT)
        pci_cmd(pool_ready && !info.quarantined, 0);
}

/*
 * Interrupt mode if everything it needs is there: the ISR status
 * capability, an INTx pin, a line the firmware routed it to (never
 * assumed), level triggered in the chipset, a handler record. Otherwise
 * polling mode, with the reason. A handler registered earlier is kept.
 */
static void choose_mode(void)
{
    const char *why = NULL;
    uint8_t pin = pci_read8(info.pci, PCI_INTERRUPT_PIN), line = pci_read8(info.pci, PCI_INTERRUPT_LINE);
    int r;

    info.irq_pin = pin;
    info.irq_line = line;
    if (prefer_poll)
        why = "polling requested (test)";
    else if (info.irq_handle)
        return;                         /* still registered, line still usable */
    else if (!caps.isr.present)
        why = "no ISR status capability";
    else if (pin < 1 || pin > 4)
        why = "no INTx pin";
    else if (line == 0 || line >= IRQ_LINES || line == IRQ_TIMER || line == 2)
        why = "no usable interrupt line";
    else if (irq_line_disabled(line))
        why = "interrupt line disabled after a storm";
    else if ((r = irq_register(line, IRQ_TRIGGER_LEVEL, "virtio-blk", vblk_top, vblk_bottom, 0, &info.irq_handle)))
        why = r == IRQ_ETRIGGER ? "interrupt line not level triggered (ELCR)" :
              r == IRQ_EFULL ? "no free interrupt handler" : "interrupt registration refused";
    if (why) {
        info.irq_handle = 0;
        drop_irq(why);
        return;
    }
    if (info.mode != VBLK_MODE_IRQ)
        info.mode_switches += info.inits > 0;
    info.mode = VBLK_MODE_IRQ;
    info.poll_why = NULL;
}

/* Steps 1-8 of 3.1.1 on a located device with parsed capabilities and a pool. */
static int bring_up(void)
{
    const char *why;
    uint64_t accept;
    uint16_t qmax, qsize;

    win_error = 0;
    memset(inflight, 0, sizeof(inflight));
    memset(done, 0, sizeof(done));
    memset(abandoned, 0, sizeof(abandoned));
    memset(slot_owner, 0, sizeof(slot_owner));
    if (!device_reset()) {
        if (pool_ready)
            info.quarantined = 1;       /* the device may still be using an earlier queue */
        drop_irq("reset did not complete");
        return fail(VBLK_FAILED, "reset did not complete");
    }
    info.resets++;
    pci_cmd(1, 0);                      /* reset confirmed: DMA allowed again (it may have been off), INTx not yet */
    choose_mode();
    status_set(S_ACKNOWLEDGE);
    status_set(S_ACKNOWLEDGE | S_DRIVER);

    wr(&caps.common, C_DFSELECT, 4, 0);
    info.offered = rd(&caps.common, C_DF, 4);
    wr(&caps.common, C_DFSELECT, 4, 1);
    info.offered |= (uint64_t)rd(&caps.common, C_DF, 4) << 32;
    if ((why = vblk_choose_features(info.offered, &accept)))
        return give_up(why);
    wr(&caps.common, C_GFSELECT, 4, 0);
    wr(&caps.common, C_GF, 4, (uint32_t)accept);
    wr(&caps.common, C_GFSELECT, 4, 1);
    wr(&caps.common, C_GF, 4, (uint32_t)(accept >> 32));
    /* The device's own record of the driver features must be exactly the accepted set. */
    wr(&caps.common, C_GFSELECT, 4, 0);
    uint64_t recorded = rd(&caps.common, C_GF, 4);
    wr(&caps.common, C_GFSELECT, 4, 1);
    recorded |= (uint64_t)rd(&caps.common, C_GF, 4) << 32;
    if (recorded != accept)
        return give_up("device did not record the driver features");
    status_set(S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK);
    if (!(status_get() & S_FEATURES_OK))
        return give_up("device refused the feature set (FEATURES_OK cleared)");
    info.accepted = accept;

    /* Queue 0 (5.2.2: requestq1). */
    if (rd(&caps.common, C_NUMQ, 2) < 1)
        return give_up("no virtqueue");
    wr(&caps.common, C_Q_SELECT, 2, 0);
    qmax = (uint16_t)rd(&caps.common, C_Q_SIZE, 2);
    info.queue_max = qmax;
    if ((why = vblk_choose_queue_size(qmax, &qsize)))
        return give_up(why);
    if (rd(&caps.common, C_Q_ENABLE, 2))
        return give_up("queue already enabled after reset");
    wr(&caps.common, C_Q_SIZE, 2, qsize);
    info.queue_size = qsize;

    uint8_t *ring = phys_to_virt(info.pool[POOL_RING]);
    memset(ring, 0, PAGE_SIZE);
    memset(phys_to_virt(info.pool[POOL_HDR]), 0, PAGE_SIZE);
    desc = (volatile struct vq_desc *)(ring + RING_DESC_OFF);
    avail = (volatile uint16_t *)(ring + RING_AVAIL_OFF);
    used_hdr = (volatile uint16_t *)(ring + RING_USED_OFF);
    used_ring = (volatile struct vq_used_elem *)(ring + RING_USED_OFF + 4);
    avail[0] = info.mode == VBLK_MODE_IRQ ? 0 : AVAIL_F_NO_INTERRUPT;
    next_avail = last_used = 0;

    uint64_t d = info.pool[POOL_RING] + RING_DESC_OFF, a = info.pool[POOL_RING] + RING_AVAIL_OFF,
             u = info.pool[POOL_RING] + RING_USED_OFF;
    wr(&caps.common, C_Q_DESC, 4, (uint32_t)d);
    wr(&caps.common, C_Q_DESC + 4, 4, (uint32_t)(d >> 32));
    wr(&caps.common, C_Q_AVAIL, 4, (uint32_t)a);
    wr(&caps.common, C_Q_AVAIL + 4, 4, (uint32_t)(a >> 32));
    wr(&caps.common, C_Q_USED, 4, (uint32_t)u);
    wr(&caps.common, C_Q_USED + 4, 4, (uint32_t)(u >> 32));
    wr(&caps.common, C_Q_MSIX, 2, MSI_NO_VECTOR);
    wr(&caps.common, C_MSIX, 2, MSI_NO_VECTOR);

    uint64_t noff = (uint64_t)rd(&caps.common, C_Q_NOFF, 2) * caps.notify_mult;
    if (noff > caps.notify.length || caps.notify.length - noff < 2)
        return give_up("queue notification register outside the notification structure");
    notify_off = caps.notify.offset + (uint32_t)noff;
    wr(&caps.common, C_Q_ENABLE, 2, 1);

    /* Capacity: a 64-bit field read as two halves, consistent under config_generation (4.1.4.3). */
    unsigned tries;
    for (tries = 0; tries < GENERATION_TRIES; tries++) {
        uint8_t g = (uint8_t)rd(&caps.common, C_GENERATION, 1);
        uint64_t cap = rd(&caps.device, 0, 4) | (uint64_t)rd(&caps.device, 4, 4) << 32;

        if ((uint8_t)rd(&caps.common, C_GENERATION, 1) == g) {
            info.capacity = cap;
            break;
        }
    }
    if (tries == GENERATION_TRIES)
        return give_up("device configuration kept changing");
    if (info.capacity == 0)
        return give_up("capacity is zero");

    status_set(S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    info.status = status_get();
    if (win_error)
        return give_up("transport access outside the virtio structures");
    if (info.status != (S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK))
        return give_up("device did not accept DRIVER_OK");

    info.why = NULL;
    info.inits++;
    info.state = VBLK_READY;
    pci_cmd(1, info.mode == VBLK_MODE_IRQ);
    return VBLK_READY;
}

static void log_device(void)
{
    serial_puts("vblk: virtio-blk at 00:");
    serial_hex(info.pci.dev);
    serial_puts(".");
    serial_dec(info.pci.fn);
    serial_puts(" id ");
    serial_hex(info.device_id);
    serial_puts(info.device_id == VIRTIO_ID_BLK_MODERN ? " (modern)" : " (transitional, driven as modern)");
    serial_puts(", ");
    serial_puts(vblk_state_name(info.state));
    if (info.state != VBLK_READY) {
        serial_puts(": ");
        serial_puts(info.why ? info.why : "?");
        serial_puts("\r\n");
        return;
    }
    serial_puts(", ");
    serial_dec(info.capacity);
    serial_puts(" sectors, queue ");
    serial_dec(info.queue_size);
    serial_puts(" of ");
    serial_dec(info.queue_max);
    serial_puts(", features VERSION_1");
    serial_puts((info.accepted >> VIRTIO_BLK_F_RO) & 1 ? " RO" : "");
    serial_puts(" (offered ");
    serial_hex(info.offered);
    serial_puts(")");
    if (info.mode == VBLK_MODE_IRQ) {
        serial_puts(", interrupts on IRQ ");
        serial_dec(info.irq_line);
        serial_puts(" (INT");
        serial_putc((char)('A' + info.irq_pin - 1));
        serial_puts(", level)");
    } else {
        serial_puts(", polled (");
        serial_puts(info.poll_why ? info.poll_why : "?");
        serial_puts(")");
    }
    serial_puts(", DMA pool ");
    serial_dec(VBLK_POOL_FRAMES);
    serial_puts(" frames, no IOMMU\r\n");
}

/* ---- block-layer attachment ---- */

int vblk_to_blk(int e)
{
    switch (e) {
    case VBLK_OK:           return BLK_OK;
    case VBLK_EINVAL:       return BLK_EINVAL;
    case VBLK_ERANGE:       return BLK_ERANGE;
    case VBLK_EIO:          return BLK_EIO;
    case VBLK_EUNSUPP:      return BLK_EUNSUPP;
    case VBLK_ETIMEDOUT:    return BLK_ETIMEDOUT;
    case VBLK_EBUSY:        return BLK_EBUSY;
    case VBLK_ENODEV:
    case VBLK_EBROKEN:      return BLK_EUNAVAIL;
    case VBLK_EPROTO:       return BLK_EIO;         /* the device misbehaved; it is now unavailable */
    case VBLK_EINTR:        return BLK_EINTR;
    default:                return BLK_EIO;
    }
}

static void op_info(void *ctx, struct blk_info *o)
{
    (void)ctx;
    o->state = info.state == VBLK_READY ? BLK_DEV_READY : BLK_DEV_UNAVAILABLE;
    o->sectors = info.capacity;
    o->max_sectors = VBLK_MAX_SECTORS;
    o->caps = BLK_CAP_READ | ((info.accepted >> VIRTIO_BLK_F_RO) & 1 ? BLK_CAP_DEVICE_RO : 0);
    o->epoch = info.inits;
}

static int op_read(void *ctx, uint64_t lba, uint32_t count, void *buf, uint64_t cap)
{
    (void)ctx;
    return vblk_to_blk(vblk_read(lba, count, buf, cap));
}

static int op_owns_frame(void *ctx, uint64_t pa)
{
    (void)ctx;
    for (unsigned i = 0; pool_ready && i < VBLK_POOL_FRAMES; i++)
        if (info.pool[i] == pa)
            return 1;
    return 0;
}

static const struct blk_ops vblk_ops = { "virtio-blk", op_info, op_read, op_owns_frame };

/* Register a ready, unregistered device with the block layer. */
static void attach(void)
{
    int err;

    if (info.state != VBLK_READY || info.blk_id)
        return;
    if ((err = blk_register("vblk0", &vblk_ops, 0, &info.blk_id)) != BLK_OK) {
        info.blk_id = 0;
        serial_puts("vblk: block-layer registration refused: ");
        serial_puts(blk_strerror(err));
        serial_puts("\r\n");
    }
}

int vblk_init(void)
{
    struct pci_addr a;
    const char *why;

    memset(&info, 0, sizeof(info));
    info.poll_why = "not initialized";
    if (!pci_scan_bus0(is_vblk, 0, &a)) {
        fail(VBLK_ABSENT, "no virtio-blk device");
        serial_puts("vblk: no virtio-blk device on PCI bus 0; block storage unavailable\r\n");
        return VBLK_ABSENT;
    }
    info.pci = a;
    info.device_id = pci_read16(a, PCI_DEVICE_ID);
    info.revision = pci_read8(a, PCI_REVISION);
    if ((why = vpci_parse_caps(live_rd8, live_rd32, &info.pci, &caps))) {
        fail(VBLK_FAILED, why);
        log_device();
        return VBLK_FAILED;
    }
    /* Bus mastering for DMA (alloc_pool) and legacy INTx off (completions are polled). */
    if (!alloc_pool()) {
        fail(VBLK_FAILED, "no frames for the DMA pool");
        log_device();
        return VBLK_FAILED;
    }
    bring_up();
    log_device();
    attach();
    return info.state;
}

static int outstanding(void)
{
    for (unsigned s = 0; s < VBLK_SLOTS; s++)
        if (inflight[s])
            return 1;
    return 0;
}

int vblk_reinit(void)
{
    if (info.state == VBLK_ABSENT || outstanding())
        return info.state;
    if (info.quarantined) {
        if (!device_reset())
            return info.state;          /* still unconfirmed: the pool stays quarantined */
        info.quarantined = 0;
    }
    if (!pool_ready && !alloc_pool()) {
        fail(VBLK_FAILED, "no frames for the DMA pool");
        log_device();
        return info.state;
    }
    bring_up();
    log_device();
    attach();
    return info.state;
}

int vblk_shutdown(void)
{
    if (info.state == VBLK_ABSENT)
        return VBLK_ENODEV;
    if (outstanding())
        return VBLK_EBUSY;
    if (info.blk_id) {
        if (blk_unregister(info.blk_id) != BLK_OK)
            return VBLK_EBUSY;
        info.blk_id = 0;
    }
    if (!device_reset()) {
        if (pool_ready)
            info.quarantined = 1;
        fail(VBLK_BROKEN, "reset not confirmed at shutdown; DMA pool quarantined");
        drop_irq("shut down");          /* bus mastering off with the pool quarantined */
        return VBLK_EBROKEN;
    }
    release_pool();
    fail(VBLK_STOPPED, "shut down");
    drop_irq("shut down");
    serial_puts("vblk: device shut down, DMA pool released\r\n");
    return VBLK_OK;
}

const struct vblk_info *vblk_info(void)
{
    return &info;
}

/* ---- requests ---- */

static int usable(void)
{
    return info.state == VBLK_READY ? VBLK_OK : info.state == VBLK_BROKEN ? VBLK_EBROKEN : VBLK_ENODEV;
}

/*
 * Reset after a timeout or protocol error (N3.4 order): the device is
 * BROKEN first, so nothing woken below uses it; then it is reset. With the
 * reset unconfirmed, bus mastering goes off and the pool is quarantined.
 * Then every slot (abandoned ones included) is released, since the device
 * no longer owns any buffer, and every waiter is woken: it finds the device
 * broken and returns VBLK_EBROKEN.
 */
static int broken(const char *why, int err)
{
    fail(VBLK_BROKEN, why);
    blk_note_reset(info.blk_id);        /* visible before any waiter resumes; epoch unchanged */
    if (!device_reset()) {
        info.resets_unconfirmed++;
        if (pool_ready)
            info.quarantined = 1;
        pci_cmd(0, 0);
    }
    for (unsigned s = 0; s < VBLK_SLOTS; s++) {
        io_scope_request_end(slot_ticket[s]);
        slot_ticket[s] = 0;
        info.abandoned_reset += abandoned[s];
        inflight[s] = done[s] = abandoned[s] = 0;
        slot_owner[s] = 0;
        thread_wake_all(&slot_wq[s]);
    }
    if (err == VBLK_EPROTO)
        info.protocol_errors++;
    serial_puts("vblk: device reset: ");
    serial_puts(why);
    serial_puts(info.quarantined ? " (reset not confirmed: bus mastering off, DMA pool quarantined)\r\n" : "\r\n");
    return err;
}

static int submit_type(uint32_t type, uint64_t lba, uint32_t count, unsigned *slot)
{
    unsigned s;
    int err;

    if ((err = usable()))
        return err;
    if (count == 0 || count > VBLK_MAX_SECTORS)
        return VBLK_EINVAL;
    if (lba >= info.capacity || count > info.capacity - lba)
        return VBLK_ERANGE;
    for (s = 0; s < VBLK_SLOTS && inflight[s]; s++)
        ;
    if (s == VBLK_SLOTS) {
        if ((err = collect()))          /* completed abandoned requests free their slots here */
            return err;
        for (s = 0; s < VBLK_SLOTS && inflight[s]; s++)
            ;
        if (s == VBLK_SLOTS)
            return VBLK_EBUSY;
    }

    uint64_t hdr_pa = info.pool[POOL_HDR] + HDR_OFF(s), st_pa = info.pool[POOL_HDR] + STATUS_OFF(s);
    uint64_t data_pa = info.pool[POOL_DATA + s];
    uint32_t bytes = count * BLK_SECTOR_SIZE;
    if (!vblk_dma_ok(hdr_pa, sizeof(struct req_header)) || !vblk_dma_ok(data_pa, bytes) || !vblk_dma_ok(st_pa, 1))
        return broken("request buffer outside the DMA pool", VBLK_EPROTO);

    err = io_scope_request_begin(&slot_ticket[s]);
    if (err)
        return err == IO_SCOPE_BUSY ? VBLK_EBUSY : VBLK_EINTR;

    struct req_header *h = phys_to_virt(hdr_pa);
    h->type = type;
    h->reserved = 0;
    h->sector = lba;
    memset(phys_to_virt(data_pa), 0, bytes);
    *(volatile uint8_t *)phys_to_virt(st_pa) = ST_UNWRITTEN;

    unsigned d0 = 3 * s;
    desc[d0].addr = hdr_pa;
    desc[d0].len = sizeof(struct req_header);
    desc[d0].flags = DESC_F_NEXT;
    desc[d0].next = (uint16_t)(d0 + 1);
    desc[d0 + 1].addr = data_pa;
    desc[d0 + 1].len = bytes;
    desc[d0 + 1].flags = DESC_F_NEXT | DESC_F_WRITE;
    desc[d0 + 1].next = (uint16_t)(d0 + 2);
    desc[d0 + 2].addr = st_pa;
    desc[d0 + 2].len = 1;
    desc[d0 + 2].flags = DESC_F_WRITE;
    desc[d0 + 2].next = 0;

    inflight[s] = 1;
    done[s] = abandoned[s] = 0;
    slot_count[s] = count;
    slot_owner[s] = thread_current();
    avail[2 + next_avail % VBLK_QUEUE_SIZE] = (uint16_t)d0;
    barrier();                              /* ring entry and descriptors before idx (2.7.13) */
    avail[1] = ++next_avail;
    barrier();                              /* idx before the notification */
    if (skip_notify)
        skip_notify--;
    else
        notify();
    info.submitted++;
    if (vblk_inflight() > info.max_inflight)
        info.max_inflight = vblk_inflight();
    *slot = s;
    return VBLK_OK;
}

int vblk_submit(uint64_t lba, uint32_t count, unsigned *slot)
{
    return submit_type(T_IN, lba, count, slot);
}

/* Collect new used-ring entries; 0, or VBLK_EPROTO after resetting the device. */
static int reap(void)
{
    uint16_t idx = used_hdr[1];
    unsigned pending = 0;

    barrier();                              /* idx before the entries it covers */
    for (unsigned s = 0; s < VBLK_SLOTS; s++)
        pending += inflight[s] && !done[s];
    if ((uint16_t)(idx - last_used) > pending)
        return broken("used ring advanced past the outstanding requests", VBLK_EPROTO);
    while (last_used != idx) {
        struct vq_used_elem e = { used_ring[last_used % VBLK_QUEUE_SIZE].id,
                                  used_ring[last_used % VBLK_QUEUE_SIZE].len };
        unsigned s = e.id / 3;

        if (e.id >= VBLK_QUEUE_SIZE || e.id % 3 || s >= VBLK_SLOTS || !inflight[s] || done[s])
            return broken("used ring names a descriptor the driver does not own", VBLK_EPROTO);
        if (e.len > slot_count[s] * BLK_SECTOR_SIZE + 1)
            return broken("used length larger than the request's buffers", VBLK_EPROTO);
        done[s] = 1;
        slot_len[s] = e.len;
        last_used++;
    }
    return VBLK_OK;
}

/* An abandoned slot the device completed: its data is discarded and the slot is free again. */
static void retire(unsigned s)
{
    io_scope_request_end(slot_ticket[s]);
    slot_ticket[s] = 0;
    inflight[s] = done[s] = abandoned[s] = 0;
    info.completed++;
    info.abandoned_completed++;
}

/*
 * Reap the used ring, retire completed abandoned slots, wake the owners of
 * completed slots (N3.4). A slot abandoned for VBLK_WAIT_TICKS without
 * completing resets the device. VBLK_OK, VBLK_EPROTO (protocol error) or
 * VBLK_EBROKEN (not usable, or reset here); *woken counts the wakeups.
 */
static int collect_woken(unsigned *woken)
{
    int err;

    if (info.state != VBLK_READY)
        return VBLK_EBROKEN;
    if ((err = reap()))
        return err;
    for (unsigned s = 0; s < VBLK_SLOTS; s++) {
        if (!inflight[s])
            continue;
        if (done[s] && abandoned[s]) {
            retire(s);
        } else if (done[s] && slot_wq[s].head) {
            *woken += (unsigned)thread_wake_all(&slot_wq[s]);
        } else if (abandoned[s] && irq_ticks() - abandoned_at[s] >= wait_ticks) {
            info.timeouts++;
            broken("an abandoned request never completed", VBLK_ETIMEDOUT);
            return VBLK_EBROKEN;
        }
    }
    return VBLK_OK;
}

static int collect(void)
{
    unsigned w = 0;

    return collect_woken(&w);
}

void vblk_service(void)
{
    if (info.state == VBLK_READY && vblk_abandoned()) {
        unsigned w = 0;
        collect_woken(&w);
    }
}

/* The owner gives up its request; the device keeps the slot until it completes or is reset. */
static void abandon(unsigned s)
{
    io_scope_request_abandon(slot_ticket[s]);
    slot_owner[s] = 0;
    abandoned[s] = 1;
    abandoned_at[s] = irq_ticks();
    info.cancelled++;
    if (done[s])
        retire(s);
}

/*
 * Top half: inside the interrupt handler, before the EOI. Reading the ISR
 * status acknowledges the device (it deasserts INTx); 0 means another
 * device on the shared line interrupted.
 */
static int vblk_top(void *ctx)
{
    uint8_t isr;

    (void)ctx;
    if (!caps.isr.present || info.state == VBLK_ABSENT || info.state == VBLK_STOPPED)
        return IRQ_NONE;
    isr = (uint8_t)rd(&caps.isr, 0, 1);
    if (!(isr & (ISR_QUEUE | ISR_CONFIG))) {
        info.irq_none++;
        return IRQ_NONE;
    }
    info.irqs++;
    if (info.state != VBLK_READY)
        info.irq_late++;
    return IRQ_HANDLED;
}

/* Bottom half: thread context. Falls back to polling if the line was disabled by a storm. */
static void vblk_bottom(void *ctx)
{
    unsigned w = 0;

    (void)ctx;
    if (info.mode == VBLK_MODE_IRQ && irq_line_disabled(info.irq_line)) {
        drop_irq("interrupt line disabled after a storm");
        if (info.state == VBLK_READY) {
            pci_cmd(1, 0);
            avail[0] = AVAIL_F_NO_INTERRUPT;
        }
        serial_puts("vblk: interrupt line disabled after a storm; polling from now on\r\n");
        for (unsigned s = 0; s < VBLK_SLOTS; s++)
            thread_wake_all(&slot_wq[s]);   /* blocked waiters continue by polling */
        return;
    }
    if (info.state != VBLK_READY)
        return;                         /* a late interrupt after a reset: nothing of the new queue is affected */
    collect_woken(&w);
    info.irq_wakeups += w;
}

/* Polling mode (the N3.2 wait): poll, yielding every yield_polls polls, within the poll and tick bounds. */
static int wait_poll(unsigned slot, struct thread *me, uint64_t inherited_deadline)
{
    uint64_t spins, epoch = info.inits, t0 = irq_ticks();
    uint64_t deadline = io_scope_deadline(t0 + wait_ticks);
    uint64_t limit = poll_limit < VBLK_POLL_LIMIT ? poll_limit : VBLK_POLL_LIMIT;
    int err, ticks_over = 0;

    if (inherited_deadline && inherited_deadline < deadline)
        deadline = inherited_deadline;
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
    storage_stress_event(STRESS_POLL, slot);
#endif
    for (spins = 0; spins < limit; spins++) {
        if ((err = collect()))
            return err;
        if (done[slot])
            break;
        if (irq_ticks() >= deadline) {
            ticks_over = 1;
            break;
        }
        if (yield_polls && spins % yield_polls == yield_polls - 1) {
            /*
             * Let other threads (and through them user programs) run. They
             * may complete this slot in their own reap, or reset the device;
             * either way the slot is re-checked before it is used.
             */
            info.yields++;
            thread_yield();
            if (info.state != VBLK_READY || info.inits != epoch || !inflight[slot] || slot_owner[slot] != me)
                return VBLK_EBROKEN;
            if (done[slot])
                break;
            io_scope_check();           /* remember cancellation; finish the bounded inner poll safely */
            if (irq_ticks() >= deadline) {
                ticks_over = 1;
                break;
            }
        }
        __asm__ volatile ("pause");
    }
    last_spins = spins;
    if (!done[slot]) {
        info.timeouts++;
        info.tick_timeouts += ticks_over;
        io_scope_stop(IO_SCOPE_TIMEOUT);
        return broken(ticks_over ? "request timed out (tick bound)" : "request timed out", VBLK_ETIMEDOUT);
    }
    if (spins > info.max_spins)
        info.max_spins = spins;
    return VBLK_OK;
}

/*
 * Interrupt mode: block on the slot's queue until the bottom half wakes us,
 * re-checking every recheck_ticks, until the deadline. Nothing can run
 * between the check of done[slot] and the block (IF = 0, no preemption),
 * and bottom halves only run in other threads' scheduler entries, so a
 * completion is never missed: it either is seen by the check or wakes the
 * blocked waiter.
 */
static int wait_irq(unsigned slot, struct thread *me)
{
    uint64_t epoch = info.inits, deadline = io_scope_deadline(irq_ticks() + wait_ticks);
    int err, r = THREAD_WOKEN;

    for (;;) {
        if ((err = collect()))
            return err;
        if (done[slot]) {
            info.recheck_completions += r == THREAD_TIMEDOUT;
            return VBLK_OK;
        }
        if (info.mode != VBLK_MODE_IRQ)
            return wait_poll(slot, me, deadline); /* fallback cannot extend this deadline */
        uint64_t now = irq_ticks();
        if (now >= deadline) {
            info.timeouts++;
            info.tick_timeouts++;
            io_scope_stop(IO_SCOPE_TIMEOUT);
            return broken("request timed out (deadline)", VBLK_ETIMEDOUT);
        }
        info.blocked_waits++;
        r = io_scope_wait(&slot_wq[slot], deadline - now > recheck_ticks ? now + recheck_ticks : deadline);
        if (info.state != VBLK_READY || info.inits != epoch || !inflight[slot] || slot_owner[slot] != me)
            return VBLK_EBROKEN;
        if (r == THREAD_INTERRUPTED) {
            if (io_scope_check() == IO_SCOPE_TIMEOUT) {
                info.timeouts++;
                info.tick_timeouts++;
                return broken("request timed out (scope deadline)", VBLK_ETIMEDOUT);
            }
            abandon(slot);
            return VBLK_EINTR;
        }
    }
}

int vblk_wait(unsigned slot, void *buf, uint64_t cap)
{
    struct thread *me = thread_current();
    int err;

    if ((err = usable()))
        return err;
    if (slot >= VBLK_SLOTS || !inflight[slot] || slot_owner[slot] != me)
        return VBLK_EINVAL;
    if ((err = info.mode == VBLK_MODE_IRQ ? wait_irq(slot, me) : wait_poll(slot, me, 0)))
        return err;
#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_storage_stress) || defined(VOS_TEST_CASE_storage_stress_b) || defined(VOS_TEST_CASE_storage_stress_c) || defined(VOS_TEST_CASE_storage_stress_d))
    storage_stress_event(STRESS_COMPLETE, slot);
#endif

    uint8_t st = *(volatile uint8_t *)phys_to_virt(info.pool[POOL_HDR] + STATUS_OFF(slot));
    uint32_t count = slot_count[slot], len = slot_len[slot];
    inflight[slot] = done[slot] = 0;
    slot_owner[slot] = 0;
    io_scope_request_end(slot_ticket[slot]);
    slot_ticket[slot] = 0;
    info.completed++;
    if (st == ST_IOERR) {
        info.ioerr++;
        return VBLK_EIO;
    }
    if (st == ST_UNSUPP) {
        info.unsupp++;
        return VBLK_EUNSUPP;
    }
    if (st != ST_OK)
        return broken("invalid request status", VBLK_EPROTO);
    if (len != count * BLK_SECTOR_SIZE + 1)
        return broken("short completion", VBLK_EPROTO);
    info.ok++;
    if (io_scope_check())
        return io_scope_check() == IO_SCOPE_TIMEOUT ? VBLK_ETIMEDOUT : VBLK_EINTR;
    if (!buf)                               /* vblk_read validated it; tests may drop data */
        return VBLK_OK;
    if (cap < (uint64_t)count * BLK_SECTOR_SIZE || !vblk_buffer_ok(buf, (uint64_t)count * BLK_SECTOR_SIZE))
        return VBLK_EINVAL;
    memcpy(buf, phys_to_virt(info.pool[POOL_DATA + slot]), count * BLK_SECTOR_SIZE);
    return VBLK_OK;
}

int vblk_read(uint64_t lba, uint32_t count, void *buf, uint64_t cap)
{
    unsigned slot;
    int err;

    if ((err = usable()))
        return err;
    if (count == 0 || count > VBLK_MAX_SECTORS || cap < (uint64_t)count * BLK_SECTOR_SIZE ||
        !vblk_buffer_ok(buf, (uint64_t)count * BLK_SECTOR_SIZE))
        return VBLK_EINVAL;
    if ((err = vblk_submit(lba, count, &slot)))
        return err;
    return vblk_wait(slot, buf, cap);
}

unsigned vblk_inflight(void)
{
    unsigned n = 0;

    for (unsigned s = 0; s < VBLK_SLOTS; s++)
        n += inflight[s] != 0;
    return n;
}

unsigned vblk_abandoned(void)
{
    unsigned n = 0;

    for (unsigned s = 0; s < VBLK_SLOTS; s++)
        n += abandoned[s] != 0;
    return n;
}

/* ---- audit ---- */

/* N3.4: slot ownership, waiters and the interrupt configuration. */
static const char *audit_slots(void)
{
    for (unsigned s = 0; s < VBLK_SLOTS; s++) {
        if ((slot_ticket[s] && !inflight[s]) ||
            !io_scope_request_valid(slot_ticket[s], slot_owner[s] ? slot_owner[s]->id : 0, abandoned[s]))
            return "invalid storage scope request reference";
        if (abandoned[s] && (!inflight[s] || slot_owner[s]))
            return "an abandoned slot is not in flight or still has an owner";
        if (slot_owner[s] && !inflight[s])
            return "a free slot has an owner";
        if (inflight[s] && !abandoned[s] && !slot_owner[s])
            return "a slot in flight has neither an owner nor was it abandoned";
        if (slot_wq[s].head && (!inflight[s] || abandoned[s] || slot_wq[s].head != slot_owner[s] ||
                                slot_wq[s].head->next))
            return "a slot's wait queue holds something other than its owner";
        if (done[s] && !inflight[s])
            return "a free slot is marked complete";
    }
    if (info.state != VBLK_READY && vblk_inflight())
        return "requests in flight on a device that is not ready";
    if (info.mode == VBLK_MODE_IRQ && (!info.irq_handle || info.poll_why))
        return "interrupt mode without a registered handler";
    if (info.mode == VBLK_MODE_POLL && (info.irq_handle || !info.poll_why))
        return "polling mode with a handler, or without a reason";
    if (info.state == VBLK_READY) {
        uint16_t cmd = pci_read16(info.pci, PCI_COMMAND);
        int intx = !(cmd & PCI_COMMAND_INTX_OFF), quiet = avail[0] & AVAIL_F_NO_INTERRUPT;

        if (!(cmd & PCI_COMMAND_MASTER))
            return "ready device without bus mastering";
        if (info.mode == VBLK_MODE_IRQ ? (!intx || quiet) : (intx || !quiet))
            return "INTx or interrupt suppression does not match the completion mode";
    }
    return NULL;
}

const char *vblk_audit(void)
{
    const char *a;

    if ((a = io_scope_audit()) || (a = audit_slots()))
        return a;
    if (info.pool_held != pool_ready)
        return "pool bookkeeping inconsistent";
    if (info.state == VBLK_ABSENT)
        return pool_ready ? "pool allocated without a device" : NULL;
    if (info.state == VBLK_READY && !info.blk_id)
        return "ready device not registered";
    if ((info.state == VBLK_STOPPED || info.state == VBLK_ABSENT) && info.blk_id)
        return "released device still registered";
    if (info.quarantined && !pool_ready)
        return "quarantine without a pool";
    if (!pool_ready) {
        for (unsigned i = 0; i < VBLK_POOL_FRAMES; i++)
            if (info.pool[i])
                return "freed pool still listed";
        return info.state == VBLK_READY ? "device usable without a DMA pool" : NULL;
    }
    if (info.state == VBLK_FAILED || info.state == VBLK_STOPPED)
        if (!info.quarantined)
            return "pool kept by a released device without quarantine";
    for (unsigned i = 0; i < VBLK_POOL_FRAMES; i++) {
        if (pmm_frame_state_in(pmm_kernel(), info.pool[i]) != PMM_FRAME_ALLOCATED)
            return "DMA pool frame not allocated";
        for (unsigned j = 0; j < i; j++)
            if (info.pool[i] == info.pool[j])
                return "DMA pool frame listed twice";
    }
    if (info.state != VBLK_READY)
        return NULL;
    for (unsigned d = 0; d < VBLK_QUEUE_SIZE; d++)
        if (desc[d].addr && !vblk_dma_ok(desc[d].addr, desc[d].len))
            return "descriptor outside the DMA pool";
    unsigned busy = 0;
    for (unsigned s = 0; s < VBLK_SLOTS; s++)
        busy += inflight[s];
    if (!busy && (avail[1] != next_avail || used_hdr[1] != last_used || next_avail != last_used))
        return "idle queue with unmatched ring indexes";
    if (status_get() != (S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK))
        return "device status changed";
    if (win_error)
        return "transport access outside the virtio structures";
    return NULL;
}

#ifdef VOS_TEST
void vblk_test_skip_notify(unsigned n)
{
    skip_notify = n;
}

void vblk_test_poll_limit(uint64_t polls)
{
    poll_limit = polls ? polls : VBLK_POLL_LIMIT;
}

uint64_t vblk_test_last_spins(void)
{
    return last_spins;
}

void vblk_test_yield_polls(uint64_t n)
{
    yield_polls = n;
}

void vblk_test_wait_ticks(uint64_t ticks)
{
    wait_ticks = ticks ? ticks : VBLK_WAIT_TICKS;
}

void vblk_test_inject_used(uint32_t id, uint32_t len)
{
    if (info.state != VBLK_READY)
        return;
    used_ring[used_hdr[1] % VBLK_QUEUE_SIZE].id = id;
    used_ring[used_hdr[1] % VBLK_QUEUE_SIZE].len = len;
    barrier();
    used_hdr[1] = (uint16_t)(used_hdr[1] + 1);
}

void vblk_test_prefer_poll(int on)
{
    prefer_poll = on;
}

void vblk_test_break(void)
{
    if (info.state == VBLK_READY)
        broken("test reset", VBLK_EBROKEN);
}

void vblk_test_recheck_ticks(uint64_t ticks)
{
    recheck_ticks = ticks ? ticks : VBLK_RECHECK_TICKS;
}

uint16_t vblk_test_used_idx(void)
{
    return info.state == VBLK_READY ? used_hdr[1] : 0;
}

void vblk_test_notify(void)
{
    if (info.state == VBLK_READY)
        notify();
}

uint8_t vblk_test_device_status(void)
{
    return info.state == VBLK_ABSENT ? 0xFF : status_get();
}

int vblk_test_submit_type(uint32_t type, uint64_t lba, uint32_t count, unsigned *slot)
{
    if (type != VBLK_TEST_T_UNKNOWN)        /* allow-list: never OUT, FLUSH, DISCARD, ... */
        return VBLK_EINVAL;
    return submit_type(type, lba, count, slot);
}
#endif

#ifdef VOS_TEST
#include "storage_observe.h"

/* Only the observer's input view is filtered; real desc/pool/request state is untouched. */
static unsigned observe_table_unavailable;

void vblk_test_observe_hide_table(int hide)
{
    observe_table_unavailable = hide != 0;
}

void vblk_test_observe(struct observe_driver *out)
{
    const volatile struct vq_desc *table = observe_table_unavailable ? NULL : desc;
    out->state = info.state;
    out->mode = info.mode;
    out->pool_held = info.pool_held;
    out->quarantined = info.quarantined;
    memcpy(out->pool, info.pool, sizeof(out->pool));
    out->submitted = info.submitted;
    out->completed = info.completed;
    out->cancelled = info.cancelled;
    out->retired_completion = info.abandoned_completed;
    out->retired_reset = info.abandoned_reset;
    out->resets = info.resets;
    out->next_avail = next_avail;
    out->last_used = last_used;
    for (unsigned i = 0; i < VBLK_SLOTS; i++)
        out->slots[i] = (struct observe_slot){(uint64_t)slot_owner[i],
            slot_owner[i] ? slot_owner[i]->id : 0, slot_ticket[i],
            (uint64_t)slot_wq[i].head, (uint64_t)slot_wq[i].tail, abandoned_at[i],
            inflight[i], done[i], abandoned[i], slot_count[i], slot_len[i]};
    /* Pool allocation precedes table initialization; early failures may retain it.
     * desc is assigned only after ring initialization and cleared on release.
     * A retained initialized table remains readable even if the device is not READY.
     */
    out->descriptors_available = pool_ready && info.pool_held && table &&
        pmm_frame_state_in(pmm_kernel(), info.pool[POOL_RING]) == PMM_FRAME_ALLOCATED &&
        table == (volatile struct vq_desc *)((uint8_t *)phys_to_virt(info.pool[POOL_RING]) + RING_DESC_OFF);
    memset(out->descriptors, 0, sizeof(out->descriptors));
    if (out->descriptors_available)
        for (unsigned i = 0; i < VBLK_QUEUE_SIZE; i++) {
            out->descriptors[i].address = table[i].addr;
            out->descriptors[i].length = table[i].len;
            out->descriptors[i].flags = table[i].flags;
            out->descriptors[i].next = table[i].next;
        }
}
#endif

#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_vblk) || defined(VOS_TEST_CASE_scope_orphan_idle) || defined(VOS_TEST_CASE_file) || defined(VOS_TEST_CASE_ext2))
#include "panic.h"

/*
 * Held publication (N3 final D1 repair; these test images only). A suppressed notification does not keep a request
 * from the device: QEMU kicks the queue when DRIVER_OK attaches its host notifier and then takes whatever the
 * available ring holds. While held, submissions fill a private copy of the available ring instead, so the index the
 * device reads does not move and the request is not exposed (virtio 1.2, 2.7.13.3), whatever kicks arrive. A reset
 * ends the hold (bring_up points avail at the ring again); vblk_test_publish_held() exposes the held entries in ring
 * order, before a test notifies the device.
 */
static uint16_t held_ring[2 + VBLK_QUEUE_SIZE];
static volatile uint16_t *held_real;

void vblk_test_hold_publish(void)
{
    if (info.state != VBLK_READY || !avail || avail == held_ring)
        panic("vblk test: publication hold needs a ready, unheld queue");
    for (unsigned i = 0; i < 2 + VBLK_QUEUE_SIZE; i++)
        held_ring[i] = avail[i];
    held_real = avail;
    avail = held_ring;
}

void vblk_test_publish_held(void)
{
    if (avail == held_ring && info.state == VBLK_READY) {
        for (uint16_t i = held_real[1]; i != held_ring[1]; i++)
            held_real[2 + i % VBLK_QUEUE_SIZE] = held_ring[2 + i % VBLK_QUEUE_SIZE];
        held_real[0] = held_ring[0];
        barrier();                          /* ring entries before idx (2.7.13) */
        held_real[1] = held_ring[1];
        barrier();
        avail = held_real;
    }
    held_real = 0;
}
#endif
