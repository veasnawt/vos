#include "pmm.h"
#include "lib.h"
#include "panic.h"

static uint64_t count_bits(uint64_t x)  /* no popcnt: the build targets baseline x86_64 */
{
    uint64_t n = 0;

    for (; x; x &= x - 1)
        n++;
    return n;
}

static int test_bit(const uint64_t *bm, uint64_t frame)
{
    return (bm[frame / 64] >> (frame % 64)) & 1;
}

static void set_frames(uint64_t *bm, uint64_t first, uint64_t end)
{
    for (uint64_t f = first; f < end; f++)
        bm[f / 64] |= 1ULL << (f % 64);
}

static void clear_frames(uint64_t *bm, uint64_t first, uint64_t end)
{
    for (uint64_t f = first; f < end; f++)
        bm[f / 64] &= ~(1ULL << (f % 64));
}

/*
 * Remove [start, end) from the allocatable set, rounded OUTWARD to frames and
 * clipped to the tracked range. `end` may be UINT64_MAX meaning "to 2^64".
 */
static void protect(struct pmm *p, uint64_t start, uint64_t end, uint64_t limit)
{
    if (start >= limit || start >= end)
        return;
    uint64_t e = end >= limit ? limit : (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    clear_frames(p->avail, start / PAGE_SIZE, e / PAGE_SIZE);
}

int pmm_init_in(struct pmm *p, uint64_t *avail, uint64_t *alloc, uint64_t frames,
                const struct boot_info *bi, const struct resv_table *t)
{
    memset(p, 0, sizeof(*p));
    p->state = PMM_FAILED;
    if (!avail || !alloc || frames == 0 || frames % 64 != 0 || frames > PMM_MAX_FRAMES)
        return PMM_EINVAL;
    if (!bi || !bi->mmap || bi->mmap_entries == 0 || bi->mmap->entry_version != 0 ||
        bi->mmap->entry_size < sizeof(struct mb2_mmap_entry) || bi->mmap->entry_size % 8 != 0)
        return PMM_EMMAP;

    uint64_t limit = frames * PAGE_SIZE;

    /* 1. Every frame starts unavailable and unallocated. */
    p->avail = avail;
    p->alloc = alloc;
    p->frames = frames;
    memset(avail, 0, PMM_WORDS(frames) * sizeof(uint64_t));
    memset(alloc, 0, PMM_WORDS(frames) * sizeof(uint64_t));

    /* 2. Free pass: whole frames inside available entries, within [1 MiB, limit). */
    for (uint32_t i = 0; i < bi->mmap_entries; i++) {
        const struct mb2_mmap_entry *e = boot_mmap_entry(bi, i);
        uint64_t end = e->base + e->length;

        if (e->type != MB2_MMAP_AVAILABLE || e->length == 0 || end < e->base)
            continue;                   /* not RAM, empty, or overflowing: never free */
        if (end > limit)
            p->ignored_bytes += end - (e->base > limit ? e->base : limit);
        uint64_t s = e->base < PMM_LOW_LIMIT ? PMM_LOW_LIMIT : e->base;
        if (end > limit)
            end = limit;
        if (s >= end)
            continue;
        s = (s + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);     /* inward: s < limit, no overflow */
        end &= ~(PAGE_SIZE - 1);
        if (s < end)
            set_frames(avail, s / PAGE_SIZE, end / PAGE_SIZE);
    }

    /* 3. Protect pass: every non-available entry, of any type, wins. */
    for (uint32_t i = 0; i < bi->mmap_entries; i++) {
        const struct mb2_mmap_entry *e = boot_mmap_entry(bi, i);
        uint64_t end = e->base + e->length;

        if (e->type == MB2_MMAP_AVAILABLE || e->length == 0)
            continue;
        if (end < e->base)
            end = UINT64_MAX;           /* overflowing: extends to 2^64 */
        protect(p, e->base & ~(PAGE_SIZE - 1), end, limit);
    }

    /* 4. Reservation pass (the table is a union; overlaps are harmless). */
    for (size_t i = 0; t && i < t->count; i++)
        protect(p, t->r[i].start & ~(PAGE_SIZE - 1), t->r[i].end, limit);

    /* 5. Defensive: nothing below 1 MiB, whatever the inputs said. */
    clear_frames(avail, 0, PMM_LOW_LIMIT / PAGE_SIZE);

    for (uint64_t w = 0; w < PMM_WORDS(frames); w++)
        p->usable += count_bits(avail[w]);
    p->free = p->usable;
    p->hint = 0;
    p->state = PMM_READY;
    return PMM_OK;
}

int pmm_alloc_in(struct pmm *p, uint64_t *phys)
{
    *phys = 0;
    if (p->state != PMM_READY)
        return PMM_ENOTREADY;

    for (uint64_t w = p->hint; w < PMM_WORDS(p->frames); w++) {
        uint64_t m = p->avail[w] & ~p->alloc[w];

        if (!m)
            continue;
        uint64_t frame = w * 64 + (uint64_t)__builtin_ctzll(m);
        if (p->free == 0 || frame * PAGE_SIZE < PMM_LOW_LIMIT || frame >= p->frames) {
            p->state = PMM_FAILED;
            return PMM_ECORRUPT;
        }
        p->alloc[w] |= 1ULL << (frame % 64);
        p->free--;
        p->hint = w;
        *phys = frame * PAGE_SIZE;
        return PMM_OK;
    }
    if (p->free != 0) {                 /* the count claims frames the bitmap lacks */
        p->state = PMM_FAILED;
        return PMM_ECORRUPT;
    }
    p->hint = PMM_WORDS(p->frames);
    return PMM_ENOMEM;
}

int pmm_free_check_in(const struct pmm *p, uint64_t phys)
{
    if (p->state != PMM_READY)
        return PMM_ENOTREADY;
    if (phys % PAGE_SIZE != 0)
        return PMM_EALIGN;
    if (phys >= p->frames * PAGE_SIZE)
        return PMM_ERANGE;
    if (!test_bit(p->avail, phys / PAGE_SIZE))
        return PMM_EPROTECTED;
    if (!test_bit(p->alloc, phys / PAGE_SIZE))
        return PMM_ENOTALLOC;
    if (p->free >= p->usable)
        return PMM_ECORRUPT;
    return PMM_OK;
}

int pmm_free_in(struct pmm *p, uint64_t phys)
{
    int err = pmm_free_check_in(p, phys);

    if (err != PMM_OK)
        return err;

    uint64_t frame = phys / PAGE_SIZE;
    p->alloc[frame / 64] &= ~(1ULL << (frame % 64));
    p->free++;
    if (frame / 64 < p->hint)
        p->hint = frame / 64;
    return PMM_OK;
}

int pmm_reserve_in(struct pmm *p, uint64_t phys)
{
    if (p->state != PMM_READY)
        return PMM_ENOTREADY;
    if (phys % PAGE_SIZE != 0)
        return PMM_EALIGN;
    if (phys >= p->frames * PAGE_SIZE)
        return PMM_ERANGE;
    uint64_t frame = phys / PAGE_SIZE;
    if (!test_bit(p->avail, frame))
        return PMM_EPROTECTED;
    if (test_bit(p->alloc, frame))
        return PMM_EINUSE;
    if (p->free == 0)
        return PMM_ECORRUPT;
    p->alloc[frame / 64] |= 1ULL << (frame % 64);
    p->free--;
    return PMM_OK;
}

int pmm_frame_state_in(const struct pmm *p, uint64_t phys)
{
    uint64_t frame = phys / PAGE_SIZE;

    if (p->state != PMM_READY || frame >= p->frames || !test_bit(p->avail, frame))
        return PMM_FRAME_UNAVAILABLE;
    return test_bit(p->alloc, frame) ? PMM_FRAME_ALLOCATED : PMM_FRAME_FREE;
}

int pmm_check_in(const struct pmm *p)
{
    uint64_t usable = 0, free = 0;

    if (p->state != PMM_READY)
        return PMM_ECORRUPT;
    for (uint64_t w = 0; w < PMM_WORDS(p->frames); w++) {
        uint64_t m = p->avail[w] & ~p->alloc[w];

        if (p->alloc[w] & ~p->avail[w])
            return PMM_ECORRUPT;        /* allocated frame that was never allocatable */
        if (w < PMM_LOW_LIMIT / PAGE_SIZE / 64 && p->avail[w])
            return PMM_ECORRUPT;        /* allocatable frame below 1 MiB */
        if (w < p->hint && m)
            return PMM_ECORRUPT;        /* free frame below the hint */
        usable += count_bits(p->avail[w]);
        free += count_bits(m);
    }
    if (usable != p->usable || free != p->free || free > usable)
        return PMM_ECORRUPT;
    return PMM_OK;
}

/* ---- the kernel allocator ---- */

/* In .bss, hence inside the reserved kernel image: the bitmaps can never be handed out. */
static uint64_t kernel_avail[PMM_WORDS(PMM_MAX_FRAMES)];
static uint64_t kernel_alloc[PMM_WORDS(PMM_MAX_FRAMES)];
static struct pmm kernel_pmm;

int pmm_init(const struct boot_info *bi, const struct resv_table *t)
{
    if (kernel_pmm.state != PMM_UNINIT)
        return PMM_EINVAL;
    return pmm_init_in(&kernel_pmm, kernel_avail, kernel_alloc, PMM_MAX_FRAMES, bi, t);
}

int pmm_alloc_frame(uint64_t *phys)
{
    int err = pmm_alloc_in(&kernel_pmm, phys);

    if (err == PMM_ECORRUPT)
        panic("pmm: allocator state corrupt");
    if (err == PMM_OK)
        memset(phys_to_virt(*phys), 0, PAGE_SIZE);      /* *phys < 1 GiB: mapped */
    return err;
}

int pmm_free_frame(uint64_t phys)
{
    int err = pmm_free_check_in(&kernel_pmm, phys);

    if (err == PMM_ECORRUPT)
        panic("pmm: allocator state corrupt");
    if (err != PMM_OK)
        return err;
#ifdef VOS_TEST
    memset(phys_to_virt(phys), PMM_POISON, PAGE_SIZE);  /* still owned by the caller */
#endif
    return pmm_free_in(&kernel_pmm, phys);
}

int pmm_reserve_frame(uint64_t phys)
{
    int err = pmm_reserve_in(&kernel_pmm, phys);

    if (err == PMM_ECORRUPT)
        panic("pmm: allocator state corrupt");
    return err;     /* the frame is neither zeroed nor touched: firmware may own it */
}

uint64_t pmm_free_count(void)
{
    return kernel_pmm.free;
}

const struct pmm *pmm_kernel(void)
{
    return &kernel_pmm;
}

const char *pmm_strerror(int err)
{
    static const char *const names[] = {
        "ok", "not ready", "out of memory", "unaligned address", "address out of range",
        "protected frame", "frame not allocated", "memory map unsupported",
        "invalid argument", "allocator state corrupt", "frame already allocated",
    };

    return err >= 0 && err < (int)(sizeof(names) / sizeof(names[0])) ? names[err] : "?";
}
