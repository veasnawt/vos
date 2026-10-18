/*
 * Read-only virtio-blk driver (N3.1), VOS_TEST only.
 *
 * Every test image runs the synthetic part (capability parser, feature and
 * queue-size choice, transport-window and buffer checks on hand-built
 * inputs) and checks the device state it was booted with: images without a
 * disk must find none and refuse every request with VBLK_ENODEV.
 *
 * VOS_TEST_CASE=vblk boots with the disk built by scripts/mkvblkdisk.c
 * attached through virtio-blk-pci, and the same file as the GRUB module
 * "vblk-fixture": every sector read through the driver is compared with the
 * fixture GRUB delivered (an independent path: the boot CD through the
 * firmware). It covers discovery, initialization, known contents, first and
 * last sectors, every sector, range and buffer validation, queue
 * exhaustion and out-of-order completion, an unsupported-request status, a
 * timeout with reset and recovery, forged used-ring entries, repeated reads
 * without leaks, and reads interleaved with running user processes.
 *
 * VOS_TEST_CASE=vblk-ioerr attaches the disk through QEMU's blkdebug driver
 * with an injected read error at sector VBLK_IOERR_SECTOR: the device must
 * report VIRTIO_BLK_S_IOERR for exactly the requests covering it.
 */
#include "selftest.h"
#include "console.h"
#include "irq.h"
#include "kmap.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "thread.h"
#include "channel.h"
#include "virtio_blk.h"
#include "vmm.h"

#if defined(VOS_TEST_CASE_vblk) || defined(VOS_TEST_CASE_vblk_ioerr)
#define VBLK_LIVE 1
#endif
#define VBLK_DISK_SECTORS   2051        /* tests/x86_64/vblk.mk */
#define VBLK_IOERR_SECTOR   1000        /* tests/x86_64/vblk-ioerr.blkdebug */

static int failures, cases;
static const struct boot_info *mbi;

static void expect(int ok, const char *what)
{
    cases++;
    if (ok)
        return;
    failures++;
    serial_puts("selftest: FAIL: ");
    serial_puts(what);
    serial_puts("\r\n");
}

static int streq(const char *a, const char *b)
{
    return a && b && strlen(a) == strlen(b) && memcmp(a, b, strlen(a)) == 0;
}

/* ---- synthetic configuration space ---- */

static uint8_t cfg[256];

static uint8_t syn_rd8(void *ctx, unsigned off)
{
    (void)ctx;
    return off < 256 ? cfg[off] : 0xFF;
}

static uint32_t syn_rd32(void *ctx, unsigned off)
{
    (void)ctx;
    return off <= 252 ? (uint32_t)cfg[off] | (uint32_t)cfg[off + 1] << 8 | (uint32_t)cfg[off + 2] << 16 |
                            (uint32_t)cfg[off + 3] << 24 : 0xFFFFFFFF;
}

static void put32(unsigned off, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        cfg[off + i] = (uint8_t)(v >> (8 * i));
}

static void vcap(unsigned at, unsigned next, unsigned len, unsigned type, unsigned bar, uint32_t off, uint32_t length)
{
    cfg[at] = 0x09;
    cfg[at + 1] = (uint8_t)next;
    cfg[at + 2] = (uint8_t)len;
    cfg[at + 3] = (uint8_t)type;
    cfg[at + 4] = (uint8_t)bar;
    put32(at + 8, off);
    put32(at + 12, length);
}

/* A plausible modern virtio-blk function: common, notify, device, pci-cfg, then an MSI-X capability. */
static void good_cfg(void)
{
    memset(cfg, 0, sizeof(cfg));
    cfg[PCI_STATUS] = PCI_STATUS_CAP_LIST;
    cfg[PCI_CAP_PTR] = 0x40;
    vcap(0x40, 0x54, 16, 1, 4, 0x0000, 0x38);
    vcap(0x54, 0x68, 20, 2, 4, 0x3000, 0x1000);
    put32(0x54 + 16, 4);
    vcap(0x68, 0x7C, 16, 4, 4, 0x2000, 0x100);
    vcap(0x7C, 0x90, 20, 5, 0, 0, 0);
    cfg[0x90] = 0x11;                   /* MSI-X: not vendor-specific, skipped */
    cfg[0x91] = 0;
}

static int parse_says(const char *want)
{
    struct vpci_caps c;
    const char *got = vpci_parse_caps(syn_rd8, syn_rd32, 0, &c);

    if (want ? streq(got, want) : got == NULL)
        return 1;
    serial_puts("selftest: vpci parser said: ");
    serial_puts(got ? got : "accepted");
    serial_puts("\r\n");
    return 0;
}

static void test_parser(void)
{
    struct vpci_caps c;

    good_cfg();
    expect(vpci_parse_caps(syn_rd8, syn_rd32, 0, &c) == NULL && c.common.present && c.common.bar == 4 &&
           c.common.offset == 0 && c.common.length == 0x38 && c.notify.offset == 0x3000 && c.notify_mult == 4 &&
           c.device.offset == 0x2000 && c.cfg_cap == 0x7C && c.seen == 5,
           "parser: modern capability list (common, notify, device, pci-cfg, MSI-X)");
    good_cfg();
    cfg[PCI_STATUS] = 0;
    expect(parse_says("no capability list"), "parser: refuses a function without a capability list");
    good_cfg();
    cfg[PCI_CAP_PTR] = 0x30;
    expect(parse_says("capability pointer outside the device-specific area"),
           "parser: refuses a capability pointer into the standard header");
    good_cfg();
    cfg[0x91] = 0x40;                   /* MSI-X -> back to the first capability */
    expect(parse_says("capability list loops"), "parser: detects a capability loop");
    good_cfg();
    cfg[0x40 + 3] = 7;                  /* unknown cfg_type: ignored, so common is missing */
    expect(parse_says("no common configuration capability"), "parser: ignores an unknown type; common required");
    good_cfg();
    cfg[0x40 + 4] = 6;                  /* reserved BAR value: ignored */
    expect(parse_says("no common configuration capability"), "parser: ignores a capability with a reserved BAR");
    good_cfg();
    cfg[0x68 + 3] = 3;                  /* ISR instead of device configuration */
    expect(parse_says("no device configuration capability"), "parser: device configuration required");
    good_cfg();
    cfg[0x54 + 3] = 3;
    expect(parse_says("no notification capability"), "parser: notification structure required");
    good_cfg();
    cfg[0x7C + 3] = 3;
    expect(parse_says("no PCI configuration access capability"), "parser: PCI configuration access required");
    good_cfg();
    put32(0x40 + 12, 0x37);
    expect(parse_says("common configuration too short"), "parser: refuses a short common configuration");
    good_cfg();
    cfg[0x68 + 2] = 15;
    expect(parse_says("virtio capability too short"), "parser: refuses a truncated capability");
    good_cfg();
    cfg[0x54 + 2] = 16;
    expect(parse_says("virtio capability too short"), "parser: notification capability needs its multiplier");
    good_cfg();
    cfg[0x7C + 2] = 19;
    expect(parse_says("PCI configuration access capability too short"), "parser: pci-cfg capability needs its data");
    good_cfg();
    put32(0x54 + 8, 0x3001);
    expect(parse_says("virtio structure misaligned"), "parser: refuses a misaligned notification structure");
    good_cfg();
    put32(0x68 + 8, 0x2002);
    expect(parse_says("virtio structure misaligned"), "parser: refuses a misaligned device configuration");
    good_cfg();
    put32(0x68 + 8, 0xFFFFFF00);
    expect(parse_says("virtio structure beyond 4 GiB of its BAR"), "parser: refuses offset + length overflow");
    good_cfg();
    vcap(0xA0, 0, 16, 1, 2, 0x100, 0x40);   /* a second common configuration */
    cfg[0x91] = 0xA0;
    expect(vpci_parse_caps(syn_rd8, syn_rd32, 0, &c) == NULL && c.common.bar == 4 && c.common.offset == 0,
           "parser: the first capability of a type wins");

    /* The transport window: only aligned 1/2/4-byte accesses inside a virtio structure. */
    good_cfg();
    vpci_parse_caps(syn_rd8, syn_rd32, 0, &c);
    expect(vblk_window_ok(&c, 4, 0x14, 1) && vblk_window_ok(&c, 4, 0x34, 4) && vblk_window_ok(&c, 4, 0x3000, 2) &&
           vblk_window_ok(&c, 4, 0x2004, 4), "window: accesses inside common, notify and device structures");
    expect(!vblk_window_ok(&c, 4, 0x36, 4) && !vblk_window_ok(&c, 4, 0x38, 4) && !vblk_window_ok(&c, 4, 0x15, 2) &&
           !vblk_window_ok(&c, 4, 0x10, 3) && !vblk_window_ok(&c, 4, 0x10, 8) && !vblk_window_ok(&c, 0, 0x14, 1) &&
           !vblk_window_ok(&c, 4, 0x1000, 4) && !vblk_window_ok(&c, 4, 0x20FE, 4),
           "window: refuses straddling, misaligned, odd-sized, other-BAR and gap accesses");
}

static void test_choices(void)
{
    const uint64_t V1 = 1ULL << VIRTIO_F_VERSION_1, RO = 1ULL << VIRTIO_BLK_F_RO;
    const uint64_t FLUSH = 1ULL << 9, DISCARD = 1ULL << 13, WZ = 1ULL << 14, EVIDX = 1ULL << 29,
                   PACKED = 1ULL << 34, AP = 1ULL << VIRTIO_F_ACCESS_PLATFORM;
    uint64_t acc = 99;
    uint16_t q = 99;

    expect(vblk_choose_features(0, &acc) && acc == 0, "features: refuses a device without VERSION_1");
    expect(vblk_choose_features(RO | FLUSH, &acc) && acc == 0, "features: refuses a legacy-only device");
    expect(!vblk_choose_features(V1, &acc) && acc == V1, "features: VERSION_1 alone");
    expect(!vblk_choose_features(V1 | RO | FLUSH | DISCARD | WZ | EVIDX | PACKED, &acc) && acc == (V1 | RO),
           "features: accepts only VERSION_1 and RO (never FLUSH, DISCARD, WRITE_ZEROES, event index, packed)");
    expect(vblk_choose_features(V1 | AP, &acc) && acc == 0, "features: refuses a device requiring ACCESS_PLATFORM");

    expect(vblk_choose_queue_size(0, &q) && q == 0, "queue: refuses an unavailable queue (size 0)");
    expect(vblk_choose_queue_size(3, &q) && vblk_choose_queue_size(24, &q) && vblk_choose_queue_size(65535, &q),
           "queue: refuses sizes that are not powers of two");
    expect(vblk_choose_queue_size(8, &q) && q == 0, "queue: refuses a queue smaller than the driver needs");
    expect(!vblk_choose_queue_size(16, &q) && q == VBLK_QUEUE_SIZE && !vblk_choose_queue_size(256, &q) &&
           q == VBLK_QUEUE_SIZE && !vblk_choose_queue_size(32768, &q) && q == VBLK_QUEUE_SIZE,
           "queue: uses 16 entries of any larger power-of-two queue");

}

static uint8_t sink[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static const uint8_t konst[64] = { 1 };

static void test_buffers(void)
{
    uint64_t g[KMAP_MAX_GUARDS];
    unsigned ng = kmap_guard_pages(g, KMAP_MAX_GUARDS);

    expect(vblk_buffer_ok(sink, sizeof(sink)) && vblk_buffer_ok(sink + 100, 512),
           "buffer: kernel .bss accepted");
    expect(!vblk_buffer_ok(0, 512) && !vblk_buffer_ok(sink, 0), "buffer: NULL and empty refused");
    expect(!vblk_buffer_ok((void *)0x400000, 512), "buffer: lower-half (user) address refused");
    expect(!vblk_buffer_ok(konst, sizeof(konst)), "buffer: read-only data refused");
    expect(ng > 0 && !vblk_buffer_ok((void *)(g[0] - 256), 512) && !vblk_buffer_ok((void *)g[0], 1),
           "buffer: a range reaching a guard page refused");
    expect(!vblk_buffer_ok((void *)0xFFFFFFFFFFFFFF00ULL, 512), "buffer: wrapping range refused");
}

/*
 * ---- device state of images without a usable disk ----
 * Normally no device is attached (ABSENT). The harness also boots these
 * images with devices the driver must refuse (VIRTIO_F_ACCESS_PLATFORM
 * required, a queue of 8): then the state is FAILED and the log line names
 * the reason, which arch.mk checks.
 */
#ifndef VBLK_LIVE
static void test_absent(void)
{
    const struct vblk_info *in = vblk_info();
    int st = in->state;
    unsigned slot;

    expect((st == VBLK_ABSENT || st == VBLK_FAILED) && in->blk_id == 0 && in->submitted == 0,
           "no usable virtio-blk device (none, or refused at initialization): no block device");
    expect(vblk_read(0, 1, sink, sizeof(sink)) == VBLK_ENODEV && vblk_submit(0, 1, &slot) == VBLK_ENODEV &&
           vblk_wait(0, sink, sizeof(sink)) == VBLK_ENODEV && vblk_reinit() == st && in->submitted == 0,
           "no usable device: every request is refused with VBLK_ENODEV; reinitialization does not change that");
    expect((st == VBLK_FAILED || !vblk_dma_ok(0x200000, 1)) && vblk_audit() == NULL,
           "no usable device: no DMA pool without a device, audit clean");
    if (st == VBLK_FAILED) {
        serial_puts("vblk: refused device: ");
        serial_puts(in->why ? in->why : "?");
        serial_puts("\r\n");
    }
}
#endif

/* ---- the live device ---- */

#ifdef VBLK_LIVE
static const uint8_t *fixture;
static uint64_t fixture_sectors;
static uint8_t buf[VBLK_MAX_SECTORS * BLK_SECTOR_SIZE + 64] __attribute__((aligned(16)));

static const uint8_t *fix(uint64_t lba)
{
    return fixture + lba * BLK_SECTOR_SIZE;
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;

    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* The generator's own header in sector lba, independent of the fixture. */
static int header_ok(const uint8_t *s, uint64_t lba)
{
    return memcmp(s, "VOSBLK01", 8) == 0 && le64(s + 8) == lba && le64(s + 16) == VBLK_DISK_SECTORS;
}

/* Read count sectors at lba and compare with the fixture and the headers. */
static int read_ok(uint64_t lba, uint32_t count)
{
    if (vblk_read(lba, count, buf, sizeof(buf)) != VBLK_OK)
        return 0;
    for (uint32_t i = 0; i < count; i++)
        if (!header_ok(buf + i * BLK_SECTOR_SIZE, lba + i) ||
            memcmp(buf + i * BLK_SECTOR_SIZE, fix(lba + i), BLK_SECTOR_SIZE))
            return 0;
    return 1;
}

static int find_fixture(void)
{
    for (uint32_t i = 0; i < mbi->module_count; i++)
        if (streq(mbi->modules[i].cmdline, "vblk-fixture")) {
            fixture = phys_to_virt(mbi->modules[i].start);
            fixture_sectors = (mbi->modules[i].end - mbi->modules[i].start) / BLK_SECTOR_SIZE;
            return 1;
        }
    return 0;
}

static void test_device(void)
{
    const struct vblk_info *in = vblk_info();
    const uint64_t V1 = 1ULL << VIRTIO_F_VERSION_1, RO = 1ULL << VIRTIO_BLK_F_RO;
    struct blk_desc bd;

    expect(in->state == VBLK_READY && in->blk_id && blk_describe(in->blk_id, &bd) == BLK_OK &&
           bd.state == BLK_DEV_READY && in->pool_held && !in->quarantined,
           "device found, initialized and registered with the block layer");
    expect(in->device_id == 0x1042 || in->device_id == 0x1001, "PCI device ID is modern 0x1042 or transitional 0x1001");
    expect((in->offered & V1) && in->accepted == (V1 | (in->offered & RO)),
           "negotiated exactly VERSION_1 and, if offered, RO");
    expect(in->status == 0x0F, "device status ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK");
    expect(in->queue_size == VBLK_QUEUE_SIZE && in->queue_max >= VBLK_QUEUE_SIZE, "queue 0: 16 entries in use");
    expect(fixture && fixture_sectors == VBLK_DISK_SECTORS && in->capacity == VBLK_DISK_SECTORS &&
           bd.sectors == VBLK_DISK_SECTORS, "capacity equals the fixture: 2051 sectors");
    expect(((bd.caps & BLK_CAP_DEVICE_RO) != 0) == ((in->offered & RO) != 0),
           "read-only capability reflects the device's offer");
    unsigned owned = 0;
    for (unsigned i = 0; i < VBLK_POOL_FRAMES; i++)
        owned += pmm_frame_state_in(pmm_kernel(), in->pool[i]) == PMM_FRAME_ALLOCATED;
    expect(owned == VBLK_POOL_FRAMES && vblk_audit() == NULL, "DMA pool: 6 allocated frames; audit clean");
    expect(vblk_dma_ok(in->pool[0], PAGE_SIZE) && vblk_dma_ok(in->pool[2] + PAGE_SIZE - 1, 1) &&
           !vblk_dma_ok(in->pool[2] + PAGE_SIZE - 1, 2) && !vblk_dma_ok(in->pool[2], 0) &&
           !vblk_dma_ok(in->pool[2], PAGE_SIZE + 1) && !vblk_dma_ok(0x100000, 1) && !vblk_dma_ok(~0ULL, 1),
           "DMA check: inside one pool frame only");
    expect(!vblk_buffer_ok(phys_to_virt(in->pool[2]), 512), "buffer: the DMA pool itself is refused");
}

static void test_contents(void)
{
    const uint64_t n = VBLK_DISK_SECTORS;

    expect(read_ok(0, 1), "sector 0 (first): fixture bytes and header");
    expect(read_ok(1, 1), "sector 1: fixture bytes and header");
    expect(read_ok(n - 1, 1), "last sector (2050): fixture bytes and header");
    expect(read_ok(n - 8, 8) && read_ok(n - 3, 3), "multi-sector reads ending exactly at the capacity");
    expect(read_ok(0, 8) && read_ok(1023, 5), "multi-sector reads at the start and across sector 1024");

    unsigned bad = 0, reqs = 0;
    for (uint64_t lba = 0; lba < n; reqs++) {
        uint32_t c = 1 + reqs % VBLK_MAX_SECTORS;

        if (c > n - lba)
            c = (uint32_t)(n - lba);
#ifdef VOS_TEST_CASE_vblk_ioerr
        /* The chunk covering the injected error must fail; its other sectors are read one by one. */
        if (lba <= VBLK_IOERR_SECTOR && VBLK_IOERR_SECTOR < lba + c) {
            bad += vblk_read(lba, c, buf, sizeof(buf)) != VBLK_EIO;
            for (uint64_t s = lba; s < lba + c; s++)
                bad += s != VBLK_IOERR_SECTOR && !read_ok(s, 1);
            lba += c;
            continue;
        }
#endif
        bad += !read_ok(lba, c);
        lba += c;
    }
    expect(bad == 0, "every sector of the disk equals the GRUB-delivered fixture (chunks of 1-8 sectors)");
}

#ifndef VOS_TEST_CASE_vblk_ioerr
static void test_validation(void)
{
    const struct vblk_info *in = vblk_info();
    const uint64_t n = VBLK_DISK_SECTORS;
    uint64_t sub0 = in->submitted;
    uint64_t g[KMAP_MAX_GUARDS];
    unsigned slot;

    kmap_guard_pages(g, KMAP_MAX_GUARDS);
    memset(buf, 0xA5, sizeof(buf));
    expect(vblk_read(n, 1, buf, sizeof(buf)) == VBLK_ERANGE && vblk_read(n - 1, 2, buf, sizeof(buf)) == VBLK_ERANGE &&
           vblk_read(n - 7, 8, buf, sizeof(buf)) == VBLK_ERANGE && vblk_read(n + 1000, 1, buf, sizeof(buf)) == VBLK_ERANGE,
           "range: the first sector past the end, and requests running past it, refused");
    expect(vblk_read(~0ULL, 1, buf, sizeof(buf)) == VBLK_ERANGE && vblk_read(~0ULL - 1, 3, buf, sizeof(buf)) == VBLK_ERANGE &&
           vblk_submit(~0ULL - 2, 8, &slot) == VBLK_ERANGE, "range: wrapping sector numbers refused");
    expect(vblk_read(0, 0, buf, sizeof(buf)) == VBLK_EINVAL && vblk_read(0, 9, buf, sizeof(buf)) == VBLK_EINVAL &&
           vblk_submit(0, 0, &slot) == VBLK_EINVAL && vblk_submit(0, 9, &slot) == VBLK_EINVAL,
           "length: 0 and more than 8 sectors refused");
    expect(vblk_read(0, 1, buf, 511) == VBLK_EINVAL && vblk_read(0, 8, buf, 4095) == VBLK_EINVAL &&
           vblk_read(0, 1, 0, 512) == VBLK_EINVAL, "buffer: capacity below the request, or NULL, refused");
    expect(vblk_read(0, 1, (void *)0x400000, 512) == VBLK_EINVAL &&
           vblk_read(0, 1, (void *)konst, 512) == VBLK_EINVAL &&
           vblk_read(0, 1, (void *)(g[0] - 256), 512) == VBLK_EINVAL &&
           vblk_read(0, 1, phys_to_virt(in->pool[3]), 512) == VBLK_EINVAL &&
           vblk_read(0, 1, (void *)0xFFFFFFFFFFFFFF00ULL, 512) == VBLK_EINVAL,
           "buffer: user address, read-only data, guard page, DMA pool and wrapping range refused");
    unsigned untouched = 1;
    for (unsigned i = 0; i < sizeof(buf); i++)
        untouched &= buf[i] == 0xA5;
    expect(in->submitted == sub0 && untouched, "refused requests never reached the device or the buffer");
    expect(vblk_wait(VBLK_SLOTS, buf, sizeof(buf)) == VBLK_EINVAL && vblk_wait(0, buf, sizeof(buf)) == VBLK_EINVAL,
           "wait: a slot that is out of range or not in flight refused");
}

static void test_queue(void)
{
    const struct vblk_info *in = vblk_info();
    static const uint64_t lba[VBLK_SLOTS] = { 10, 2045, 700, 1500 };
    static const uint32_t cnt[VBLK_SLOTS] = { 1, 6, 8, 3 };
    static const unsigned order[VBLK_SLOTS] = { 2, 0, 3, 1 };
    unsigned slot[VBLK_SLOTS], extra, ok = 1, distinct = 1;
    uint64_t f0 = pmm_free_count(), c0 = in->completed;

    for (unsigned i = 0; i < VBLK_SLOTS; i++)
        ok &= vblk_submit(lba[i], cnt[i], &slot[i]) == VBLK_OK;
    for (unsigned i = 0; i < VBLK_SLOTS; i++)
        for (unsigned j = 0; j < i; j++)
            distinct &= slot[i] != slot[j];
    expect(ok && distinct, "four requests outstanding at once, in four distinct slots");
    expect(vblk_submit(0, 1, &extra) == VBLK_EBUSY && vblk_read(0, 1, buf, sizeof(buf)) == VBLK_EBUSY,
           "queue exhaustion: a fifth request is refused with VBLK_EBUSY");
    for (unsigned k = 0; k < VBLK_SLOTS; k++) {
        unsigned i = order[k];
        int good = vblk_wait(slot[i], buf, sizeof(buf)) == VBLK_OK;

        for (uint32_t s = 0; s < cnt[i] && good; s++)
            good = memcmp(buf + s * BLK_SECTOR_SIZE, fix(lba[i] + s), BLK_SECTOR_SIZE) == 0;
        ok &= good;
    }
    expect(ok && in->completed == c0 + VBLK_SLOTS, "completions collected in another order than submitted, data exact");
    expect(read_ok(42, 4) && vblk_audit() == NULL && pmm_free_count() == f0,
           "recovery: the queue accepts requests again; audit clean; no frame used");
}

static void test_unsupported(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t sub0 = in->submitted, u0 = in->unsupp;
    unsigned slot;

    expect(vblk_test_submit_type(1, 0, 1, &slot) == VBLK_EINVAL && vblk_test_submit_type(4, 0, 1, &slot) == VBLK_EINVAL &&
           vblk_test_submit_type(11, 0, 1, &slot) == VBLK_EINVAL && vblk_test_submit_type(13, 0, 1, &slot) == VBLK_EINVAL &&
           in->submitted == sub0, "test hook refuses OUT, FLUSH, DISCARD and WRITE_ZEROES (nothing submitted)");
    expect(vblk_test_submit_type(VBLK_TEST_T_UNKNOWN, 0, 1, &slot) == VBLK_OK &&
           vblk_wait(slot, buf, sizeof(buf)) == VBLK_EUNSUPP && in->unsupp == u0 + 1,
           "an unknown request type completes with VIRTIO_BLK_S_UNSUPP -> VBLK_EUNSUPP");
    expect(in->state == VBLK_READY && read_ok(7, 2) && vblk_audit() == NULL,
           "after an unsupported request the device keeps working");
}

/* Reinitialize after a deliberate failure and show the device works again without new frames. */
static int recovered(uint64_t f0)
{
    return vblk_reinit() == VBLK_READY && read_ok(99, 8) && pmm_free_count() == f0 && vblk_audit() == NULL;
}

static void test_timeout(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), t0 = in->timeouts, i0 = in->inits;
    unsigned slot;

    vblk_test_poll_limit(20000);
    vblk_test_skip_notify(1);
    int r = vblk_read(5, 1, buf, sizeof(buf));
    uint64_t spins = vblk_test_last_spins();
    vblk_test_poll_limit(0);
    expect(r == VBLK_ETIMEDOUT && spins == 20000 && in->timeouts == t0 + 1,
           "a request the device never sees times out after exactly the poll bound");
    struct blk_desc bd;
    expect(in->state == VBLK_BROKEN && vblk_test_device_status() == 0 && blk_describe(in->blk_id, &bd) == BLK_OK &&
           bd.state == BLK_DEV_UNAVAILABLE &&
           vblk_read(5, 1, buf, sizeof(buf)) == VBLK_EBROKEN && vblk_submit(5, 1, &slot) == VBLK_EBROKEN,
           "after the timeout the device is reset (status 0) and the driver refuses requests");
    expect(recovered(f0) && in->inits == i0 + 1, "reinitialization recovers it with the same DMA pool");
}

static void test_protocol(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), p0 = in->protocol_errors;
    unsigned slot;

    /* Two completions for one outstanding request (the device is never notified, so nothing races). */
    vblk_test_skip_notify(1);
    expect(vblk_submit(3, 1, &slot) == VBLK_OK && slot == 0, "request 0 outstanding (device not notified)");
    vblk_test_inject_used(0, 513);
    vblk_test_inject_used(3, 513);
    expect(vblk_wait(slot, buf, sizeof(buf)) == VBLK_EPROTO && in->state == VBLK_BROKEN &&
           vblk_test_device_status() == 0 && streq(in->why, "used ring advanced past the outstanding requests"),
           "more completions than outstanding requests: protocol error, device reset (status 0)");
    expect(recovered(f0), "recovered after the surplus completions");

    vblk_test_skip_notify(1);
    vblk_submit(3, 1, &slot);
    vblk_test_inject_used(3, 513);      /* the head of slot 1, which is free */
    expect(vblk_wait(slot, buf, sizeof(buf)) == VBLK_EPROTO &&
           streq(in->why, "used ring names a descriptor the driver does not own"),
           "completion naming a free slot: refused (descriptor ownership)");
    expect(recovered(f0), "recovered after the foreign descriptor");

    vblk_test_skip_notify(1);
    vblk_submit(3, 1, &slot);
    vblk_test_inject_used(1, 513);      /* not a chain head */
    expect(vblk_wait(slot, buf, sizeof(buf)) == VBLK_EPROTO &&
           streq(in->why, "used ring names a descriptor the driver does not own"),
           "completion naming a non-head descriptor: refused");
    expect(recovered(f0), "recovered after the non-head descriptor");

    vblk_test_skip_notify(1);
    vblk_submit(3, 1, &slot);
    vblk_test_inject_used(0, 5000);
    expect(vblk_wait(slot, buf, sizeof(buf)) == VBLK_EPROTO &&
           streq(in->why, "used length larger than the request's buffers"),
           "completion longer than the request's buffers: refused");
    expect(recovered(f0), "recovered after the oversized completion");

    vblk_test_skip_notify(1);
    vblk_submit(3, 1, &slot);
    vblk_test_inject_used(0, 513);      /* right length, but the device never wrote the status */
    expect(vblk_wait(slot, buf, sizeof(buf)) == VBLK_EPROTO && streq(in->why, "invalid request status"),
           "completion without a written status byte: refused");
    expect(recovered(f0), "recovered after the missing status");

    vblk_test_skip_notify(1);
    vblk_submit(3, 1, &slot);
    vblk_test_inject_used(0, 0);        /* nothing written although the status says... nothing */
    expect(vblk_wait(slot, buf, sizeof(buf)) == VBLK_EPROTO, "completion of zero length: refused");
    expect(recovered(f0) && in->protocol_errors == p0 + 6, "recovered; six protocol errors counted");
}

static void test_repeat(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), s0 = in->submitted, c0 = in->completed, x = 12345;
    unsigned bad = 0;

    for (unsigned i = 0; i < 3000; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t c = 1 + (uint32_t)(x >> 60) % VBLK_MAX_SECTORS;
        uint64_t lba = (x >> 20) % (VBLK_DISK_SECTORS - c + 1);

        bad += !read_ok(lba, c);
    }
    expect(bad == 0, "3000 pseudo-random reads of 1-8 sectors all equal the fixture");
    expect(in->submitted == s0 + 3000 && in->completed == c0 + 3000 && pmm_free_count() == f0 &&
           vblk_audit() == NULL, "no frame, slot or descriptor leaked over 3000 reads");
}

/* ---- reads interleaved with running user processes ---- */

static struct process *const P = selftest_procs;
static volatile unsigned reader_done, reader_bad;
static uint8_t rbuf[VBLK_MAX_SECTORS * BLK_SECTOR_SIZE];

static void reader(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 200; i++) {
        uint64_t lba = (i * 37) % (VBLK_DISK_SECTORS - 4);

        if (vblk_read(lba, 4, rbuf, sizeof(rbuf)) != VBLK_OK || memcmp(rbuf, fix(lba), 4 * BLK_SECTOR_SIZE))
            reader_bad++;
        thread_yield();
    }
    reader_done = 1;
}

static const struct boot_module *module(const char *name)
{
    for (uint32_t i = 0; i < mbi->module_count; i++)
        if (streq(mbi->modules[i].cmdline, name))
            return &mbi->modules[i];
    return NULL;
}

static int launch(struct process *p, const char *mod)
{
    const struct boot_module *m = module(mod);
    struct elf_image im;
    int eerr;

    if (!m || load_elf(p, mod, phys_to_virt(m->start), m->end - m->start, &im, &eerr) != PROC_OK)
        return 0;
    if (proc_start(p) == PROC_OK)
        return 1;
    proc_destroy(p);
    return 0;
}

static void test_processes(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), t0 = irq_ticks();
    unsigned threads0 = thread_count();
    int ok;

    proc_test_tick_budget(2000);
    ok = launch(&P[0], "m-memtag1") && launch(&P[1], "m-memtag2") && launch(&P[2], "m-irqregs");
    reader_done = reader_bad = 0;
    ok &= thread_create("vblk-reader", reader, 0) > 0;
    for (unsigned waited = 0; ok && !reader_done && waited < 1500; waited += 10)
        thread_sleep(10);
    thread_sleep(10);                   /* let the programs run once more after the reader */

    uint64_t ticks[3], pre[3], pages[3][8], root[3];
    unsigned npages[3], killed = 0;
    for (unsigned i = 0; i < 3; i++)
        proc_kill(&P[i], "test kill");
    for (unsigned i = 0; i < 3; i++) {
        int st = proc_wait(&P[i]);

        killed += st == PROC_KILLED && streq(P[i].reason, "test kill");
        ticks[i] = P[i].ticks;
        pre[i] = P[i].preemptions;
        root[i] = P[i].root;
        npages[i] = P[i].npages < 8 ? P[i].npages : 8;
        for (unsigned j = 0; j < npages[i]; j++)
            pages[i][j] = P[i].page_pa[j];
        proc_destroy(&P[i]);
    }
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);

    unsigned shared = 0, dma = 0;
    for (unsigned a = 0; a < 3; a++)
        for (unsigned j = 0; j < npages[a]; j++) {
            for (unsigned b = a + 1; b < 3; b++)
                for (unsigned k = 0; k < npages[b]; k++)
                    shared += pages[a][j] == pages[b][k];
            for (unsigned k = 0; k < VBLK_POOL_FRAMES; k++)
                dma += pages[a][j] == in->pool[k];
        }
    expect(ok && reader_done && reader_bad == 0, "a kernel thread read 200 x 4 sectors correctly while programs ran");
    expect(killed == 3 && ticks[0] >= 4 && ticks[1] >= 4 && ticks[2] >= 4 && pre[0] && pre[1] && pre[2],
           "memtag1, memtag2 and irqregs kept running and being preempted between the reads");
    expect(root[0] != root[1] && root[1] != root[2] && root[0] != root[2] && shared == 0 && dma == 0,
           "the programs' address spaces share no frame with each other or with the DMA pool");
    expect(irq_ticks() - t0 >= 20, "the timer kept ticking (at least 20 ticks over the scenario)");
    expect(pmm_free_count() == f0 && proc_count() == 0 && thread_count() == threads0 && thread_canaries_intact(),
           "frames, process records and threads back to baseline");
    const char *sa = sched_audit(), *pa = proc_audit(), *ca = channel_audit(), *va = vblk_audit();
    expect(!sa && !pa && !ca && !va, "scheduler, process, channel and vblk audits clean");
}
#endif /* !VOS_TEST_CASE_vblk_ioerr */
#endif /* VBLK_LIVE */

#ifdef VOS_TEST_CASE_vblk_ioerr
static void test_ioerr(void)
{
    const struct vblk_info *in = vblk_info();
    const uint64_t e = VBLK_IOERR_SECTOR;
    uint64_t f0 = pmm_free_count(), io0 = in->ioerr;
    unsigned again = 0;

    expect(vblk_read(e, 1, buf, sizeof(buf)) == VBLK_EIO && in->ioerr == io0 + 1,
           "the injected media error at sector 1000 is reported as VBLK_EIO");
    expect(in->state == VBLK_READY && read_ok(e - 1, 1) && read_ok(e + 1, 1) && read_ok(e - 8, 8) &&
           read_ok(e + 1, 8), "the device stays ready; the neighbouring sectors read correctly");
    expect(vblk_read(e - 3, 8, buf, sizeof(buf)) == VBLK_EIO && vblk_read(e - 7, 8, buf, sizeof(buf)) == VBLK_EIO,
           "multi-sector requests covering sector 1000 fail with VBLK_EIO");
    for (unsigned i = 0; i < 50; i++)
        again += vblk_read(e, 1, buf, sizeof(buf)) == VBLK_EIO;
    expect(again == 50 && in->ioerr == io0 + 53 && in->state == VBLK_READY,
           "the error is reported every time; the device is never reset for it");
    expect(read_ok(0, 1) && read_ok(VBLK_DISK_SECTORS - 1, 1) && pmm_free_count() == f0 && vblk_audit() == NULL,
           "after 53 errors: reads elsewhere exact, no frame leaked, audit clean");
}
#endif

int selftest_vblk(const struct boot_info *bi)
{
    failures = cases = 0;
    mbi = bi;
    test_parser();
    test_choices();
    test_buffers();
#ifndef VBLK_LIVE
    test_absent();
#else
    expect(find_fixture(), "GRUB delivered the fixture module vblk-fixture");
    if (fixture && vblk_info()->state == VBLK_READY) {
        test_device();
        test_contents();
#ifdef VOS_TEST_CASE_vblk_ioerr
        test_ioerr();
#else
        test_validation();
        test_queue();
        test_unsupported();
        test_timeout();
        test_protocol();
        test_repeat();
        test_processes();
#endif
        expect(vblk_audit() == NULL && vblk_info()->state == VBLK_READY, "final vblk audit clean, device ready");
    } else {
        expect(0, "virtio-blk device ready");
    }
    const struct vblk_info *in = vblk_info();
    serial_puts("vblk: ");
    serial_dec(in->submitted);
    serial_puts(" requests, ");
    serial_dec(in->ok);
    serial_puts(" ok, ");
    serial_dec(in->ioerr);
    serial_puts(" I/O errors, ");
    serial_dec(in->unsupp);
    serial_puts(" unsupported, ");
    serial_dec(in->timeouts);
    serial_puts(" timeouts, ");
    serial_dec(in->protocol_errors);
    serial_puts(" protocol errors, ");
    serial_dec(in->inits);
    serial_puts(" initializations; longest wait ");
    serial_dec(in->max_spins);
    serial_puts(" polls\r\n");
#endif
    serial_puts("selftest: vblk ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
