#include "blockdev.h"
#include "io_scope.h"
#include "panic.h"
#include "lib.h"
#include "memlayout.h"
#include "pmm.h"
#include "thread.h"
#include "vmm.h"

#define LINE_FREE       0
#define LINE_LOADING    1
#define LINE_VALID      2
#define ID_SLOT_MASK    0xFu
#define ID_GEN_SHIFT    4
#define GEN_MASK        0x0FFFFFFFu
#define LINE_ATTEMPTS   8               /* corrupt-line reloads per request before giving up */
#define LINE_WAITS      1024            /* waits for other threads' loads per request before giving up */
#define LINE_BYTES      (BLK_LINE_SECTORS * BLK_SECTOR_SIZE)

_Static_assert(LINE_BYTES == PAGE_SIZE, "a cache line is one frame");
_Static_assert(BLK_MAX_DEVICES < ID_SLOT_MASK, "slot must fit the ID");

struct dev {
    int used;
    uint32_t gen;                       /* part of the ID; changes when the slot is released */
    const char *name;
    const struct blk_ops *ops;
    void *ctx;
    uint64_t reset_seq;
    uint64_t incarnation;
};

struct line {
    int state;
    unsigned slot;                      /* key: registry slot, its generation, the device epoch, line number */
    uint32_t gen;
    uint64_t epoch, n;
    uint32_t nsect;                     /* sectors held: BLK_LINE_SECTORS, fewer for the device's last line */
    uint32_t pins;
    uint64_t last_use, sum, frame;
};

static struct dev devs[BLK_MAX_DEVICES];
static uint32_t next_gen[BLK_MAX_DEVICES];
static uint64_t next_incarnation = 1;
static unsigned ndev;
static struct line cache[BLK_CACHE_LINES];
static int cache_ready;
static uint64_t use_clock;
static struct blk_stats st;
static struct wait_queue load_wq;

const char *blk_strerror(int err)
{
    static const char *const s[] = {
        "ok", "invalid request", "sectors outside the device", "no such device", "device unavailable",
        "resources exhausted", "device I/O error", "timed out", "operation not supported", "interrupted",
    };

    return err >= 0 && err <= BLK_EINTR ? s[err] : "unknown error";
}

const struct blk_stats *blk_stats(void)
{
    return &st;
}

/* ---- registry ---- */

static struct dev *lookup(blk_id_t id, unsigned *slot)
{
    unsigned s = id & ID_SLOT_MASK;

    if (s == 0 || s > BLK_MAX_DEVICES)
        return NULL;
    s--;
    if (!devs[s].used || devs[s].gen != id >> ID_GEN_SHIFT)
        return NULL;
    if (slot)
        *slot = s;
    return &devs[s];
}

static void free_cache(void)
{
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++) {
        if (cache[i].frame && pmm_free_frame(cache[i].frame) != PMM_OK)
            continue;                   /* never happens for our own frames; the audit would see it */
        cache[i].frame = 0;
        cache[i].state = LINE_FREE;
    }
    cache_ready = 0;
}

static int alloc_cache(void)
{
    memset(cache, 0, sizeof(cache));
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        if (pmm_alloc_frame(&cache[i].frame) != PMM_OK) {
            cache[i].frame = 0;
            free_cache();
            return 0;
        }
    cache_ready = 1;
    return 1;
}

static uint64_t issue_incarnation(uint64_t *serial)
{
    if (!*serial || *serial == UINT64_MAX)
        return 0;
    return (*serial)++;
}

int blk_register(const char *name, const struct blk_ops *ops, void *ctx, blk_id_t *id)
{
    struct blk_info in;
    unsigned s;

    if (!name || !ops || !ops->info || !ops->read || !id)
        return BLK_EINVAL;
    memset(&in, 0, sizeof(in));
    ops->info(ctx, &in);
    if (in.sectors == 0 || in.max_sectors < BLK_LINE_SECTORS || !(in.caps & BLK_CAP_READ))
        return BLK_EINVAL;
    if (!next_incarnation || next_incarnation == UINT64_MAX)
        return BLK_EBUSY;               /* fail before allocating any cache frame */
    for (s = 0; s < BLK_MAX_DEVICES && devs[s].used; s++)
        ;
    if (s == BLK_MAX_DEVICES)
        return BLK_EBUSY;
    if (!cache_ready && !alloc_cache())
        return BLK_EBUSY;
    if (!next_gen[s])
        next_gen[s] = 1;
    devs[s].used = 1;
    devs[s].gen = next_gen[s];
    devs[s].name = name;
    devs[s].ops = ops;
    devs[s].ctx = ctx;
    devs[s].reset_seq = 0;
    devs[s].incarnation = issue_incarnation(&next_incarnation);
    ndev++;
    *id = devs[s].gen << ID_GEN_SHIFT | (s + 1);
    return BLK_OK;
}

static int lines_busy(unsigned slot, int all)
{
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        if ((all || cache[i].slot == slot) && cache[i].state != LINE_FREE &&
            (cache[i].state == LINE_LOADING || cache[i].pins))
            return 1;
    return 0;
}

static unsigned drop_lines(unsigned slot, int all)
{
    unsigned n = 0;

    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        if ((all || cache[i].slot == slot) && cache[i].state == LINE_VALID) {
            cache[i].state = LINE_FREE;
            n++;
        }
    return n;
}

int blk_unregister(blk_id_t id)
{
    unsigned s;

    if (!lookup(id, &s))
        return BLK_ENODEV;
    if (lines_busy(s, 0))
        return BLK_EBUSY;
    st.invalidated += drop_lines(s, 0);
    devs[s].used = 0;
    next_gen[s] = (devs[s].gen + 1) & GEN_MASK;
    if (!next_gen[s])
        next_gen[s] = 1;
    if (--ndev == 0)
        free_cache();
    return BLK_OK;
}

unsigned blk_count(void)
{
    return ndev;
}

blk_id_t blk_nth(unsigned n)
{
    for (unsigned s = 0; s < BLK_MAX_DEVICES; s++)
        if (devs[s].used && n-- == 0)
            return devs[s].gen << ID_GEN_SHIFT | (s + 1);
    return 0;
}

int blk_describe(blk_id_t id, struct blk_desc *d)
{
    struct dev *v = lookup(id, NULL);
    struct blk_info in;

    if (!v || !d)
        return v ? BLK_EINVAL : BLK_ENODEV;
    v->ops->info(v->ctx, &in);
    d->name = v->name;
    d->driver = v->ops->driver;
    d->state = in.state;
    d->sectors = in.sectors;
    d->epoch = in.epoch;
    d->sector_size = BLK_SECTOR_SIZE;
    d->max_request = BLK_MAX_REQUEST;
    d->caps = in.caps;
    d->reset_seq = v->reset_seq;
    d->incarnation = v->incarnation;
    return BLK_OK;
}

void blk_note_reset(blk_id_t id)
{
    struct dev *v = lookup(id, NULL);
    if (!v)
        return;
    if (v->reset_seq == UINT64_MAX)
        panic("blk: reset sequence exhausted");
    v->reset_seq++;
}

/* ---- validation ---- */

static int is_cache_frame(uint64_t pa)
{
    for (unsigned i = 0; cache_ready && i < BLK_CACHE_LINES; i++)
        if (cache[i].frame == pa)
            return 1;
    return 0;
}

/* Every page of [buf, buf + len) present, writable, supervisor, and neither cache nor driver memory. */
static int buffer_ok(const struct dev *d, const void *buf, uint64_t len)
{
    uint64_t va = (uint64_t)buf, first = va & ~(PAGE_SIZE - 1);

    if (!buf || len == 0 || va < KERNEL_VMA || va + len - 1 < va)
        return 0;
    for (uint64_t p = first; p >= first && p < va + len; p += PAGE_SIZE) {
        struct vmm_mapping m;
        uint64_t pa;

        if (vmm_lookup(vmm_kernel_root(), p, &m) != VMM_OK || !(m.flags & VMM_WRITE) || (m.flags & VMM_USER))
            return 0;
        pa = m.pa & ~(PAGE_SIZE - 1);
        if (is_cache_frame(pa) || (d->ops->owns_frame && d->ops->owns_frame(d->ctx, pa)))
            return 0;
    }
    return 1;
}

static uint64_t checksum(const struct line *e)
{
    const uint64_t *w = phys_to_virt(e->frame);
    uint64_t h = 0xCBF29CE484222325ULL;

    for (unsigned i = 0; i < e->nsect * BLK_SECTOR_SIZE / 8; i++)
        h = (h ^ w[i]) * 0x100000001B3ULL;
    return h;
}

/* ---- cache ---- */

static struct line *find(unsigned slot, uint32_t gen, uint64_t epoch, uint64_t n)
{
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++) {
        struct line *e = &cache[i];

        if (e->state == LINE_FREE || e->slot != slot || e->gen != gen)
            continue;
        if (e->epoch != epoch) {            /* cached before a reset or reinitialization */
            if (e->state == LINE_VALID && !e->pins) {
                e->state = LINE_FREE;
                st.stale_drops++;
            }
            continue;
        }
        if (e->n == n)
            return e;
    }
    return NULL;
}

/* A free line, else the least recently used valid line that is not pinned; NULL if none. */
static struct line *victim(void)
{
    struct line *best = NULL;

    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        if (cache[i].state == LINE_FREE)
            return &cache[i];
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        if (cache[i].state == LINE_VALID && !cache[i].pins && (!best || cache[i].last_use < best->last_use))
            best = &cache[i];
    return best;
}

/* After a whole-line load failed with BLK_EIO: the requested sectors one by one, uncached. */
static int backend_read(struct dev *d, uint64_t lba, uint32_t count, void *buf, uint64_t cap)
{
    int r = io_scope_read();
    if (r)
        return r == IO_SCOPE_TIMEOUT ? BLK_ETIMEDOUT : r == IO_SCOPE_BUDGET ? BLK_EBUSY : BLK_EINTR;
    int err = d->ops->read(d->ctx, lba, count, buf, cap);
    r = io_scope_check();
    return r ? (r == IO_SCOPE_TIMEOUT ? BLK_ETIMEDOUT : r == IO_SCOPE_BUDGET ? BLK_EBUSY : BLK_EINTR) : err;
}

static int fallback(struct dev *d, uint64_t lba, uint32_t k, uint8_t *out)
{
    for (uint32_t i = 0; i < k; i++) {
        int err;

        st.fallback_reads++;
        if ((err = backend_read(d, lba + i, 1, out + i * BLK_SECTOR_SIZE, BLK_SECTOR_SIZE)))
            return err;
    }
    return BLK_OK;
}

/* Copy sectors [off, off + k) of line n into out, from the cache or after loading the line. */
static int line_read(struct dev *d, unsigned slot, struct blk_info *in, uint64_t n, uint32_t off, uint32_t k,
                     uint8_t *out)
{
    unsigned attempts = 0, waits = 0;

    while (attempts < LINE_ATTEMPTS) {
        int stopped = io_scope_check();
        if (stopped)
            return stopped == IO_SCOPE_TIMEOUT ? BLK_ETIMEDOUT : stopped == IO_SCOPE_BUDGET ? BLK_EBUSY : BLK_EINTR;
        struct line *e = find(slot, d->gen, in->epoch, n);

        if (e && e->state == LINE_LOADING) {
            if (++waits > LINE_WAITS)
                return BLK_EBUSY;
            uint32_t gen = d->gen;

            st.load_waits++;
            if (io_scope_wait(&load_wq, 0) == THREAD_INTERRUPTED)
                return BLK_EINTR;           /* N3.4: the loader goes on; this caller gives up */
            if (!d->used || d->gen != gen)
                return BLK_ENODEV;          /* unregistered while this thread waited */
            d->ops->info(d->ctx, in);
            if (in->state != BLK_DEV_READY)
                return BLK_EUNAVAIL;
            continue;
        }
        if (e) {
            if (off + k > e->nsect)
                return BLK_EINVAL;          /* never past the sectors the line holds */
            if (checksum(e) != e->sum) {
                st.corrupt_drops++;
                e->state = LINE_FREE;
                attempts++;
                continue;
            }
            st.hits++;
            e->last_use = ++use_clock;
            memcpy(out, (uint8_t *)phys_to_virt(e->frame) + off * BLK_SECTOR_SIZE, k * BLK_SECTOR_SIZE);
            return BLK_OK;
        }

        st.misses++;
        if (!(e = victim())) {
            st.busy++;
            return BLK_EBUSY;
        }
        if (e->state == LINE_VALID)
            st.evictions++;
        e->state = LINE_LOADING;
        e->slot = slot;
        e->gen = d->gen;
        e->epoch = in->epoch;
        e->n = n;
        e->nsect = in->sectors - n * BLK_LINE_SECTORS < BLK_LINE_SECTORS ?
                   (uint32_t)(in->sectors - n * BLK_LINE_SECTORS) : BLK_LINE_SECTORS;
        e->pins = 0;

        uint64_t epoch0 = in->epoch;
        st.device_reads++;
        int err = backend_read(d, n * BLK_LINE_SECTORS, e->nsect, phys_to_virt(e->frame), LINE_BYTES);
        d->ops->info(d->ctx, in);
        if (!err && (in->state != BLK_DEV_READY || in->epoch != epoch0))
            err = BLK_EUNAVAIL;             /* reset while loading: the data is not trusted */
        if (err) {
            e->state = LINE_FREE;
            st.load_errors++;
            thread_wake_all(&load_wq);
            return err == BLK_EIO ? fallback(d, n * BLK_LINE_SECTORS + off, k, out) : err;
        }
        e->sum = checksum(e);
        e->state = LINE_VALID;
        e->last_use = ++use_clock;
        thread_wake_all(&load_wq);
        memcpy(out, (uint8_t *)phys_to_virt(e->frame) + off * BLK_SECTOR_SIZE, k * BLK_SECTOR_SIZE);
        return BLK_OK;
    }
    return BLK_EIO;
}

static int refuse(int err)
{
    st.refused++;
    return err;
}

int blk_read(blk_id_t id, uint64_t lba, uint32_t count, void *buf, uint64_t cap)
{
    struct blk_info in;
    struct dev *d;
    unsigned slot;
    uint8_t *out = buf;

    st.requests++;
    if (!(d = lookup(id, &slot)))
        return refuse(BLK_ENODEV);
    d->ops->info(d->ctx, &in);
    if (in.state != BLK_DEV_READY)
        return refuse(BLK_EUNAVAIL);
    if (count == 0 || count > BLK_MAX_REQUEST || cap < (uint64_t)count * BLK_SECTOR_SIZE)
        return refuse(BLK_EINVAL);
    if (lba >= in.sectors || count > in.sectors - lba)
        return refuse(BLK_ERANGE);
    if (!buffer_ok(d, buf, (uint64_t)count * BLK_SECTOR_SIZE))
        return refuse(BLK_EINVAL);
    while (count) {
        uint64_t n = lba / BLK_LINE_SECTORS;
        uint32_t off = (uint32_t)(lba % BLK_LINE_SECTORS);
        uint32_t k = BLK_LINE_SECTORS - off < count ? BLK_LINE_SECTORS - off : count;
        int err = line_read(d, slot, &in, n, off, k, out);

        if (err)
            return err;
        out += (uint64_t)k * BLK_SECTOR_SIZE;
        lba += k;
        count -= k;
    }
    return BLK_OK;
}

int blk_read_block(blk_id_t id, uint32_t block_size, uint64_t block, void *buf, uint64_t cap)
{
    uint32_t spb = block_size / BLK_SECTOR_SIZE;

    if (block_size != 512 && block_size != 1024 && block_size != 2048 && block_size != 4096) {
        st.requests++;
        return refuse(lookup(id, NULL) ? BLK_EINVAL : BLK_ENODEV);
    }
    if (block > UINT64_MAX / spb) {
        st.requests++;
        return refuse(lookup(id, NULL) ? BLK_ERANGE : BLK_ENODEV);
    }
    return blk_read(id, block * spb, spb, buf, cap);
}

int blk_write(blk_id_t id, uint64_t lba, uint32_t count, const void *buf, uint64_t len)
{
    (void)lba, (void)count, (void)buf, (void)len;
    st.requests++;
    return refuse(lookup(id, NULL) ? BLK_EUNSUPP : BLK_ENODEV);
}

int blk_invalidate(blk_id_t id)
{
    unsigned s = 0;

    if (id && !lookup(id, &s))
        return BLK_ENODEV;
    if (lines_busy(s, id == 0))
        return BLK_EBUSY;
    st.invalidated += drop_lines(s, id == 0);
    return BLK_OK;
}

unsigned blk_cache_frames(uint64_t *pa, unsigned max)
{
    unsigned n = 0;

    for (unsigned i = 0; cache_ready && i < BLK_CACHE_LINES && n < max; i++)
        pa[n++] = cache[i].frame;
    return n;
}

const char *blk_audit(void)
{
    unsigned used = 0;

    for (unsigned s = 0; s < BLK_MAX_DEVICES; s++)
        if (devs[s].used) {
            used++;
            if (!devs[s].ops || !devs[s].ops->info || !devs[s].ops->read || !devs[s].gen ||
                !devs[s].incarnation || devs[s].incarnation >= next_incarnation)
                return "registered device without operations or valid identity";
            for (unsigned j = 0; j < s; j++)
                if (devs[j].used && devs[j].incarnation == devs[s].incarnation)
                    return "duplicate device incarnation";
        }
    if (used != ndev)
        return "device count differs from the registry";
    if (cache_ready != (ndev > 0))
        return "cache allocated without devices, or devices without cache";
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++) {
        const struct line *e = &cache[i];

        if (!cache_ready) {
            if (e->frame || e->state != LINE_FREE)
                return "cache line without a cache";
            continue;
        }
        if (pmm_frame_state_in(pmm_kernel(), e->frame) != PMM_FRAME_ALLOCATED)
            return "cache frame not allocated";
        for (unsigned j = 0; j < i; j++)
            if (cache[j].frame == e->frame)
                return "cache frame used twice";
        if (e->state == LINE_LOADING)
            return "line left loading";
        if (e->state != LINE_VALID)
            continue;
        if (e->slot >= BLK_MAX_DEVICES || !devs[e->slot].used || devs[e->slot].gen != e->gen)
            return "valid line of an unregistered device";
        if (e->nsect == 0 || e->nsect > BLK_LINE_SECTORS)
            return "valid line with an impossible sector count";
        struct blk_info in;
        devs[e->slot].ops->info(devs[e->slot].ctx, &in);
        if (e->epoch == in.epoch && e->n * BLK_LINE_SECTORS + e->nsect > in.sectors)
            return "valid line beyond the device capacity";
        if (checksum(e) != e->sum)
            return "valid line changed since it was loaded";
    }
    return NULL;
}

#ifdef VOS_TEST
static struct line *test_line(blk_id_t id, uint64_t n)
{
    struct dev *d;
    unsigned s;
    struct blk_info in;

    if (!(d = lookup(id, &s)))
        return NULL;
    d->ops->info(d->ctx, &in);
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        if (cache[i].state != LINE_FREE && cache[i].slot == s && cache[i].gen == d->gen &&
            cache[i].epoch == in.epoch && cache[i].n == n)
            return &cache[i];
    return NULL;
}

int blk_test_line_state(blk_id_t id, uint64_t line)
{
    struct line *e = test_line(id, line);
    return e ? e->state : BLK_LINE_FREE;
}

int blk_test_pin(blk_id_t id, uint64_t line, int pin)
{
    struct line *e = test_line(id, line);

    if (!e || e->state != LINE_VALID || (!pin && !e->pins))
        return BLK_EINVAL;
    e->pins += pin ? 1 : (uint32_t)-1;
    return BLK_OK;
}

int blk_test_corrupt(blk_id_t id, uint64_t line)
{
    struct line *e = test_line(id, line);

    if (!e || e->state != LINE_VALID)
        return BLK_EINVAL;
    ((volatile uint8_t *)phys_to_virt(e->frame))[100] ^= 0x5A;
    return BLK_OK;
}

unsigned blk_test_valid_lines(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < BLK_CACHE_LINES; i++)
        n += cache[i].state == LINE_VALID;
    return n;
}

int blk_test_reuse_id(blk_id_t retired)
{
    unsigned s = retired & ID_SLOT_MASK;
    uint32_t gen = retired >> ID_GEN_SHIFT;
    if (!s || s > BLK_MAX_DEVICES || !gen || devs[s - 1].used)
        return BLK_EINVAL;
    next_gen[s - 1] = gen;              /* simulate full legacy rollover without 2^28 cycles */
    return BLK_OK;
}

uint64_t blk_test_issue_incarnation(uint64_t *serial)
{
    return issue_incarnation(serial);
}
#endif

#ifdef VOS_TEST
#include "storage_observe.h"

void blk_test_observe(struct observe_block *out)
{
    out->ready = cache_ready;
    out->devices_count = ndev;
    out->head = (uint64_t)load_wq.head;
    out->tail = (uint64_t)load_wq.tail;
    out->stats = st;
    for (unsigned i = 0; i < BLK_MAX_DEVICES; i++) {
        out->devices[i].used = devs[i].used;
        out->devices[i].incarnation = devs[i].incarnation;
        out->devices[i].reset_seq = devs[i].reset_seq;
        out->devices[i].id = devs[i].used ? devs[i].gen << ID_GEN_SHIFT | (i + 1) : 0;
    }
    for (unsigned i = 0; i < BLK_CACHE_LINES; i++) {
        const struct line *e = &cache[i];
        out->lines[i] = (struct observe_cache_line){e->frame, e->epoch, e->n, e->sum,
            e->state, e->slot, e->gen, e->nsect, e->pins};
    }
}
#endif
