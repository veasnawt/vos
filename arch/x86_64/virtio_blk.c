#include "virtio_blk.h"
#include "console.h"
#include "lib.h"
#include "memlayout.h"
#include "pmm.h"
#include "vmm.h"

/* PCI identity (virtio 1.2, 4.1.2). */
#define VIRTIO_VENDOR           0x1AF4
#define VIRTIO_ID_BLK_MODERN    0x1042      /* 0x1040 + device type 2 */
#define VIRTIO_ID_BLK_TRANS     0x1001      /* transitional; subsystem ID = device type */
#define VIRTIO_TYPE_BLK         2

/* Capability types and layout (4.1.4). */
#define PCI_CAP_VNDR            0x09
#define CAP_COMMON              1
#define CAP_NOTIFY              2
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
static uint64_t poll_limit = VBLK_POLL_LIMIT, last_spins;
static unsigned skip_notify;
static struct blockdev bdev;

static void barrier(void)
{
    __asm__ volatile ("mfence" : : : "memory");
}

const char *vblk_state_name(int s)
{
    static const char *const n[] = { "absent", "failed", "ready", "broken" };
    return s >= 0 && s <= VBLK_BROKEN ? n[s] : "?";
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
           region_has(&c->device, bar, off, len);
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
                                    type == CAP_DEVICE ? &c->device : NULL;

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
                if ((type == CAP_NOTIFY && off % 2) || (type != CAP_NOTIFY && off % 4))
                    return "virtio structure misaligned";
                if (type == CAP_COMMON && length < COMMON_MIN_LEN)
                    return "common configuration too short";
                if (type == CAP_NOTIFY && length < 2)
                    return "notification structure too short";
                if (type == CAP_DEVICE && length < 8)
                    return "device configuration too short";
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
    bdev.read = 0;
    return state;
}

/* Give up on the device: FAILED status bit (2.1.2), then reset so it stops using the queue. */
static int give_up(const char *why)
{
    status_set(status_get() | S_FAILED);
    device_reset();
    return fail(VBLK_FAILED, why);
}

static int alloc_pool(void)
{
    unsigned n;

    for (n = 0; n < VBLK_POOL_FRAMES; n++)
        if (pmm_alloc_frame(&info.pool[n]) != PMM_OK)
            break;
    if (n == VBLK_POOL_FRAMES) {
        pool_ready = 1;
        return 1;
    }
    while (n)
        pmm_free_frame(info.pool[--n]);
    memset(info.pool, 0, sizeof(info.pool));
    return 0;
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
    if (!device_reset())
        return fail(VBLK_FAILED, "reset did not complete");
    info.resets++;
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
    avail[0] = AVAIL_F_NO_INTERRUPT;
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

    bdev.name = "vblk0";
    bdev.sectors = info.capacity;
    bdev.max_sectors = VBLK_MAX_SECTORS;
    bdev.device_read_only = (info.accepted >> VIRTIO_BLK_F_RO) & 1;
    bdev.read = vblk_read;
    info.why = NULL;
    info.inits++;
    info.state = VBLK_READY;
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
    serial_puts(bdev.device_read_only ? " RO" : "");
    serial_puts(" (offered ");
    serial_hex(info.offered);
    serial_puts("), polled, DMA pool ");
    serial_dec(VBLK_POOL_FRAMES);
    serial_puts(" frames, no IOMMU\r\n");
}

int vblk_init(void)
{
    struct pci_addr a;
    const char *why;

    memset(&info, 0, sizeof(info));
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
    /* Bus mastering for DMA; legacy INTx off (completions are polled). */
    pci_write16(a, PCI_COMMAND, (uint16_t)(pci_read16(a, PCI_COMMAND) | PCI_COMMAND_MASTER | PCI_COMMAND_INTX_OFF));
    if (!alloc_pool()) {
        fail(VBLK_FAILED, "no frames for the DMA pool");
        log_device();
        return VBLK_FAILED;
    }
    bring_up();
    log_device();
    return info.state;
}

int vblk_reinit(void)
{
    if (info.state == VBLK_ABSENT || !pool_ready)
        return info.state;
    bring_up();
    log_device();
    return info.state;
}

const struct vblk_info *vblk_info(void)
{
    return &info;
}

const struct blockdev *vblk_blockdev(void)
{
    return info.state == VBLK_READY ? &bdev : NULL;
}

/* ---- requests ---- */

static int usable(void)
{
    return info.state == VBLK_READY ? BLK_OK : info.state == VBLK_BROKEN ? BLK_EBROKEN : BLK_ENODEV;
}

/* Reset after a timeout or protocol error: the queue is dead, every slot is abandoned. */
static int broken(const char *why, int err)
{
    device_reset();
    memset(inflight, 0, sizeof(inflight));
    memset(done, 0, sizeof(done));
    if (err == BLK_EPROTO)
        info.protocol_errors++;
    fail(VBLK_BROKEN, why);
    serial_puts("vblk: device reset: ");
    serial_puts(why);
    serial_puts("\r\n");
    return err;
}

static int submit_type(uint32_t type, uint64_t lba, uint32_t count, unsigned *slot)
{
    unsigned s;
    int err;

    if ((err = usable()))
        return err;
    if (count == 0 || count > VBLK_MAX_SECTORS)
        return BLK_EINVAL;
    if (lba >= info.capacity || count > info.capacity - lba)
        return BLK_ERANGE;
    for (s = 0; s < VBLK_SLOTS && inflight[s]; s++)
        ;
    if (s == VBLK_SLOTS)
        return BLK_EBUSY;

    uint64_t hdr_pa = info.pool[POOL_HDR] + HDR_OFF(s), st_pa = info.pool[POOL_HDR] + STATUS_OFF(s);
    uint64_t data_pa = info.pool[POOL_DATA + s];
    uint32_t bytes = count * BLK_SECTOR_SIZE;
    if (!vblk_dma_ok(hdr_pa, sizeof(struct req_header)) || !vblk_dma_ok(data_pa, bytes) || !vblk_dma_ok(st_pa, 1))
        return broken("request buffer outside the DMA pool", BLK_EPROTO);

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
    done[s] = 0;
    slot_count[s] = count;
    avail[2 + next_avail % VBLK_QUEUE_SIZE] = (uint16_t)d0;
    barrier();                              /* ring entry and descriptors before idx (2.7.13) */
    avail[1] = ++next_avail;
    barrier();                              /* idx before the notification */
    if (skip_notify)
        skip_notify--;
    else
        notify();
    info.submitted++;
    *slot = s;
    return BLK_OK;
}

int vblk_submit(uint64_t lba, uint32_t count, unsigned *slot)
{
    return submit_type(T_IN, lba, count, slot);
}

/* Collect new used-ring entries; 0, or BLK_EPROTO after resetting the device. */
static int reap(void)
{
    uint16_t idx = used_hdr[1];
    unsigned pending = 0;

    barrier();                              /* idx before the entries it covers */
    for (unsigned s = 0; s < VBLK_SLOTS; s++)
        pending += inflight[s] && !done[s];
    if ((uint16_t)(idx - last_used) > pending)
        return broken("used ring advanced past the outstanding requests", BLK_EPROTO);
    while (last_used != idx) {
        struct vq_used_elem e = { used_ring[last_used % VBLK_QUEUE_SIZE].id,
                                  used_ring[last_used % VBLK_QUEUE_SIZE].len };
        unsigned s = e.id / 3;

        if (e.id >= VBLK_QUEUE_SIZE || e.id % 3 || s >= VBLK_SLOTS || !inflight[s] || done[s])
            return broken("used ring names a descriptor the driver does not own", BLK_EPROTO);
        if (e.len > slot_count[s] * BLK_SECTOR_SIZE + 1)
            return broken("used length larger than the request's buffers", BLK_EPROTO);
        done[s] = 1;
        slot_len[s] = e.len;
        last_used++;
    }
    return BLK_OK;
}

int vblk_wait(unsigned slot, void *buf, uint64_t cap)
{
    uint64_t spins;
    int err;

    if ((err = usable()))
        return err;
    if (slot >= VBLK_SLOTS || !inflight[slot])
        return BLK_EINVAL;
    for (spins = 0; spins < poll_limit; spins++) {
        if ((err = reap()))
            return err;
        if (done[slot])
            break;
        __asm__ volatile ("pause");
    }
    last_spins = spins;
    if (!done[slot]) {
        info.timeouts++;
        return broken("request timed out", BLK_ETIMEDOUT);
    }

    uint8_t st = *(volatile uint8_t *)phys_to_virt(info.pool[POOL_HDR] + STATUS_OFF(slot));
    uint32_t count = slot_count[slot], len = slot_len[slot];
    inflight[slot] = done[slot] = 0;
    info.completed++;
    if (spins > info.max_spins)
        info.max_spins = spins;
    if (st == ST_IOERR) {
        info.ioerr++;
        return BLK_EIO;
    }
    if (st == ST_UNSUPP) {
        info.unsupp++;
        return BLK_EUNSUPP;
    }
    if (st != ST_OK)
        return broken("invalid request status", BLK_EPROTO);
    if (len != count * BLK_SECTOR_SIZE + 1)
        return broken("short completion", BLK_EPROTO);
    info.ok++;
    if (!buf)                               /* vblk_read validated it; tests may drop data */
        return BLK_OK;
    if (cap < (uint64_t)count * BLK_SECTOR_SIZE || !vblk_buffer_ok(buf, (uint64_t)count * BLK_SECTOR_SIZE))
        return BLK_EINVAL;
    memcpy(buf, phys_to_virt(info.pool[POOL_DATA + slot]), count * BLK_SECTOR_SIZE);
    return BLK_OK;
}

int vblk_read(uint64_t lba, uint32_t count, void *buf, uint64_t cap)
{
    unsigned slot;
    int err;

    if ((err = usable()))
        return err;
    if (count == 0 || count > VBLK_MAX_SECTORS || cap < (uint64_t)count * BLK_SECTOR_SIZE ||
        !vblk_buffer_ok(buf, (uint64_t)count * BLK_SECTOR_SIZE))
        return BLK_EINVAL;
    if ((err = vblk_submit(lba, count, &slot)))
        return err;
    return vblk_wait(slot, buf, cap);
}

/* ---- audit ---- */

const char *vblk_audit(void)
{
    if (info.state == VBLK_ABSENT)
        return pool_ready ? "pool allocated without a device" : NULL;
    if (!pool_ready)
        return info.state == VBLK_FAILED ? NULL : "device usable without a DMA pool";
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

void vblk_test_inject_used(uint32_t id, uint32_t len)
{
    if (info.state != VBLK_READY)
        return;
    used_ring[used_hdr[1] % VBLK_QUEUE_SIZE].id = id;
    used_ring[used_hdr[1] % VBLK_QUEUE_SIZE].len = len;
    barrier();
    used_hdr[1] = (uint16_t)(used_hdr[1] + 1);
}

uint8_t vblk_test_device_status(void)
{
    return info.state == VBLK_ABSENT ? 0xFF : status_get();
}

int vblk_test_submit_type(uint32_t type, uint64_t lba, uint32_t count, unsigned *slot)
{
    if (type != VBLK_TEST_T_UNKNOWN)        /* allow-list: never OUT, FLUSH, DISCARD, ... */
        return BLK_EINVAL;
    return submit_type(type, lba, count, slot);
}
#endif
