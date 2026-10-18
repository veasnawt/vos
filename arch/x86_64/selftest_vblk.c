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
 * N3.4: the same image then checks interrupt mode (the line read from PCI
 * configuration, ELCR, masks, one interrupt per request, blocked waiters
 * woken by the bottom half), a completion that arrives before its waiter
 * blocks, a deadline timeout beside a CPU-bound program, cancellation
 * (interrupted waiters, completion after cancellation into the kept DMA
 * frame, an abandoned request that never completes), interrupted block-
 * layer waiters, concurrent requests, resets with completions and
 * interrupts pending, the shared line (a second handler, an interrupt
 * virtio-blk did not raise, a storm and the fallback to polling), and the
 * CPU left to a program while reads wait; then it runs the N3.1/N3.2
 * tests again in polling mode, the fallback.
 *
 * VOS_TEST_CASE=vblk-ioerr attaches the disk through QEMU's blkdebug driver
 * with an injected read error at sector VBLK_IOERR_SECTOR: the device must
 * report VIRTIO_BLK_S_IOERR for exactly the requests covering it.
 */
#include "selftest.h"
#include "console.h"
#include "cpu.h"
#include "irq.h"
#include "kmap.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "thread.h"
#include "channel.h"
#include "pci.h"
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
    cfg[0x7C + 3] = 7;
    expect(parse_says("no PCI configuration access capability"), "parser: PCI configuration access required");
    /* N3.4: the ISR status capability (optional; interrupt mode needs it). */
    good_cfg();
    expect(vpci_parse_caps(syn_rd8, syn_rd32, 0, &c) == NULL && !c.isr.present,
           "parser: no ISR status capability is accepted (polling mode only)");
    vcap(0xA0, 0, 16, 3, 4, 0x1001, 1);    /* one byte, at an odd offset */
    cfg[0x91] = 0xA0;
    expect(vpci_parse_caps(syn_rd8, syn_rd32, 0, &c) == NULL && c.isr.present && c.isr.bar == 4 &&
           c.isr.offset == 0x1001 && c.isr.length == 1 && vblk_window_ok(&c, 4, 0x1001, 1) &&
           !vblk_window_ok(&c, 4, 0x1002, 1) && !vblk_window_ok(&c, 4, 0x1000, 2),
           "parser: ISR status capability located; the window allows exactly its byte");
    put32(0xA0 + 12, 0);
    expect(parse_says("ISR status structure empty"), "parser: refuses an empty ISR status structure");
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

    /* Driver codes never reach the block layer unmapped (N3.2). */
    expect(vblk_to_blk(VBLK_OK) == BLK_OK && vblk_to_blk(VBLK_EINVAL) == BLK_EINVAL &&
           vblk_to_blk(VBLK_ERANGE) == BLK_ERANGE && vblk_to_blk(VBLK_EIO) == BLK_EIO &&
           vblk_to_blk(VBLK_EUNSUPP) == BLK_EUNSUPP && vblk_to_blk(VBLK_ETIMEDOUT) == BLK_ETIMEDOUT &&
           vblk_to_blk(VBLK_EBUSY) == BLK_EBUSY && vblk_to_blk(VBLK_ENODEV) == BLK_EUNAVAIL &&
           vblk_to_blk(VBLK_EBROKEN) == BLK_EUNAVAIL && vblk_to_blk(VBLK_EPROTO) == BLK_EIO &&
           vblk_to_blk(99) == BLK_EIO, "driver results map to device-independent block errors");
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
#if !defined(VBLK_LIVE) && !defined(VOS_TEST_CASE_ext2)
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
        /* N3.2: a refused device keeps no DMA frames once its reset is confirmed. */
        uint64_t f = pmm_free_count(), a0 = in->pool_allocs;
        unsigned same = 1;
        for (unsigned i = 0; i < 10; i++)
            same &= vblk_reinit() == VBLK_FAILED && pmm_free_count() == f && !in->pool_held;
        expect(!in->pool_held && !in->quarantined && in->pool[0] == 0 && in->pool_allocs == in->pool_frees &&
               in->pool_allocs == a0 + 10 && same,
               "refused device: its DMA pool is freed after the confirmed reset, also over 10 retries");
        expect(vblk_test_device_status() == 0 && !(pci_read16(in->pci, PCI_COMMAND) & PCI_COMMAND_MASTER),
               "refused device: reset (status 0) and bus mastering switched off");
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
/*
 * N3.2: a thread waits (and yields) while another thread breaks the device.
 * The waiter's slot is abandoned by the reset; when it runs again it must
 * notice and return VBLK_EBROKEN instead of polling a slot that is no longer
 * its own.
 */
static volatile int waiter_result, waiter_done;
static uint8_t wbuf[BLK_SECTOR_SIZE];

static void waiter(void *arg)
{
    (void)arg;
    waiter_result = vblk_read(21, 1, wbuf, sizeof(wbuf));
    waiter_done = 1;
}

static void test_reset_while_waiting(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), sub0 = in->submitted;
    unsigned slot, threads0 = thread_count();

    vblk_test_yield_polls(64);
    vblk_test_poll_limit(3000000);
    waiter_done = 0;
    waiter_result = -1;
    vblk_test_skip_notify(2);           /* neither request reaches the device */
    int ok = thread_create("vblk-waiter", waiter, 0) > 0;
    thread_yield();                     /* the waiter submits, polls 64 times and yields back */
    ok &= !waiter_done && in->submitted == sub0 + 1;
    ok &= vblk_submit(22, 1, &slot) == VBLK_OK && slot != 0;
    vblk_test_inject_used(3 * 3, 513);  /* the head of slot 3, which nobody owns */
    int mine = vblk_wait(slot, buf, sizeof(buf));
    for (unsigned i = 0; !waiter_done && i < 1000; i++)
        thread_yield();
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    vblk_test_poll_limit(0);
    expect(ok && mine == VBLK_EPROTO && in->state == VBLK_BROKEN,
           "this thread's wait detects a forged completion and resets the device");
    expect(waiter_done && waiter_result == VBLK_EBROKEN,
           "the yielded waiter notices the reset and returns VBLK_EBROKEN (it does not poll on)");
    thread_yield();                     /* let the waiter be reaped */
    expect(vblk_reinit() == VBLK_READY && read_ok(21, 1) && pmm_free_count() == f0 &&
           thread_count() == threads0 && vblk_audit() == NULL, "recovered; no frame or thread left over");
}

/*
 * N3.2 responsiveness: a request the device never completes while a CPU-bound
 * program is runnable. Without yielding (the N3.1 behaviour) the kernel polls
 * with IF = 0 and the program gets no CPU at all for the whole wait; with
 * cooperative waiting it keeps running, and the wait ends at the tick bound.
 */
static void test_responsive(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), pt, it, y0, tt0;
    unsigned threads0 = thread_count();
    int ok, r1, r2;

    proc_test_tick_budget(5000);
    ok = launch(&P[0], "m-memtag1");

    vblk_test_yield_polls(0);
    vblk_test_poll_limit(3000000);
    vblk_test_skip_notify(1);
    pt = P[0].ticks;
    it = irq_ticks();
    r1 = vblk_read(11, 1, buf, sizeof(buf));
    uint64_t prog1 = P[0].ticks - pt, tick1 = irq_ticks() - it;
    ok &= vblk_reinit() == VBLK_READY;

    vblk_test_yield_polls(256);
    vblk_test_poll_limit(40000);        /* about 156 yields: the 30-tick bound must end the wait first */
    vblk_test_wait_ticks(30);
    y0 = in->yields;
    tt0 = in->tick_timeouts;
    vblk_test_skip_notify(1);
    pt = P[0].ticks;
    it = irq_ticks();
    r2 = vblk_read(11, 1, buf, sizeof(buf));
    uint64_t prog2 = P[0].ticks - pt, tick2 = irq_ticks() - it;
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    vblk_test_poll_limit(0);
    vblk_test_wait_ticks(0);
    ok &= vblk_reinit() == VBLK_READY;

    proc_kill(&P[0], "test kill");
    ok &= proc_wait(&P[0]) == PROC_KILLED;
    proc_destroy(&P[0]);
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    serial_puts("vblk: hung request beside a CPU-bound program: without yielding the program ran ");
    serial_dec(prog1);
    serial_puts(" of ");
    serial_dec(tick1);
    serial_puts(" ticks; with yielding ");
    serial_dec(prog2);
    serial_puts(" of ");
    serial_dec(tick2);
    serial_puts(" ticks (");
    serial_dec(in->yields - y0);
    serial_puts(" yields)\r\n");
    expect(ok && r1 == VBLK_ETIMEDOUT && prog1 == 0 && tick1 == 0,
           "without yielding, a hung request gives a runnable program no CPU (0 ticks during the wait)");
    expect(r2 == VBLK_ETIMEDOUT && in->tick_timeouts == tt0 + 1 && tick2 >= 30 && prog2 >= 20 && in->yields > y0,
           "with cooperative waiting the program keeps running, and the wait ends at the 30-tick bound");
    expect(read_ok(64, 8) && pmm_free_count() == f0 && thread_count() == threads0 && proc_count() == 0 &&
           vblk_audit() == NULL, "afterwards: device recovered, no frame, thread or process left over");
}

/* ---- N3.4: interrupt-driven completion, cancellation, resets, shared lines ---- */

/* Reinitialize into interrupt (irq = 1) or polling mode. */
static int set_mode(int irq)
{
    vblk_test_prefer_poll(!irq);
    return vblk_reinit() == VBLK_READY && vblk_info()->mode == (irq ? VBLK_MODE_IRQ : VBLK_MODE_POLL) &&
           vblk_audit() == NULL;
}

static int intx_asserted(struct pci_addr a)
{
    return (pci_read16(a, PCI_STATUS) & PCI_STATUS_INTX) != 0;
}

static int wait_blocked(int64_t id)
{
    for (unsigned k = 0; k < 20 && id > 0 && thread_state((uint64_t)id) != THREAD_BLOCKED; k++)
        thread_yield();
    return id > 0 && thread_state((uint64_t)id) == THREAD_BLOCKED;
}

static int wait_free(int64_t id, unsigned ticks)
{
    for (unsigned k = 0; k < ticks && thread_state((uint64_t)id) != THREAD_FREE; k++)
        thread_sleep(1);
    return thread_state((uint64_t)id) == THREAD_FREE;
}

static void test_irq_mode(void)
{
    const struct vblk_info *in = vblk_info();
    const struct irq_stats *is = irq_stats();
    const struct sched_stats *ss = sched_stats();
    uint8_t line = pci_read8(in->pci, PCI_INTERRUPT_LINE), pin = pci_read8(in->pci, PCI_INTERRUPT_PIN);
    uint16_t masks = irq_chip()->masks(), cmd = pci_read16(in->pci, PCI_COMMAND);

    expect(in->mode == VBLK_MODE_IRQ && in->irq_handle && !in->poll_why && in->irq_line == line &&
           in->irq_pin == pin && pin >= 1 && pin <= 4 && line < IRQ_LINES,
           "interrupt mode, on the line the firmware routed INTx to (PCI interrupt line register)");
    expect(line < IRQ_LINES && ((irq_chip()->triggers() >> line) & 1), "that line is level triggered in the ELCR");
    expect(line < IRQ_LINES && !(masks & (1u << line)) && (line < 8 || !(masks & (1u << 2))) &&
           masks == irq_expected_masks() && irq_audit() == NULL,
           "its 8259 line, and the cascade for a slave line, unmasked; masks match the handler table");
    expect(!(cmd & PCI_COMMAND_INTX_OFF) && (cmd & PCI_COMMAND_MASTER) && vblk_audit() == NULL,
           "PCI INTx enabled and bus mastering on; driver audit clean");

    /* 64 reads with nothing else runnable: the waiter blocks, the idle thread halts, the interrupt wakes it. */
    uint64_t i0 = in->irqs, w0 = in->irq_wakeups, b0 = in->blocked_waits, r0 = in->recheck_completions;
    uint64_t h0 = ss->idle_halts, t0 = irq_ticks(), c0 = line < IRQ_LINES ? is->claimed[line] : 0, to0 = in->timeouts;
    unsigned bad = 0;
    for (unsigned i = 0; i < 64; i++)
        bad += !read_ok((i * 97) % (VBLK_DISK_SECTORS - 8), 8);
    thread_sleep(1);                    /* the last interrupt may still be pending */
    uint64_t irqs = in->irqs - i0, woke = in->irq_wakeups - w0, rech = in->recheck_completions - r0;
    serial_puts("vblk: 64 reads in interrupt mode: ");
    serial_dec(irqs);
    serial_puts(" interrupts, ");
    serial_dec(woke);
    serial_puts(" waiters woken by the bottom half, ");
    serial_dec(in->blocked_waits - b0);
    serial_puts(" blocks, ");
    serial_dec(rech);
    serial_puts(" completions found by re-check, idle halted ");
    serial_dec(ss->idle_halts - h0);
    serial_puts(" times, ");
    serial_dec(irq_ticks() - t0);
    serial_puts(" ticks\r\n");
    expect(bad == 0 && in->timeouts == to0, "64 reads in interrupt mode, all exact, no timeout");
    /*
     * A request that completes before its waiter blocks shares its interrupt
     * with the next one (the ISR bit stays set until read), so sequential
     * reads raise between 32 and 64 interrupts.
     */
    expect(irqs >= 32 && irqs <= 64 && line < IRQ_LINES && is->claimed[line] - c0 == irqs,
           "interrupts delivered (32-64 for 64 reads) and each claimed by the virtio-blk top half");
    expect(woke >= 1 && rech <= 3 && ss->idle_halts - h0 >= 1,
           "blocked waiters were woken by the bottom half (at most 3 by re-check); the CPU halted meanwhile");
}

/* A completion before its waiter blocks: it must be found by the check, never waited for. */
static void test_early_completion(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t b0 = in->blocked_waits, i0 = in->irqs, t0;
    uint16_t u0 = vblk_test_used_idx();
    unsigned slot, spins = 0;
    int ok = vblk_submit(77, 4, &slot) == VBLK_OK, r, asserted;

    while (ok && vblk_test_used_idx() == u0 && ++spins < 100000000)
        __asm__ volatile ("pause");     /* IF = 0: the interrupt cannot be delivered meanwhile */
    asserted = intx_asserted(in->pci);
    t0 = irq_ticks();
    r = vblk_wait(slot, buf, sizeof(buf));
    expect(ok && vblk_test_used_idx() != u0 && asserted && r == VBLK_OK && !memcmp(buf, fix(77), 4 * BLK_SECTOR_SIZE) &&
           in->blocked_waits == b0 && irq_ticks() == t0 && in->irqs == i0,
           "completed before its waiter blocked (INTx asserted, not delivered): found by the check, no block");
    thread_sleep(2);
    expect(in->irqs == i0 + 1 && !intx_asserted(in->pci) && vblk_audit() == NULL && irq_audit() == NULL,
           "its interrupt is delivered later, claimed and acknowledged (INTx drops); nothing to wake, nothing broken");
}

static void test_irq_timeout(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), pt, t0, b0 = in->blocked_waits, tt0 = in->tick_timeouts, prog, el;
    int ok, r;

    proc_test_tick_budget(5000);
    ok = launch(&P[0], "m-memtag1");
    vblk_test_wait_ticks(30);
    vblk_test_skip_notify(1);
    pt = P[0].ticks;
    t0 = irq_ticks();
    r = vblk_read(11, 1, buf, sizeof(buf));
    prog = P[0].ticks - pt;
    el = irq_ticks() - t0;
    vblk_test_wait_ticks(0);
    proc_kill(&P[0], "test kill");
    ok &= proc_wait(&P[0]) == PROC_KILLED;
    proc_destroy(&P[0]);
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    serial_puts("vblk: hung request in interrupt mode beside a CPU-bound program: the program ran ");
    serial_dec(prog);
    serial_puts(" of ");
    serial_dec(el);
    serial_puts(" ticks; the waiter blocked ");
    serial_dec(in->blocked_waits - b0);
    serial_puts(" times\r\n");
    expect(ok && r == VBLK_ETIMEDOUT && el >= 30 && el <= 40 && in->tick_timeouts == tt0 + 1 &&
           in->state == VBLK_BROKEN && vblk_test_device_status() == 0,
           "a request the device never sees times out at its 30-tick deadline; the device is reset");
    expect(prog + 5 >= el && in->blocked_waits - b0 >= 3 && in->blocked_waits - b0 <= 5,
           "meanwhile the program kept the CPU: the waiter blocked, re-checking every 10 ticks, instead of polling");
    expect(set_mode(1) && read_ok(64, 8) && pmm_free_count() == f0, "recovered in interrupt mode, no frame used");
}

/* ---- cancellation ---- */

static volatile int cw_result, cw_done;
static volatile unsigned cw_slot;
static uint8_t cbuf[VBLK_MAX_SECTORS * BLK_SECTOR_SIZE];

static void cancel_waiter(void *arg)
{
    unsigned s = VBLK_SLOTS;

    cw_result = vblk_submit((uint64_t)(uintptr_t)arg, 8, &s);
    cw_slot = s;
    if (cw_result == VBLK_OK)
        cw_result = vblk_wait(s, cbuf, sizeof(cbuf));
    cw_done = 1;
}

/* Start a waiter whose request the device does not see yet; then interrupt it and let it exit. */
static int64_t start_and_cancel(uint64_t lba)
{
    int64_t id;

    cw_done = 0;
    cw_result = -1;
    vblk_test_skip_notify(1);
    id = thread_create("vblk-cancel", cancel_waiter, (void *)(uintptr_t)lba);
    if (!wait_blocked(id) || thread_interrupt((uint64_t)id) != 1)
        return -1;
    return wait_free(id, 10) ? id : -1;
}

static int frame_is(uint64_t pa, const void *want)
{
    return memcmp(phys_to_virt(pa), want, PAGE_SIZE) == 0;
}

static uint8_t zeros[PAGE_SIZE];

static void test_cancel(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), c0 = in->cancelled, a0 = in->abandoned_completed, ar0 = in->abandoned_reset;
    unsigned threads0 = thread_count();

    /* Interrupted while the device has not seen the request: the slot is abandoned, the frames kept. */
    int64_t id = start_and_cancel(300);
    unsigned s = cw_slot;
    uint64_t data_pa = s < VBLK_SLOTS ? in->pool[2 + s] : 0;
    int untouched = data_pa && frame_is(data_pa, zeros);
    expect(id > 0 && cw_done && cw_result == VBLK_EINTR && vblk_abandoned() == 1 && vblk_inflight() == 1 &&
           in->cancelled == c0 + 1 && pmm_frame_state_in(pmm_kernel(), data_pa) == PMM_FRAME_ALLOCATED &&
           vblk_audit() == NULL, "an interrupted waiter returns VBLK_EINTR and exits; its slot stays in flight, abandoned");

    /* The device completes it after the cancellation, into the kept frame. */
    vblk_test_notify();
    thread_sleep(3);
    expect(untouched && frame_is(data_pa, fix(300)) && vblk_abandoned() == 0 && vblk_inflight() == 0 &&
           in->abandoned_completed == a0 + 1,
           "the device wrote the data into the kept DMA frame after the cancellation; the bottom half retired the slot");
    expect(read_ok(300, 8) && read_ok(1, 1) && vblk_audit() == NULL && pmm_free_count() == f0 &&
           thread_count() == threads0, "the slot is reused correctly; no frame or thread left over");

    /* An abandoned request that never completes resets the device once it is older than the bound. */
    vblk_test_wait_ticks(20);
    id = start_and_cancel(400);
    int abandoned_ok = id > 0 && cw_result == VBLK_EINTR && vblk_abandoned() == 1;
    thread_sleep(25);
    vblk_test_skip_notify(1);           /* the next request is not notified either: the device never sees them */
    int r = vblk_read(5, 1, buf, sizeof(buf));
    vblk_test_wait_ticks(0);
    expect(abandoned_ok && r == VBLK_EBROKEN && in->state == VBLK_BROKEN &&
           streq(in->why, "an abandoned request never completed") && in->abandoned_reset == ar0 + 1 &&
           vblk_inflight() == 0 && vblk_test_device_status() == 0,
           "an abandoned request still outstanding after the bound resets the device, which releases its slot");
    expect(set_mode(1) && read_ok(400, 8) && pmm_free_count() == f0 && vblk_audit() == NULL,
           "recovered; the same DMA pool, no frame leaked");
}

/* Interrupted threads in the block layer: one loading a cache line, one waiting for that load. */
static volatile int bl_result[2], bl_done[2];
static uint8_t bbuf[2][BLK_SECTOR_SIZE];

static void blk_waiter(void *arg)
{
    unsigned i = (unsigned)(uintptr_t)arg;

    bl_result[i] = blk_read(vblk_info()->blk_id, 1600 + i, 1, bbuf[i], sizeof(bbuf[i]));
    bl_done[i] = 1;
}

static void test_blk_cancel(void)
{
    const struct vblk_info *in = vblk_info();
    blk_id_t id = in->blk_id;
    uint64_t f0 = pmm_free_count(), lw0 = blk_stats()->load_waits;
    int64_t t[2];

    blk_invalidate(id);
    bl_done[0] = bl_done[1] = 0;
    vblk_test_skip_notify(1);
    t[0] = thread_create("blk-loader", blk_waiter, (void *)0);
    int a = wait_blocked(t[0]) && blk_test_line_state(id, 200) == BLK_LINE_LOADING;
    t[1] = thread_create("blk-follower", blk_waiter, (void *)1);
    int b = wait_blocked(t[1]) && blk_stats()->load_waits == lw0 + 1;
    thread_interrupt((uint64_t)t[1]);
    int fb = wait_free(t[1], 10) && bl_result[1] == BLK_EINTR && blk_test_line_state(id, 200) == BLK_LINE_LOADING;
    thread_interrupt((uint64_t)t[0]);
    int fa = wait_free(t[0], 10) && bl_result[0] == BLK_EINTR && blk_test_line_state(id, 200) == BLK_LINE_FREE;
    expect(a && b && fb && fa && vblk_abandoned() == 1 && blk_audit() == NULL,
           "block layer: a thread waiting for another's line load and the loader itself are interrupted: BLK_EINTR, line left FREE");
    vblk_test_notify();
    thread_sleep(3);
    uint8_t two[2 * BLK_SECTOR_SIZE];
    expect(vblk_abandoned() == 0 && blk_read(id, 1600, 2, two, sizeof(two)) == BLK_OK && !memcmp(two, fix(1600), sizeof(two)) &&
           blk_test_line_state(id, 200) == BLK_LINE_VALID && blk_audit() == NULL && vblk_audit() == NULL &&
           pmm_free_count() == f0, "the abandoned load completes and is retired; the line then loads and is exact");
    blk_invalidate(id);                 /* the block-layer tests start from an empty cache */
}

/* ---- concurrent requests ---- */

static volatile unsigned cc_bad, cc_done;

static void cc_reader(void *arg)
{
    unsigned k = (unsigned)(uintptr_t)arg;

    for (unsigned i = 0; i < 100; i++) {
        uint64_t lba = (k * 500 + i * 13) % (VBLK_DISK_SECTORS - 8);
        uint8_t mine[VBLK_MAX_SECTORS * BLK_SECTOR_SIZE];

        if (vblk_read(lba, 8, mine, sizeof(mine)) != VBLK_OK || memcmp(mine, fix(lba), sizeof(mine)))
            cc_bad++;
    }
    cc_done++;
}

static volatile unsigned q_done;
static volatile int q_res[VBLK_SLOTS];

static void q_reader(void *arg)
{
    unsigned k = (unsigned)(uintptr_t)arg;
    uint8_t mine[VBLK_MAX_SECTORS * BLK_SECTOR_SIZE];

    q_res[k] = vblk_read(800 + 8 * k, 8, mine, sizeof(mine));
    if (q_res[k] == VBLK_OK && memcmp(mine, fix(800 + 8 * k), sizeof(mine)))
        q_res[k] = -1;
    q_done++;
}

static void test_concurrent(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), to0 = in->timeouts, i0, w0;
    unsigned threads0 = thread_count(), blocked = 0;
    int64_t id[VBLK_SLOTS];

    /* Four waiters blocked at once; one notification; the completions may share interrupts. */
    q_done = 0;
    vblk_test_skip_notify(VBLK_SLOTS);
    for (unsigned k = 0; k < VBLK_SLOTS; k++)
        id[k] = thread_create("vblk-q", q_reader, (void *)(uintptr_t)k);
    for (unsigned k = 0; k < VBLK_SLOTS; k++)
        blocked += wait_blocked(id[k]);
    unsigned busy = 0;
    expect(blocked == VBLK_SLOTS && vblk_inflight() == VBLK_SLOTS && vblk_submit(0, 1, &busy) == VBLK_EBUSY,
           "four waiters blocked with four requests in flight; a fifth request is refused (VBLK_EBUSY)");
    i0 = in->irqs;
    w0 = in->irq_wakeups;
    vblk_test_notify();
    for (unsigned k = 0; k < 20 && q_done < VBLK_SLOTS; k++)
        thread_sleep(1);
    unsigned good = 0;
    for (unsigned k = 0; k < VBLK_SLOTS; k++)
        good += q_res[k] == VBLK_OK;
    serial_puts("vblk: 4 requests completed together: ");
    serial_dec(in->irqs - i0);
    serial_puts(" interrupts, ");
    serial_dec(in->irq_wakeups - w0);
    serial_puts(" waiters woken\r\n");
    expect(q_done == VBLK_SLOTS && good == VBLK_SLOTS && in->irqs - i0 >= 1 && in->irqs - i0 <= VBLK_SLOTS &&
           in->irq_wakeups - w0 == VBLK_SLOTS, "all four completed exactly; each owner woken once, by 1-4 interrupts");

    /* Four threads reading 100 times each, overlapping freely. */
    cc_bad = cc_done = 0;
    for (unsigned k = 0; k < 4; k++)
        thread_create("vblk-cc", cc_reader, (void *)(uintptr_t)k);
    for (unsigned k = 0; k < 500 && cc_done < 4; k++)
        thread_sleep(1);
    serial_puts("vblk: 4 threads x 100 reads: at most ");
    serial_dec(in->max_inflight);
    serial_puts(" requests in flight at once\r\n");
    expect(cc_done == 4 && cc_bad == 0 && in->timeouts == to0 && in->max_inflight == VBLK_SLOTS,
           "four threads x 100 reads, all exact, no timeout, up to four requests in flight");
    thread_sleep(1);
    expect(pmm_free_count() == f0 && thread_count() == threads0 && vblk_inflight() == 0 && vblk_audit() == NULL &&
           sched_audit() == NULL, "afterwards nothing in flight, no frame or thread left over");
}

/* ---- resets with completions and interrupts in flight ---- */

static void test_late_irq(void)
{
    const struct vblk_info *in = vblk_info();
    uint64_t f0 = pmm_free_count(), p0 = in->protocol_errors, i0;
    uint16_t u0 = vblk_test_used_idx();
    unsigned slot, spins = 0;
    int ok, asserted;

    /* A completed request whose interrupt is pending when the device is reset. */
    ok = vblk_submit(50, 8, &slot) == VBLK_OK;
    while (ok && vblk_test_used_idx() == u0 && ++spins < 100000000)
        __asm__ volatile ("pause");
    asserted = intx_asserted(in->pci);
    vblk_test_break();
    expect(ok && asserted && !intx_asserted(in->pci) && in->state == VBLK_BROKEN && vblk_inflight() == 0,
           "reset with a completion's interrupt pending: the slot is released and the device drops INTx");
    i0 = in->irqs;
    thread_sleep(2);
    expect(in->irqs == i0 && set_mode(1) && read_ok(50, 8) && in->protocol_errors == p0,
           "no late interrupt reaches the driver; the reinitialized queue never sees the old completion");

    /* An interrupt claimed before a reset whose bottom half runs after the reinitialization. */
    u0 = vblk_test_used_idx();
    ok = vblk_submit(60, 8, &slot) == VBLK_OK;
    spins = 0;
    while (ok && vblk_test_used_idx() == u0 && ++spins < 100000000)
        __asm__ volatile ("pause");
    unsigned claimed = irq_test_dispatch(in->irq_line);  /* as if delivered now: the top half reads the ISR */
    vblk_test_break();
    ok &= set_mode(1);
    uint64_t late0 = in->irq_late;
    irq_run_deferred();                 /* the stale bottom half runs against the new queue */
    expect(ok && claimed == 1 && in->irq_late == late0 && vblk_inflight() == 0 && in->protocol_errors == p0 &&
           read_ok(60, 8) && vblk_audit() == NULL, "a bottom half left over from before a reset does nothing to the new queue");

    /* A blocked waiter is woken by the reset at once, not at its deadline. */
    waiter_done = 0;
    waiter_result = -1;
    vblk_test_skip_notify(1);
    int64_t id = thread_create("vblk-waiter", waiter, 0);
    int b = wait_blocked(id);
    uint64_t t0 = irq_ticks();
    vblk_test_break();
    for (unsigned k = 0; k < 10 && !waiter_done; k++)
        thread_yield();
    expect(b && waiter_done && waiter_result == VBLK_EBROKEN && irq_ticks() - t0 <= 1 && wait_free(id, 5),
           "a waiter blocked when the device is reset returns VBLK_EBROKEN at once");
    expect(set_mode(1) && read_ok(21, 1) && pmm_free_count() == f0 && vblk_audit() == NULL,
           "recovered after the resets; no frame leaked");
}

/* ---- shared interrupt line ---- */

static unsigned foreign_calls;

static int foreign_top(void *ctx)
{
    (void)ctx;
    foreign_calls++;
    return IRQ_NONE;
}

/* QEMU's default e1000 NIC, if present: only located (VOS has no network driver and maps no device memory). */
static struct pci_addr nic;

static int is_e1000(struct pci_addr a, void *ctx)
{
    (void)ctx;
    return pci_read16(a, PCI_VENDOR_ID) == 0x8086 && pci_read16(a, PCI_DEVICE_ID) == 0x100E;
}

static unsigned foreign_claims;

static int foreign_claim_top(void *ctx)
{
    (void)ctx;
    foreign_claims++;
    return IRQ_HANDLED;
}

static void test_shared_irq(void)
{
    const struct vblk_info *in = vblk_info();
    const struct irq_stats *is = irq_stats();
    unsigned line = in->irq_line, bad = 0;
    uint64_t f0 = pmm_free_count(), i0, n0, s0;
    int h;

    /* Another handler on the virtio-blk line: every interrupt asks both. */
    foreign_calls = 0;
    int reg = irq_register(line, IRQ_TRIGGER_LEVEL, "test-foreign", foreign_top, 0, 0, &h) == IRQ_OK;
    i0 = in->irqs + in->irq_none;
    for (unsigned i = 0; i < 32; i++)
        bad += !read_ok(i * 50, 8);
    thread_sleep(1);
    uint64_t asked = in->irqs + in->irq_none - i0;
    reg &= irq_unregister(h) == IRQ_OK;
    expect(reg && bad == 0 && asked >= 32 && foreign_calls == asked && !irq_line_disabled(line) &&
           irq_chip()->masks() == irq_expected_masks(),
           "a second handler on the line: both are asked on every interrupt; virtio-blk claims its own; reads exact");

    /*
     * Who else is on the line. QEMU's default e1000 shares it in the default
     * configuration (not with -nic none); it stays idle (its interrupts are
     * masked from reset), so every interrupt on the line so far was claimed.
     */
    int nic_found = pci_scan_bus0(is_e1000, 0, &nic), shared = nic_found && pci_read8(nic, PCI_INTERRUPT_LINE) == line;
    serial_puts("vblk: IRQ ");
    serial_dec(line);
    serial_puts(shared ? " is shared with the e1000 at 00:" : " is not shared with another known device");
    if (shared)
        serial_hex(nic.dev);
    serial_puts("; unclaimed interrupts on it so far: ");
    serial_dec(is->unclaimed[line]);
    serial_puts("\r\n");
    expect(is->unclaimed[line] == 0 && (!shared || !intx_asserted(nic)),
           "no interrupt on the line went unclaimed; a sharing e1000 never asserted INTx");

    /* An interrupt on the line that virtio-blk did not raise (synthesized): its ISR reads 0, the other handler claims. */
    foreign_claims = 0;
    reg = irq_register(line, IRQ_TRIGGER_LEVEL, "test-foreign", foreign_claim_top, 0, 0, &h) == IRQ_OK;
    n0 = in->irq_none;
    i0 = in->irqs;
    unsigned claimed = irq_test_dispatch(line);
    irq_run_deferred();
    reg &= irq_unregister(h) == IRQ_OK;
    expect(reg && claimed == 1 && foreign_claims == 1 && in->irq_none == n0 + 1 && in->irqs == i0 && read_ok(9, 8),
           "an interrupt virtio-blk did not raise: its top half reads ISR 0 and says not mine; the other handler claims it");

    /* A storm on the line, synthesized (also without another device): fallback to polling, recovery. */
    s0 = is->storms;
    for (unsigned i = 0; i <= IRQ_STORM_LIMIT; i++)
        irq_test_dispatch(line);
    irq_run_deferred();
    expect(irq_line_disabled(line) && is->storms == s0 + 1 && in->mode == VBLK_MODE_POLL && in->irq_handle == 0 &&
           read_ok(33, 8) && vblk_audit() == NULL && irq_audit() == NULL,
           "a synthesized storm on the line: masked, virtio-blk polls and keeps reading exactly");
    irq_test_clear_storm(line);
    expect(set_mode(1) && read_ok(34, 8) && pmm_free_count() == f0 && irq_audit() == NULL,
           "recovered into interrupt mode; no frame used");
}

/* ---- CPU progress while requests wait ---- */

static volatile unsigned pr_done, pr_bad;

static void progress_reader(void *arg)
{
    uint64_t x = (uint64_t)(uintptr_t)arg;
    uint8_t mine[VBLK_MAX_SECTORS * BLK_SECTOR_SIZE];

    for (unsigned i = 0; i < 300; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t lba = (x >> 20) % (VBLK_DISK_SECTORS - 8);

        if (vblk_read(lba, 8, mine, sizeof(mine)) != VBLK_OK || memcmp(mine, fix(lba), sizeof(mine)))
            pr_bad++;
    }
    pr_done = 1;
}

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* TSC cycles per timer tick, measured over 10 ticks with only the idle thread running. */
static uint64_t tsc_per_tick(void)
{
    uint64_t c0;

    thread_sleep(1);                    /* start right after a tick */
    c0 = rdtsc();
    thread_sleep(10);
    return (rdtsc() - c0) / 10;
}

/*
 * 300 reads by a kernel thread beside a CPU-bound program. Timer ticks are
 * only counted while interrupts are enabled (ring 3 or the idle halt), so
 * the wall time comes from the TSC: ticks the kernel never saw were lost
 * while it ran with IF = 0 (polling).
 */
static int measure_progress(const char *mode, uint64_t *prog, uint64_t *el)
{
    uint64_t pt, t0, c0, per = tsc_per_tick(), expected;
    int ok;

    proc_test_tick_budget(5000);
    ok = launch(&P[0], "m-memtag1");
    pr_done = pr_bad = 0;
    pt = P[0].ticks;
    t0 = irq_ticks();
    c0 = rdtsc();
    ok &= thread_create("vblk-progress", progress_reader, (void *)12345) > 0;
    for (unsigned w = 0; ok && !pr_done && w < 3000; w++)
        thread_sleep(1);
    expected = per ? (rdtsc() - c0 + per / 2) / per : 0;
    *prog = P[0].ticks - pt;
    *el = irq_ticks() - t0;
    proc_kill(&P[0], "test kill");
    ok &= proc_wait(&P[0]) == PROC_KILLED;
    proc_destroy(&P[0]);
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    serial_puts("vblk: 300 reads of 8 sectors beside a CPU-bound program, ");
    serial_puts(mode);
    serial_puts(": wall time ");
    serial_dec(expected);
    serial_puts(" tick periods (TSC), ");
    serial_dec(*el);
    serial_puts(" timer ticks counted, the program ran ");
    serial_dec(*prog);
    serial_puts(" of them\r\n");
    *el = expected > *el ? expected : *el;     /* the share is measured against the wall time */
    return ok && pr_done && pr_bad == 0 && per > 0;
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
#if defined(VOS_TEST_CASE_ext2)
    /* A virtio-blk device holds an ext2 image here; selftest_ext2.c drives it. */
    expect(vblk_info()->state == VBLK_READY && vblk_audit() == NULL, "virtio-blk device ready (ext2 image)");
#elif !defined(VBLK_LIVE)
    test_absent();
#else
    expect(find_fixture(), "GRUB delivered the fixture module vblk-fixture");
    if (fixture && vblk_info()->state == VBLK_READY) {
        test_device();
#ifdef VOS_TEST_CASE_vblk_ioerr
        test_contents();
        test_ioerr();
#else
        /*
         * N3.4: interrupt mode first. Every later test relies on working
         * interrupts (without them each read waits for its re-check), so a
         * failure here ends the live tests at once.
         */
        int before = failures;
        test_irq_mode();
        if (failures != before) {
            serial_puts("vblk: interrupt mode failed its basic checks; the other live tests are skipped\r\n");
            set_mode(0);                /* the block-layer tests that follow then finish in time */
            goto summary;
        }
        test_contents();
        test_validation();
        test_queue();
        test_unsupported();
        test_protocol();
        test_repeat();
        test_processes();
        /* N3.4: interrupt mode in depth. */
        test_early_completion();
        test_irq_timeout();
        test_cancel();
        test_blk_cancel();
        test_concurrent();
        test_late_irq();
        test_shared_irq();
        uint64_t iprog, iel, pprog, pel;
        expect(measure_progress("interrupt mode", &iprog, &iel) && iprog * 10 >= iel * 7 && iel <= 150,
               "interrupt mode: 300 exact reads within 150 ticks while the program kept at least 70% of the wall time");
        /* The polling fallback, on its own: the N3.1/N3.2 tests in polling mode. */
        expect(set_mode(0) && streq(vblk_info()->poll_why, "polling requested (test)"),
               "polling mode on request (the fallback), device reinitialized");
        test_contents();
        test_queue();
        test_timeout();
        test_repeat();
        test_reset_while_waiting();
        test_responsive();
        expect(measure_progress("polling mode", &pprog, &pel), "polling mode: 300 exact reads beside the program");
        expect(set_mode(1), "back to interrupt mode");
#endif
#ifndef VOS_TEST_CASE_vblk_ioerr
    summary:
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
