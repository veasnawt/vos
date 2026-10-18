/*
 * Block I/O layer and read-only cache (N3.2), VOS_TEST only.
 *
 * Every test image registers a test-only RAM device ("ramdev": 1027 sectors,
 * i.e. 128 full cache lines and a last line of 3 sectors) whose contents
 * are a function of the sector number, and whose reads can fail, yield or
 * change the device epoch on request. Against it: the registry (IDs,
 * generations, limits), request validation, exact hit/miss/eviction
 * accounting, LRU replacement, exhaustion by pinned lines, failed loads,
 * the uncached fallback after an I/O error, invalidation after a reset,
 * checksum-detected corruption, concurrent loads of the same line, and
 * frame accounting over repeated registration.
 *
 * With VOS_TEST_CASE=vblk or vblk-ioerr the same layer runs on the
 * virtio-blk device, compared sector by sector with the GRUB module
 * "vblk-fixture" (see selftest_vblk.c), including shutdown and
 * reinitialization cycles and reads beside running user processes.
 */
#include "selftest.h"
#include "blockdev.h"
#include "console.h"
#include "irq.h"
#include "kmap.h"
#include "lib.h"
#include "loader.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "thread.h"
#include "virtio_blk.h"
#include "vmm.h"

#if defined(VOS_TEST_CASE_vblk) || defined(VOS_TEST_CASE_vblk_ioerr)
#define VBLK_LIVE 1
#endif
#define VBLK_DISK_SECTORS   2051
#define VBLK_IOERR_SECTOR   1000

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

static const struct blk_stats *S;
static struct blk_stats s0;

static void mark(void)
{
    s0 = *blk_stats();
}

#define D(field) (S->field - s0.field)

/* ---- the RAM test device ---- */

#define RAM_SECTORS 1027

static struct {
    int state;
    uint64_t epoch;
    uint64_t reads, bad, last_lba;
    uint32_t last_count;
    int fail_on, fail_err;
    uint64_t fail_lba;
    int yield_in_read, bump_epoch_in_read;
    uint64_t line_reads[RAM_SECTORS / 8 + 1];
} ram;

/* Contents of sector lba of the RAM device with this salt (0: the main RAM device). */
static void ram_sector_s(uint64_t salt, uint64_t lba, uint8_t *p)
{
    uint64_t x = (lba + 7) * 0x9E3779B97F4A7C15ULL ^ 0x5241444556303131ULL ^ salt;

    memcpy(p, "RAMDEV01", 8);
    for (int i = 0; i < 8; i++)
        p[8 + i] = (uint8_t)(lba >> (8 * i));
    for (int i = 16; i < BLK_SECTOR_SIZE; i += 8) {
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        uint64_t v = x * 0x2545F4914F6CDD1DULL;
        for (int j = 0; j < 8; j++)
            p[i + j] = (uint8_t)(v >> (8 * j));
    }
}

static void ram_sector(uint64_t lba, uint8_t *p)
{
    ram_sector_s(0, lba, p);
}

static void ram_info(void *ctx, struct blk_info *o)
{
    (void)ctx;
    o->state = ram.state;
    o->sectors = RAM_SECTORS;
    o->max_sectors = BLK_LINE_SECTORS;
    o->caps = BLK_CAP_READ;
    o->epoch = ram.epoch;
}

static int ram_read(void *ctx, uint64_t lba, uint32_t count, void *buf, uint64_t cap)
{
    uint64_t salt = ctx ? *(const uint64_t *)ctx : 0;

    ram.reads++;
    if (count == 0 || count > BLK_LINE_SECTORS || cap < (uint64_t)count * BLK_SECTOR_SIZE ||
        lba >= RAM_SECTORS || count > RAM_SECTORS - lba) {
        ram.bad++;                          /* the layer asked for something impossible */
        return BLK_EINVAL;
    }
    ram.last_lba = lba;
    ram.last_count = count;
    if (count == BLK_LINE_SECTORS || lba % BLK_LINE_SECTORS == 0)
        ram.line_reads[lba / BLK_LINE_SECTORS]++;
    if (ram.yield_in_read)
        thread_yield();                     /* like a driver waiting for its device */
    if (ram.bump_epoch_in_read)
        ram.epoch++;                        /* like a reset by another thread meanwhile */
    if (ram.fail_on && ram.fail_lba >= lba && ram.fail_lba < lba + count)
        return ram.fail_err;
    for (uint32_t i = 0; i < count; i++)
        ram_sector_s(salt, lba + i, (uint8_t *)buf + i * BLK_SECTOR_SIZE);
    return BLK_OK;
}

static const struct blk_ops ram_ops = { "ramdev", ram_info, ram_read, 0 };
static int ram_null_read(void *c, uint64_t l, uint32_t n, void *b, uint64_t p)
{
    (void)c, (void)l, (void)n, (void)b, (void)p;
    return BLK_EIO;
}
static void small_info(void *ctx, struct blk_info *o)
{
    ram_info(ctx, o);
    o->max_sectors = 4;
}
static void empty_info(void *ctx, struct blk_info *o)
{
    ram_info(ctx, o);
    o->sectors = 0;
}
static const struct blk_ops bad_read_ops = { "bad", ram_info, 0, 0 };
static const struct blk_ops small_ops = { "small", small_info, ram_null_read, 0 };
static const struct blk_ops empty_ops = { "empty", empty_info, ram_null_read, 0 };

static uint8_t big[BLK_MAX_REQUEST * BLK_SECTOR_SIZE + 64];
static uint8_t want[BLK_MAX_REQUEST * BLK_SECTOR_SIZE];
static const uint8_t konst[64] = { 1 };

/* Read through the layer and compare with the RAM device's contents. */
static int ram_ok(blk_id_t id, uint64_t lba, uint32_t count)
{
    if (blk_read(id, lba, count, big, sizeof(big)) != BLK_OK)
        return 0;
    for (uint32_t i = 0; i < count; i++)
        ram_sector(lba + i, want + i * BLK_SECTOR_SIZE);
    return memcmp(big, want, (uint64_t)count * BLK_SECTOR_SIZE) == 0;
}

static void ram_reset_state(void)
{
    memset(&ram, 0, sizeof(ram));
    ram.state = BLK_DEV_READY;
    ram.epoch = 1;
}

static void test_registry(void)
{
    unsigned base = blk_count();
    blk_id_t id, id2, extra[BLK_MAX_DEVICES], tmp;
    struct blk_desc d;
    uint64_t f0 = pmm_free_count();

    ram_reset_state();
    expect(blk_register("bad", &bad_read_ops, 0, &tmp) == BLK_EINVAL &&
           blk_register("small", &small_ops, 0, &tmp) == BLK_EINVAL &&
           blk_register("empty", &empty_ops, 0, &tmp) == BLK_EINVAL && blk_register(0, &ram_ops, 0, &tmp) == BLK_EINVAL &&
           blk_count() == base && pmm_free_count() == f0,
           "registry: refuses missing operations, requests below one cache line, zero capacity, no name");
    expect(blk_register("ram0", &ram_ops, 0, &id) == BLK_OK && id != 0 && blk_count() == base + 1,
           "registry: the RAM test device registers");
    expect(blk_describe(id, &d) == BLK_OK && streq(d.name, "ram0") && streq(d.driver, "ramdev") &&
           d.sectors == RAM_SECTORS && d.sector_size == 512 && d.max_request == BLK_MAX_REQUEST &&
           d.caps == BLK_CAP_READ && d.state == BLK_DEV_READY,
           "registry: identity, capacity, sector size, limits, capabilities and state described");
    if (base == 0)
        expect(pmm_free_count() == f0 - BLK_CACHE_LINES, "cache: 32 frames allocated with the first device");
    else
        expect(pmm_free_count() == f0, "cache: an existing cache is shared, no frame allocated");

    blk_id_t bad[] = { 0, id ^ (1u << 4), (id & ~0xFu) | 0xF, (id & ~0xFu) | (BLK_MAX_DEVICES + 1), 0xFFFFFFFFu };
    unsigned refused = 0;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        refused += blk_read(bad[i], 0, 1, big, sizeof(big)) == BLK_ENODEV && blk_describe(bad[i], &d) == BLK_ENODEV &&
                   blk_write(bad[i], 0, 1, big, 512) == BLK_ENODEV && blk_invalidate(bad[i] ? bad[i] : 1) == BLK_ENODEV &&
                   blk_read_block(bad[i], 1024, 0, big, sizeof(big)) == BLK_ENODEV;
    expect(refused == sizeof(bad) / sizeof(bad[0]) && ram.reads == 0,
           "registry: zero, wrong-generation, out-of-range and unused IDs are refused with BLK_ENODEV");

    unsigned n = 0;
    while (n < BLK_MAX_DEVICES && blk_register("extra", &ram_ops, 0, &extra[n]) == BLK_OK)
        n++;
    expect(blk_count() == BLK_MAX_DEVICES && n == BLK_MAX_DEVICES - base - 1 &&
           blk_register("one-too-many", &ram_ops, 0, &tmp) == BLK_EBUSY,
           "registry: holds 4 devices; the fifth is refused with BLK_EBUSY");
    for (unsigned i = 0; i < n; i++)
        blk_unregister(extra[i]);
    expect(blk_unregister(id) == BLK_OK && blk_count() == base && blk_read(id, 0, 1, big, sizeof(big)) == BLK_ENODEV &&
           blk_unregister(id) == BLK_ENODEV, "registry: after unregistration the ID is dead");
    expect(blk_register("ram0", &ram_ops, 0, &id2) == BLK_OK && id2 != id &&
           blk_read(id, 0, 1, big, sizeof(big)) == BLK_ENODEV && blk_unregister(id2) == BLK_OK,
           "registry: a reused slot gets a new generation; the old ID stays dead");
    expect(pmm_free_count() == f0 && blk_audit() == NULL, "registry: every cache frame returned; audit clean");
}

static void test_validation(blk_id_t id)
{
    uint64_t g[KMAP_MAX_GUARDS], cf[BLK_CACHE_LINES];
    uint64_t r0 = ram.reads;

    kmap_guard_pages(g, KMAP_MAX_GUARDS);
    blk_cache_frames(cf, BLK_CACHE_LINES);
    memset(big, 0xA5, sizeof(big));
    mark();
    expect(blk_read(id, 0, 0, big, sizeof(big)) == BLK_EINVAL && blk_read(id, 0, BLK_MAX_REQUEST + 1, big, sizeof(big)) ==
           BLK_EINVAL && blk_read(id, 0, 2, big, 1023) == BLK_EINVAL, "validation: 0 sectors, 65 sectors, short capacity");
    expect(blk_read(id, 0, 1, 0, 512) == BLK_EINVAL && blk_read(id, 0, 1, (void *)0x400000, 512) == BLK_EINVAL &&
           blk_read(id, 0, 1, (void *)konst, 512) == BLK_EINVAL && blk_read(id, 0, 1, (void *)(g[0] - 256), 512) ==
           BLK_EINVAL && blk_read(id, 0, 1, phys_to_virt(cf[3]), 512) == BLK_EINVAL &&
           blk_read(id, 0, 1, (void *)0xFFFFFFFFFFFFFF00ULL, 512) == BLK_EINVAL,
           "validation: NULL, user, read-only, guard-page, cache-frame and wrapping buffers refused");
    expect(blk_read(id, RAM_SECTORS, 1, big, sizeof(big)) == BLK_ERANGE &&
           blk_read(id, RAM_SECTORS - 1, 2, big, sizeof(big)) == BLK_ERANGE &&
           blk_read(id, ~0ULL, 1, big, sizeof(big)) == BLK_ERANGE && blk_read(id, ~0ULL - 1, 4, big, sizeof(big)) ==
           BLK_ERANGE, "validation: sectors past the end and wrapping ranges refused (overflow-safe)");
    expect(blk_read_block(id, 3000, 0, big, sizeof(big)) == BLK_EINVAL && blk_read_block(id, 256, 0, big, sizeof(big)) ==
           BLK_EINVAL && blk_read_block(id, 1024, (~0ULL >> 1) + 1, big, sizeof(big)) == BLK_ERANGE &&
           blk_read_block(id, 4096, 128, big, sizeof(big)) == BLK_ERANGE && blk_read_block(id, 1024, 0, big, 1023) ==
           BLK_EINVAL, "validation: block sizes other than 512-4096, overflowing and partial blocks refused");
    expect(blk_write(id, 0, 1, big, 512) == BLK_EUNSUPP, "validation: writes are refused as unsupported");
    ram.state = BLK_DEV_UNAVAILABLE;
    expect(blk_read(id, 0, 1, big, sizeof(big)) == BLK_EUNAVAIL, "validation: an unavailable device is refused");
    ram.state = BLK_DEV_READY;
    unsigned untouched = 1;
    for (unsigned i = 0; i < sizeof(big); i++)
        untouched &= big[i] == 0xA5;
    expect(ram.reads == r0 && untouched && D(refused) == 20 && D(misses) == 0 && D(hits) == 0,
           "validation: refused requests reach neither the device nor the buffer (20 refusals counted)");
}

static void test_contents(blk_id_t id)
{
    expect(blk_invalidate(id) == BLK_OK && blk_test_valid_lines() == 0, "cache: empty after invalidation");
    mark();
    uint64_t r0 = ram.reads;
    expect(ram_ok(id, 0, 1) && D(misses) == 1 && D(hits) == 0 && D(device_reads) == 1 && ram.reads == r0 + 1 &&
           ram.last_lba == 0 && ram.last_count == 8, "first sector: one miss loads line 0 (8 sectors)");
    expect(ram_ok(id, 5, 1) && D(hits) == 1 && D(device_reads) == 1, "same line again: a hit, no device read");
    expect(ram_ok(id, 6, 4) && D(hits) == 2 && D(misses) == 2 && D(device_reads) == 2,
           "sectors 6-9 across the line boundary: one hit (line 0) and one miss (line 1)");
    expect(ram_ok(id, RAM_SECTORS - 3, 3) && ram.last_lba == 1024 && ram.last_count == 3 &&
           blk_test_line_state(id, 128) == BLK_LINE_VALID,
           "final partial line: line 128 loaded with exactly its 3 sectors (1024-1026)");
    expect(ram_ok(id, RAM_SECTORS - 1, 1) && ram_ok(id, RAM_SECTORS - 2, 2) && D(device_reads) == 3,
           "last sector and the last two: hits in the partial line");
    expect(ram_ok(id, 100, BLK_MAX_REQUEST) && ram_ok(id, 1027 - 64, 64),
           "64-sector requests at an unaligned start and ending exactly at the capacity");
    expect(blk_read_block(id, 4096, 127, big, sizeof(big)) == BLK_OK && ram_ok(id, 1016, 8) &&
           blk_read_block(id, 1024, 512, big, 1024) == BLK_OK && blk_read_block(id, 1024, 513, big, 1024) == BLK_ERANGE &&
           blk_read_block(id, 2048, 1, big, 2048) == BLK_OK && blk_read_block(id, 512, 1026, big, 512) == BLK_OK,
           "file-system blocks of 512, 1024, 2048 and 4096 bytes, refused only past the capacity");
    unsigned same = 1;
    for (unsigned i = 0; i < 20; i++)
        same &= ram_ok(id, 333, 7);
    expect(same, "repeated reads of the same sectors return identical, correct data");
    expect(ram.bad == 0 && blk_audit() == NULL, "the layer never asked the device for an impossible range");
}

static void test_lru(blk_id_t id)
{
    blk_invalidate(id);
    mark();
    unsigned ok = 1;
    for (uint64_t l = 0; l < BLK_CACHE_LINES; l++)
        ok &= ram_ok(id, l * 8, 8);
    expect(ok && D(misses) == BLK_CACHE_LINES && D(evictions) == 0 && blk_test_valid_lines() == BLK_CACHE_LINES,
           "32 lines fill the cache: 32 misses, no eviction");
    expect(ram_ok(id, 0, 1) && D(hits) == 1, "line 0 touched again (now the most recently used)");
    expect(ram_ok(id, 32 * 8, 8) && D(evictions) == 1 && blk_test_line_state(id, 1) == BLK_LINE_FREE &&
           blk_test_line_state(id, 0) == BLK_LINE_VALID && blk_test_line_state(id, 32) == BLK_LINE_VALID,
           "a 33rd line evicts the least recently used line (1), not line 0");
    uint64_t r1 = ram.line_reads[1];
    expect(ram_ok(id, 8, 8) && ram.line_reads[1] == r1 + 1 && D(evictions) == 2 && blk_test_line_state(id, 2) ==
           BLK_LINE_FREE, "the evicted line reloads from the device (and evicts the next LRU line, 2)");
}

static void test_exhaustion(blk_id_t id)
{
    unsigned pinned = 0;

    for (uint64_t l = 0; l < 64; l++)
        if (blk_test_line_state(id, l) == BLK_LINE_VALID && blk_test_pin(id, l, 1) == BLK_OK)
            pinned++;
    mark();
    uint64_t r0 = ram.reads;
    expect(pinned == BLK_CACHE_LINES && blk_read(id, 800, 1, big, sizeof(big)) == BLK_EBUSY && D(busy) == 1 &&
           ram.reads == r0, "every line pinned: a miss is refused with BLK_EBUSY, nothing read");
    expect(blk_invalidate(id) == BLK_EBUSY && blk_unregister(id) == BLK_EBUSY,
           "pinned lines block invalidation and unregistration");
    expect(ram_ok(id, 0, 1) && D(hits) == 1, "a pinned line still serves hits");
    blk_test_pin(id, 0, 0);
    expect(ram_ok(id, 800, 1) && blk_test_line_state(id, 0) == BLK_LINE_FREE && D(evictions) == 1,
           "recovery: one unpinned line is evicted for the miss");
    for (uint64_t l = 0; l < 64; l++)
        while (blk_test_pin(id, l, 0) == BLK_OK)
            ;
    expect(blk_invalidate(id) == BLK_OK && blk_audit() == NULL, "all lines unpinned; invalidation succeeds again");
}

static void test_failures(blk_id_t id)
{
    blk_invalidate(id);
    ram.fail_on = 1;
    ram.fail_lba = 300;
    ram.fail_err = BLK_EIO;
    mark();
    expect(blk_read(id, 300, 1, big, sizeof(big)) == BLK_EIO && blk_test_line_state(id, 37) == BLK_LINE_FREE &&
           D(load_errors) == 1 && D(fallback_reads) == 1, "I/O error on a miss: BLK_EIO, the line is not cached");
    expect(ram_ok(id, 301, 3) && blk_test_line_state(id, 37) == BLK_LINE_FREE && D(fallback_reads) == 4,
           "the other sectors of the failing line are served one by one, uncached");
    expect(blk_read(id, 298, 4, big, sizeof(big)) == BLK_EIO && blk_test_valid_lines() == 0,
           "a request covering the failing sector fails; no valid line was created");
    ram.fail_err = BLK_ETIMEDOUT;
    uint64_t fb = S->fallback_reads;
    expect(blk_read(id, 300, 1, big, sizeof(big)) == BLK_ETIMEDOUT && S->fallback_reads == fb &&
           blk_test_line_state(id, 37) == BLK_LINE_FREE, "a timeout is reported as such, without the fallback");
    ram.fail_err = BLK_EUNAVAIL;
    expect(blk_read(id, 300, 1, big, sizeof(big)) == BLK_EUNAVAIL, "an unavailable device during a load is reported");
    ram.fail_on = 0;
    expect(ram_ok(id, 300, 1) && blk_test_line_state(id, 37) == BLK_LINE_VALID,
           "after the device recovers the line loads and is cached");

    /* Reset (epoch change) while a line is cached, and while one loads. */
    mark();
    ram.epoch++;
    expect(ram_ok(id, 300, 1) && D(stale_drops) == 1 && D(misses) == 1 && D(device_reads) == 1,
           "after a device reset the cached line is stale: dropped and reloaded");
    ram.bump_epoch_in_read = 1;
    expect(blk_read(id, 500, 1, big, sizeof(big)) == BLK_EUNAVAIL && blk_test_valid_lines() == 1,
           "a reset during the load discards the loaded data (BLK_EUNAVAIL)");
    ram.bump_epoch_in_read = 0;

    /* Corrupted cache contents are detected by the checksum and reloaded. */
    mark();
    expect(ram_ok(id, 300, 1) && blk_test_corrupt(id, 37) == BLK_OK && ram_ok(id, 300, 1) && D(corrupt_drops) == 1 &&
           D(device_reads) == 2, "a changed cache line is detected on the next hit and reloaded");
    expect(blk_audit() == NULL, "audit clean after the failures");
}

/* Two devices, the same line numbers: the cache key must keep them apart. */
static uint64_t salt1 = 0x1111222233334444ULL;

static int salted_ok(blk_id_t id, uint64_t salt, uint64_t lba, uint32_t count)
{
    if (blk_read(id, lba, count, big, sizeof(big)) != BLK_OK)
        return 0;
    for (uint32_t i = 0; i < count; i++)
        ram_sector_s(salt, lba + i, want + i * BLK_SECTOR_SIZE);
    return memcmp(big, want, (uint64_t)count * BLK_SECTOR_SIZE) == 0;
}

static void test_two_devices(blk_id_t id)
{
    blk_id_t id1;

    if (blk_register("ram1", &ram_ops, &salt1, &id1) != BLK_OK) {
        expect(0, "second RAM device registered");
        return;
    }
    blk_invalidate(0);
    mark();
    expect(salted_ok(id, 0, 0, 8) && salted_ok(id1, salt1, 0, 8) && D(misses) == 2 && D(hits) == 0,
           "line 0 of two devices: two misses, each device's own data");
    expect(salted_ok(id, 0, 3, 2) && salted_ok(id1, salt1, 3, 2) && salted_ok(id1, salt1, 1026, 1) &&
           salted_ok(id, 0, 1026, 1) && D(hits) == 2 && D(misses) == 4,
           "hits return each device's own line; the same last line is cached once per device");
    expect(blk_invalidate(id1) == BLK_OK && blk_test_line_state(id, 0) == BLK_LINE_VALID &&
           blk_test_line_state(id1, 0) == BLK_LINE_FREE, "invalidating one device leaves the other's lines");
    expect(blk_unregister(id1) == BLK_OK && blk_test_line_state(id, 128) == BLK_LINE_VALID && blk_audit() == NULL,
           "unregistering it drops only its lines");
}

static volatile unsigned conc_done, conc_bad;
static blk_id_t conc_id;
static uint8_t conc_buf[2][2 * BLK_SECTOR_SIZE], conc_want[2 * BLK_SECTOR_SIZE];

static void conc_reader(void *arg)
{
    uint64_t lba = (uint64_t)arg;
    unsigned k = lba == 400 ? 0 : 1;

    if (blk_read(conc_id, lba, 2, conc_buf[k], sizeof(conc_buf[k])) != BLK_OK)
        conc_bad++;
    ram_sector(lba, conc_want);
    ram_sector(lba + 1, conc_want + BLK_SECTOR_SIZE);
    conc_bad += memcmp(conc_buf[k], conc_want, sizeof(conc_want)) != 0;
    conc_done++;
}

static void test_concurrent(blk_id_t id)
{
    blk_invalidate(id);
    mark();
    uint64_t r0 = ram.line_reads[50];
    ram.yield_in_read = 1;
    conc_done = conc_bad = 0;
    conc_id = id;
    int ok = thread_create("blk-a", conc_reader, (void *)(uint64_t)400) > 0 &&
             thread_create("blk-b", conc_reader, (void *)(uint64_t)402) > 0;
    for (unsigned i = 0; ok && conc_done < 2 && i < 1000; i++)
        thread_yield();
    ram.yield_in_read = 0;
    expect(ok && conc_done == 2 && conc_bad == 0 && ram.line_reads[50] == r0 + 1 && D(load_waits) >= 1 &&
           D(hits) == 1 && D(misses) == 1, "two threads need line 50 at once: one loads it, the other waits, then hits");
    expect(blk_audit() == NULL, "no line left loading");
}

static void test_ram(void)
{
    blk_id_t id;
    uint64_t f0 = pmm_free_count();
    unsigned base = blk_count();

    test_registry();
    ram_reset_state();
    if (blk_register("ram0", &ram_ops, 0, &id) != BLK_OK) {
        expect(0, "RAM test device registered");
        return;
    }
    test_validation(id);
    test_contents(id);
    test_lru(id);
    test_exhaustion(id);
    test_failures(id);
    test_concurrent(id);
    test_two_devices(id);
    expect(blk_unregister(id) == BLK_OK && blk_count() == base, "RAM test device unregistered");

    unsigned cycles = 0;
    for (unsigned i = 0; i < 20; i++) {
        blk_id_t t;
        cycles += blk_register("ram0", &ram_ops, 0, &t) == BLK_OK && ram_ok(t, 7, 3) && blk_unregister(t) == BLK_OK;
    }
    expect(cycles == 20 && pmm_free_count() == f0 && blk_audit() == NULL,
           "20 register/read/unregister cycles: no frame leaked, audit clean");
}

/* ---- the live virtio-blk device ---- */

#ifdef VBLK_LIVE
static const uint8_t *fixture;

static int find_fixture(void)
{
    for (uint32_t i = 0; i < mbi->module_count; i++)
        if (streq(mbi->modules[i].cmdline, "vblk-fixture")) {
            fixture = phys_to_virt(mbi->modules[i].start);
            return (mbi->modules[i].end - mbi->modules[i].start) / BLK_SECTOR_SIZE == VBLK_DISK_SECTORS;
        }
    return 0;
}

static int disk_ok(blk_id_t id, uint64_t lba, uint32_t count)
{
    return blk_read(id, lba, count, big, sizeof(big)) == BLK_OK &&
           memcmp(big, fixture + lba * BLK_SECTOR_SIZE, (uint64_t)count * BLK_SECTOR_SIZE) == 0;
}

static void test_live_device(void)
{
    const struct vblk_info *in = vblk_info();
    blk_id_t id = in->blk_id;
    struct blk_desc d;

    expect(blk_count() == 1 && blk_nth(0) == id && id != 0, "the virtio-blk device is the one registered device");
    expect(blk_describe(id, &d) == BLK_OK && streq(d.name, "vblk0") && streq(d.driver, "virtio-blk") &&
           d.sectors == VBLK_DISK_SECTORS && d.sector_size == 512 && d.state == BLK_DEV_READY &&
           (d.caps & BLK_CAP_READ) && ((d.caps & BLK_CAP_DEVICE_RO) != 0) == ((in->offered >> VIRTIO_BLK_F_RO) & 1),
           "described as vblk0 / virtio-blk, 2051 sectors of 512 bytes, read capability, RO as offered");
}

#ifndef VOS_TEST_CASE_vblk_ioerr
static void test_live_cache(void)
{
    const struct vblk_info *in = vblk_info();
    blk_id_t id = in->blk_id;
    unsigned bad = 0, reqs = 0;

    blk_invalidate(id);
    mark();
    uint64_t sub0 = in->submitted;
    for (uint64_t lba = 0; lba < VBLK_DISK_SECTORS; reqs++) {
        uint32_t c = 1 + (reqs * 7) % BLK_MAX_REQUEST;

        if (c > VBLK_DISK_SECTORS - lba)
            c = (uint32_t)(VBLK_DISK_SECTORS - lba);
        bad += !disk_ok(id, lba, c);
        lba += c;
    }
    expect(bad == 0, "every sector through the cache equals the fixture (requests of 1-64 sectors)");
    expect(D(misses) == 257 && D(device_reads) == 257 && in->submitted - sub0 == 257,
           "one scan: 257 lines (256 full + the partial line 2048-2050) loaded exactly once each");
    expect(D(evictions) == 257 - BLK_CACHE_LINES && blk_test_valid_lines() == BLK_CACHE_LINES,
           "LRU kept the last 32 lines");
    mark();
    sub0 = in->submitted;
    expect(disk_ok(id, 225 * 8, VBLK_DISK_SECTORS - 225 * 8 < 64 ? VBLK_DISK_SECTORS - 225 * 8 : 64) &&
           disk_ok(id, 2048, 3) && disk_ok(id, 2050, 1) && D(misses) == 0 && in->submitted == sub0,
           "re-reading the cached tail (including the partial line) is served without the device");
    unsigned blocks = 1;
    for (uint64_t b = 0; b < 256; b += 37)
        blocks &= blk_read_block(id, 4096, b, big, 4096) == BLK_OK && !memcmp(big, fixture + b * 4096, 4096);
    expect(blocks && blk_read_block(id, 4096, 256, big, 4096) == BLK_ERANGE &&
           blk_read_block(id, 1024, 1024, big, 1024) == BLK_OK && !memcmp(big, fixture + 1024 * 1024, 1024) &&
           blk_read_block(id, 1024, 1025, big, 1024) == BLK_ERANGE,
           "4 KiB and 1 KiB blocks equal the fixture; the block that would cross sector 2051 is refused");

    /* Reinitialization changes the epoch: nothing cached before may be returned. */
    expect(disk_ok(id, 2050, 1), "sector 2050 cached");
    mark();
    sub0 = in->submitted;
    expect(vblk_reinit() == VBLK_READY && disk_ok(id, 2050, 1) && D(stale_drops) >= 1 && D(misses) == 1 &&
           in->submitted == sub0 + 1, "after a reinitialization the cached line is dropped and read again");

    /* A timeout makes the device unavailable: even cached data is refused until it recovers. */
    expect(disk_ok(id, 0, 1), "sector 0 cached");
    vblk_test_poll_limit(20000);
    vblk_test_skip_notify(1);
    int r = blk_read(id, 1500, 1, big, sizeof(big));
    vblk_test_poll_limit(0);
    expect(r == BLK_ETIMEDOUT && blk_read(id, 0, 1, big, sizeof(big)) == BLK_EUNAVAIL,
           "a timeout on a miss is BLK_ETIMEDOUT; afterwards even cached sectors are BLK_EUNAVAIL");
    mark();
    expect(vblk_reinit() == VBLK_READY && disk_ok(id, 0, 1) && D(misses) == 1 && blk_test_line_state(id, 187) ==
           BLK_LINE_FREE, "recovered: the old line is not trusted; the timed-out line was never cached");
}

static uint64_t tsc(void)
{
    uint32_t lo, hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* Relative cost of a hit and a miss (TSC cycles under QEMU TCG: only the ratio is meaningful). */
static void test_live_timing(void)
{
    blk_id_t id = vblk_info()->blk_id;
    uint64_t t, miss, hit;
    unsigned ok = 1;

    blk_invalidate(id);
    mark();
    t = tsc();
    for (uint64_t l = 0; l < BLK_CACHE_LINES; l++)
        ok &= blk_read(id, l * 8 + 3, 1, big, sizeof(big)) == BLK_OK;
    miss = tsc() - t;
    t = tsc();
    for (uint64_t l = 0; l < BLK_CACHE_LINES; l++)
        ok &= blk_read(id, l * 8 + 3, 1, big, sizeof(big)) == BLK_OK;
    hit = tsc() - t;
    serial_puts("blk: timing over 32 one-sector reads: miss ");
    serial_dec(miss / BLK_CACHE_LINES);
    serial_puts(" TSC cycles each, hit ");
    serial_dec(hit / BLK_CACHE_LINES);
    serial_puts(" (QEMU TCG; ratio only)\r\n");
    expect(ok && D(misses) == BLK_CACHE_LINES && D(hits) == BLK_CACHE_LINES && hit < miss,
           "32 misses then the same 32 sectors as hits; the hits cost less than the misses");
}

static void test_lifecycle(void)
{
    const struct vblk_info *in = vblk_info();
    blk_id_t old = in->blk_id;
    uint64_t f_on = pmm_free_count();
    unsigned ok = 1;
    uint64_t f_off = 0;

    for (unsigned i = 0; i < 5; i++) {
        blk_id_t before = in->blk_id;

        ok &= vblk_shutdown() == VBLK_OK && in->state == VBLK_STOPPED && blk_count() == 0 && !in->pool_held &&
              blk_read(before, 0, 1, big, sizeof(big)) == BLK_ENODEV &&
              vblk_test_device_status() == 0;
        if (i == 0)
            f_off = pmm_free_count();
        ok &= pmm_free_count() == f_off;
        ok &= vblk_reinit() == VBLK_READY && in->blk_id != 0 && in->blk_id != before && disk_ok(in->blk_id, 2047, 4) &&
              pmm_free_count() == f_on;
    }
    expect(ok && f_off == f_on + VBLK_POOL_FRAMES + BLK_CACHE_LINES,
           "5 shutdown/reinitialization cycles: 6 DMA + 32 cache frames returned and taken again, new IDs, data exact");
    expect(blk_read(old, 0, 1, big, sizeof(big)) == BLK_ENODEV && vblk_audit() == NULL && blk_audit() == NULL,
           "the first ID stays dead; audits clean");
}

/* Two readers and three user programs at once; the driver yields while it waits. */
static struct process *const P = selftest_procs;
static volatile unsigned rd_done, rd_bad;
#define READER_MAX  24                      /* sectors per reader request */
static uint8_t rbuf[2][READER_MAX * BLK_SECTOR_SIZE];

static void live_reader(void *arg)
{
    unsigned k = (unsigned)(uint64_t)arg;
    uint64_t x = 99 + k;

    for (unsigned i = 0; i < 150; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t c = 1 + (uint32_t)(x >> 58) % READER_MAX;
        uint64_t lba = (x >> 20) % (VBLK_DISK_SECTORS - c + 1);

        if (blk_read(vblk_info()->blk_id, lba, c, rbuf[k], sizeof(rbuf[k])) != BLK_OK ||
            memcmp(rbuf[k], fixture + lba * BLK_SECTOR_SIZE, (uint64_t)c * BLK_SECTOR_SIZE))
            rd_bad++;
        thread_yield();
    }
    rd_done++;
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

static void test_live_concurrent(void)
{
    uint64_t f0 = pmm_free_count(), t0 = irq_ticks();
    unsigned threads0 = thread_count(), killed = 0;
    uint64_t cf[BLK_CACHE_LINES];
    unsigned ncf = blk_cache_frames(cf, BLK_CACHE_LINES), shared = 0;

    blk_invalidate(vblk_info()->blk_id);
    mark();
    vblk_test_yield_polls(64);              /* yield often, so that waits overlap */
    proc_test_tick_budget(3000);
    int ok = launch(&P[0], "m-memtag1") && launch(&P[1], "m-memtag2") && launch(&P[2], "m-irqregs");
    rd_done = rd_bad = 0;
    ok &= thread_create("blk-r0", live_reader, (void *)0) > 0 && thread_create("blk-r1", live_reader, (void *)1) > 0;
    for (unsigned w = 0; ok && rd_done < 2 && w < 2000; w += 10)
        thread_sleep(10);
    thread_sleep(10);
    uint64_t ticks[3], pre[3];
    for (unsigned i = 0; i < 3; i++)
        proc_kill(&P[i], "test kill");
    for (unsigned i = 0; i < 3; i++) {
        killed += proc_wait(&P[i]) == PROC_KILLED && streq(P[i].reason, "test kill");
        ticks[i] = P[i].ticks;
        pre[i] = P[i].preemptions;
        for (unsigned j = 0; j < P[i].npages; j++)
            for (unsigned c = 0; c < ncf; c++)
                shared += P[i].page_pa[j] == cf[c];
        proc_destroy(&P[i]);
    }
    proc_test_tick_budget(PROC_TEST_TICK_BUDGET);
    vblk_test_yield_polls(VBLK_YIELD_POLLS);
    expect(ok && rd_done == 2 && rd_bad == 0, "two kernel threads: 300 random reads through the cache, all exact");
    expect(killed == 3 && ticks[0] >= 4 && ticks[1] >= 4 && ticks[2] >= 4 && pre[0] && pre[1] && pre[2] && shared == 0,
           "three programs kept running beside them; none of their frames is a cache frame");
    expect(D(hits) > 0 && D(misses) > 0 && irq_ticks() - t0 >= 20, "both hits and misses occurred; the timer kept ticking");
    expect(pmm_free_count() == f0 && proc_count() == 0 && thread_count() == threads0 && !blk_audit() && !vblk_audit() &&
           !sched_audit() && !proc_audit(), "frames, records and threads back to baseline; all audits clean");
}
#endif /* !vblk_ioerr */

#ifdef VOS_TEST_CASE_vblk_ioerr
static void test_live_ioerr(void)
{
    blk_id_t id = vblk_info()->blk_id;
    const uint64_t e = VBLK_IOERR_SECTOR;

    blk_invalidate(id);
    mark();
    expect(blk_read(id, e, 1, big, sizeof(big)) == BLK_EIO && blk_test_line_state(id, e / 8) == BLK_LINE_FREE &&
           D(load_errors) == 1, "the injected error at sector 1000 fails its line load: BLK_EIO, nothing cached");
    expect(disk_ok(id, e + 1, 7) && disk_ok(id, e - 8, 8) && blk_test_line_state(id, e / 8) == BLK_LINE_FREE &&
           blk_test_line_state(id, e / 8 - 1) == BLK_LINE_VALID,
           "sectors 1001-1007 of the same line are read one by one (uncached); line 124 is cached normally");
    expect(blk_read(id, e - 2, 4, big, sizeof(big)) == BLK_EIO && blk_read_block(id, 4096, e / 8, big, 4096) == BLK_EIO,
           "requests covering sector 1000 fail, also as a 4 KiB block");
    expect(disk_ok(id, 0, 64) && disk_ok(id, VBLK_DISK_SECTORS - 3, 3) && blk_audit() == NULL && vblk_audit() == NULL &&
           vblk_info()->state == VBLK_READY, "elsewhere the disk reads exactly; the device stays ready; audits clean");
}
#endif
#endif /* VBLK_LIVE */

int selftest_blk(const struct boot_info *bi)
{
    failures = cases = 0;
    mbi = bi;
    S = blk_stats();
    expect(streq(blk_strerror(BLK_EIO), "device I/O error") && streq(blk_strerror(BLK_EUNSUPP), "operation not supported") &&
           streq(blk_strerror(99), "unknown error"), "blk_strerror names the codes");
#if !defined(VBLK_LIVE) && !defined(VOS_TEST_CASE_ext2)
    expect(blk_count() == 0 && blk_nth(0) == 0 && blk_cache_frames(0, 0) == 0 && blk_audit() == NULL,
           "no usable disk: no device registered, no cache frames");
#endif
    test_ram();
#ifdef VBLK_LIVE
    if (find_fixture() && vblk_info()->state == VBLK_READY) {
        test_live_device();
#ifdef VOS_TEST_CASE_vblk_ioerr
        test_live_ioerr();
#else
        test_live_cache();
        test_live_timing();
        test_lifecycle();
        test_live_concurrent();
#endif
    } else {
        expect(0, "virtio-blk device ready with its fixture");
    }
#endif
    const struct blk_stats *s = blk_stats();
    serial_puts("blk: ");
    serial_dec(s->requests);
    serial_puts(" requests, ");
    serial_dec(s->hits);
    serial_puts(" hits, ");
    serial_dec(s->misses);
    serial_puts(" misses, ");
    serial_dec(s->device_reads);
    serial_puts(" device reads, ");
    serial_dec(s->evictions);
    serial_puts(" evictions, ");
    serial_dec(s->load_errors);
    serial_puts(" load errors, ");
    serial_dec(s->fallback_reads);
    serial_puts(" fallback reads, ");
    serial_dec(s->stale_drops);
    serial_puts(" stale, ");
    serial_dec(s->corrupt_drops);
    serial_puts(" corrupt, ");
    serial_dec(s->load_waits);
    serial_puts(" load waits, ");
    serial_dec(s->busy);
    serial_puts(" busy, ");
    serial_dec(s->refused);
    serial_puts(" refused\r\n");
    serial_puts("selftest: blk ");
    serial_dec(cases);
    serial_puts(" cases, ");
    serial_dec(failures);
    serial_puts(" failed\r\n");
    return failures;
}
