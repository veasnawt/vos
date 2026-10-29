/*
 * Physical page allocator: 4 KiB frames in [1 MiB, 1 GiB).
 *
 * Each frame has two bits:
 *   avail  1 = proven allocatable RAM. Set once by initialization, never
 *            changed afterwards. Frames below 1 MiB or at/above the limit,
 *            frames not wholly inside an available memory-map entry, frames
 *            touching any non-available entry, and reserved frames are 0.
 *   alloc  1 = currently allocated. Only ever set where avail is 1.
 * A frame is free when avail & ~alloc. Two bits are needed because a free
 * must tell "allocated" apart from "protected": one bit cannot.
 *
 * Allocation is first-fit from the lowest address (deterministic), using a
 * word hint below which no free frame exists. The kernel allocator zeroes
 * every frame it returns; in VOS_TEST builds a freed frame is filled with
 * PMM_POISON first.
 *
 * The *_in functions operate on any instance and never touch the frames
 * themselves, so self-tests can run them on synthetic maps.
 */
#ifndef VOS_X86_64_PMM_H
#define VOS_X86_64_PMM_H

#include <stdint.h>

#include "bootinfo.h"
#include "memlayout.h"
#include "resv.h"

#define PMM_LOW_LIMIT   0x100000ULL                     /* never allocate below 1 MiB */
#define PMM_LIMIT       BOOT_MAP_LIMIT                  /* never allocate at/above 1 GiB */
#define PMM_MAX_FRAMES  (PMM_LIMIT / PAGE_SIZE)         /* 262144 */
#define PMM_WORDS(n)    ((n) / 64)                      /* bitmap words for n frames */
#define PMM_POISON      0xA5

/* Results. */
#define PMM_OK          0
#define PMM_ENOTREADY   1   /* not initialized, or initialization failed */
#define PMM_ENOMEM      2   /* no free frame */
#define PMM_EALIGN      3   /* address not 4 KiB aligned */
#define PMM_ERANGE      4   /* address outside the tracked frames */
#define PMM_EPROTECTED  5   /* frame was never allocatable (reserved, unusable) */
#define PMM_ENOTALLOC   6   /* frame is not allocated: double free */
#define PMM_EMMAP       7   /* memory map missing or unsupported */
#define PMM_EINVAL      8   /* bad instance parameters or repeated init */
#define PMM_ECORRUPT    9   /* internal invariant broken */
#define PMM_EINUSE      10  /* frame already allocated: not reserved again */

#define PMM_UNINIT      0
#define PMM_READY       1
#define PMM_FAILED      2

/* Frame states for pmm_frame_state_in. */
#define PMM_FRAME_UNAVAILABLE   0
#define PMM_FRAME_FREE          1
#define PMM_FRAME_ALLOCATED     2

struct pmm {
    uint64_t *avail, *alloc;            /* PMM_WORDS(frames) words each */
    uint64_t frames;                    /* tracked frames: phys [0, frames * 4 KiB) */
    uint64_t usable;                    /* popcount(avail), fixed after init */
    uint64_t free;                      /* popcount(avail & ~alloc) */
    uint64_t hint;                      /* no free frame in words below this */
    uint64_t ignored_bytes;             /* available RAM at/above the limit (per entry) */
    int state;
};

/*
 * Initialize `p` over `frames` frames (a non-zero multiple of 64, at most
 * PMM_MAX_FRAMES) from the memory map in `bi` and the reservations in `t`.
 * On error the instance is left PMM_FAILED and allocates nothing.
 */
int pmm_init_in(struct pmm *p, uint64_t *avail, uint64_t *alloc, uint64_t frames,
                const struct boot_info *bi, const struct resv_table *t);
int pmm_alloc_in(struct pmm *p, uint64_t *phys);       /* *phys = 0 on failure */
int pmm_free_check_in(const struct pmm *p, uint64_t phys);     /* validation only */
int pmm_free_in(struct pmm *p, uint64_t phys);         /* no change on failure */
int pmm_frame_state_in(const struct pmm *p, uint64_t phys);
/* USB.2-C3a: mark one specific free frame allocated for the rest of the boot
 * (firmware-owned DMA residue found after initialization). It never claims an
 * allocated frame (PMM_EINUSE) or a never-allocatable one (PMM_EPROTECTED:
 * already safe). No change on failure; there is no matching release path. */
int pmm_reserve_in(struct pmm *p, uint64_t phys);
/* Recount both bitmaps and check every invariant; PMM_OK or PMM_ECORRUPT. */
int pmm_check_in(const struct pmm *p);

/* The kernel allocator (one instance over the full 1 GiB). */
int pmm_init(const struct boot_info *bi, const struct resv_table *t);
int pmm_alloc_frame(uint64_t *phys);    /* zeroed frame; panics on PMM_ECORRUPT */
int pmm_free_frame(uint64_t phys);      /* callers should treat any failure as a bug */
int pmm_reserve_frame(uint64_t phys);   /* pmm_reserve_in on the kernel instance */
uint64_t pmm_free_count(void);
const struct pmm *pmm_kernel(void);     /* read-only view for diagnostics and tests */
const char *pmm_strerror(int err);

#endif
