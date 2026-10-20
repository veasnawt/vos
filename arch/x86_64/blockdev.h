/*
 * Block I/O layer (N3.2, storage.md layers 2-3): a bounded registry of block
 * devices, device-independent errors, validated read-only requests and a
 * bounded read-only cache in PMM frames.
 *
 * Read-only by construction: there is no write path. blk_write exists only
 * to refuse writes explicitly (BLK_EUNSUPP).
 *
 * Devices. A driver registers a device with an ops table (struct blk_ops);
 * the layer never sees driver structures. Each registered device has an ID
 * that encodes its registry slot and a finite generation. Numeric IDs can
 * alias after legacy generation rollover; long-lived file bindings also
 * compare a never-reused 64-bit registration incarnation.
 * The driver reports state, capacity and an epoch on every request
 * (blk_ops.info); the epoch must change whenever the device was reset or
 * reinitialized, which invalidates everything cached for it.
 *
 * Cache. BLK_CACHE_LINES lines of BLK_LINE_SECTORS sectors (one 4 KiB PMM
 * frame each), allocated when the first device registers and freed when
 * the last one unregisters. Line n of a device holds sectors
 * [8n, 8n + 8) clipped to the capacity (the last line may be shorter).
 * Lines are FREE, LOADING (being filled by one thread; never returned,
 * never evicted) or VALID (key: device slot, generation, epoch, line;
 * checksum verified on every hit). Replacement: a free line first, else
 * the least recently used valid line that is not pinned. Data is always
 * copied into the caller's validated buffer; no pointer to cache storage
 * is ever handed out. A failed load leaves the line FREE. When a whole-line
 * load fails with an I/O error, the requested sectors are read one by one,
 * uncached, so only the sectors that really fail return BLK_EIO.
 *
 * Concurrency. The kernel is not preemptible and runs with IF = 0, so the
 * layer's state changes are atomic between explicit waits. A thread that
 * needs a line another thread is loading blocks on a wait queue until the
 * load ends. The driver may block or yield while it waits for its device.
 * N3.4: a thread interrupted while it waits (thread_interrupt) gets
 * BLK_EINTR; a line it was loading is left FREE and the threads waiting for
 * it are woken, as after any failed load.
 */
#ifndef VOS_X86_64_BLOCKDEV_H
#define VOS_X86_64_BLOCKDEV_H

#include <stdint.h>

#define BLK_SECTOR_SIZE     512
#define BLK_MAX_DEVICES     4
#define BLK_MAX_REQUEST     64          /* sectors per blk_read call (32 KiB) */
#define BLK_LINE_SECTORS    8           /* one cache line = one 4 KiB frame */
#define BLK_CACHE_LINES     32          /* 128 KiB of PMM frames */

/* Device-independent errors. */
#define BLK_OK          0
#define BLK_EINVAL      1   /* malformed request: count, block size, buffer or capacity */
#define BLK_ERANGE      2   /* sectors outside the device */
#define BLK_ENODEV      3   /* no registered device with this ID */
#define BLK_EUNAVAIL    4   /* the device exists but cannot serve requests (failed, reset, stopped) */
#define BLK_EBUSY       5   /* a bounded resource is exhausted (request slots, cache lines, registry) */
#define BLK_EIO         6   /* the device failed the transfer or misbehaved */
#define BLK_ETIMEDOUT   7   /* the device did not complete in time (it was reset) */
#define BLK_EUNSUPP     8   /* the operation is not supported (every write) */
#define BLK_EINTR       9   /* N3.4: the waiting thread was interrupted; its request was abandoned */

/* Device state as the driver reports it. */
#define BLK_DEV_READY       1
#define BLK_DEV_UNAVAILABLE 2

/* Capabilities. */
#define BLK_CAP_READ        1u
#define BLK_CAP_DEVICE_RO   2u      /* the device itself refuses writes */

typedef uint32_t blk_id_t;          /* 0 is never a valid ID */

struct blk_info {
    int state;                      /* BLK_DEV_* */
    uint64_t sectors;               /* capacity in 512-byte sectors */
    uint32_t max_sectors;           /* largest request the driver accepts (>= BLK_LINE_SECTORS) */
    unsigned caps;                  /* BLK_CAP_* */
    uint64_t epoch;                 /* changes on every reset or reinitialization */
};

struct blk_ops {
    const char *driver;
    void (*info)(void *ctx, struct blk_info *out);
    /* Read sectors [lba, lba + count) into the kernel buffer buf (cap bytes). BLK_*. */
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf, uint64_t cap);
    /* Nonzero if the frame at pa is private to the driver (e.g. DMA): never a caller buffer. */
    int (*owns_frame)(void *ctx, uint64_t pa);
};

struct blk_desc {
    const char *name, *driver;
    int state;
    uint64_t sectors, epoch;
    uint32_t sector_size, max_request;
    unsigned caps;
    uint64_t reset_seq;             /* additive reset observation; epoch semantics unchanged */
    uint64_t incarnation;           /* never reused, even when the legacy ID generation wraps */
};

struct blk_stats {
    uint64_t requests, hits, misses, device_reads, evictions, load_errors, fallback_reads;
    uint64_t busy, invalidated, stale_drops, corrupt_drops, load_waits, refused;
};

/* Registry. */
int blk_register(const char *name, const struct blk_ops *ops, void *ctx, blk_id_t *id);
int blk_unregister(blk_id_t id);
unsigned blk_count(void);
/* The ID of the n-th registered device (0-based), or 0. */
blk_id_t blk_nth(unsigned n);
int blk_describe(blk_id_t id, struct blk_desc *d);
/* Driver notification before reset waiters wake; no I/O or cache mutation. */
void blk_note_reset(blk_id_t id);

/* Reads (through the cache). buf must be kernel memory: present, writable, supervisor. */
int blk_read(blk_id_t id, uint64_t lba, uint32_t count, void *buf, uint64_t cap);
/* One file-system block of block_size bytes (512, 1024, 2048 or 4096) at block number block. */
int blk_read_block(blk_id_t id, uint32_t block_size, uint64_t block, void *buf, uint64_t cap);
/* Always BLK_EUNSUPP for a valid ID (BLK_ENODEV otherwise): the layer is read-only. */
int blk_write(blk_id_t id, uint64_t lba, uint32_t count, const void *buf, uint64_t len);

/* Drop every cached line of the device (0: of every device). BLK_EBUSY if one is in use. */
int blk_invalidate(blk_id_t id);

const struct blk_stats *blk_stats(void);
const char *blk_strerror(int err);
/* Consistency of registry and cache; NULL if consistent, else the first problem. */
const char *blk_audit(void);
/* The cache's PMM frames (0 if no cache exists); returns how many were stored. */
unsigned blk_cache_frames(uint64_t *pa, unsigned max);

#ifdef VOS_TEST
#define BLK_LINE_FREE       0
#define BLK_LINE_LOADING    1
#define BLK_LINE_VALID      2
/* State of the cache line holding line n of device id (BLK_LINE_FREE if not cached). */
int blk_test_line_state(blk_id_t id, uint64_t line);
/* Pin (pin = 1) or unpin (0) the cached line n of device id; 0 or BLK_* error. */
int blk_test_pin(blk_id_t id, uint64_t line, int pin);
/* Flip one byte of the cached line n of device id (checksum test). */
int blk_test_corrupt(blk_id_t id, uint64_t line);
/* Lines currently VALID. */
unsigned blk_test_valid_lines(void);
/* Simulate legacy numeric ID reuse on an empty slot; incarnation still advances. */
int blk_test_reuse_id(blk_id_t retired);
uint64_t blk_test_issue_incarnation(uint64_t *serial);
#endif

#endif
