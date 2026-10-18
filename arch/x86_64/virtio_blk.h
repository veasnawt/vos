/*
 * Read-only virtio-blk driver (N3.1; block-layer attachment, lifecycle and
 * cooperative waiting since N3.2), modern virtio 1.x PCI transport, polled,
 * one split virtqueue.
 *
 * Transport. The device is found on PCI bus 0 (vendor 0x1AF4, device 0x1042,
 * or the transitional 0x1001 with subsystem ID 2, always driven through its
 * modern interface). Its registers (common configuration, notification,
 * device configuration) are reached through the VIRTIO_PCI_CAP_PCI_CFG
 * window in PCI configuration space, so no device memory is mapped into the
 * kernel and the W^X kernel map is unchanged. The legacy I/O-port interface
 * is never used.
 *
 * Features. Required: VIRTIO_F_VERSION_1. Accepted if offered:
 * VIRTIO_BLK_F_RO. Everything else is declined. A device that requires
 * VIRTIO_F_ACCESS_PLATFORM is refused (VOS has no IOMMU support).
 *
 * Queue. Queue 0, VBLK_QUEUE_SIZE descriptors, VBLK_SLOTS request slots of
 * three fixed descriptors each (header, data, status). Interrupts are
 * suppressed (VRING_AVAIL_F_NO_INTERRUPT, PCI INTx disabled); completions
 * are found by polling the used ring for at most VBLK_POLL_LIMIT iterations.
 *
 * DMA. The device writes only into frames of a dedicated pool allocated
 * from the PMM at initialization (ring, headers/status, one data frame per
 * slot); data is copied out to the caller after validation. Every
 * descriptor is checked against the pool before it is published. Without an
 * IOMMU this constrains what the driver asks for; it does NOT stop a faulty
 * or malicious device from writing any physical memory.
 *
 * Errors. An I/O error or unsupported-request status fails that request
 * only. A timeout or a protocol violation (an impossible used-ring entry)
 * resets the device: it is then BROKEN until vblk_reinit. The driver's own
 * codes (VBLK_E*) never leave it: the block layer sees BLK_E* (vblk_to_blk).
 *
 * Block layer (N3.2). A device that reaches READY is registered as "vblk0"
 * (driver "virtio-blk"); it stays registered while BROKEN (reported
 * unavailable) and is unregistered by vblk_shutdown. Its epoch is the
 * number of successful initializations, so every reinitialization
 * invalidates what the block layer cached.
 *
 * DMA frame lifecycle (N3.2). The pool is freed only after a reset the
 * device confirmed (status reads 0) and with bus mastering switched off:
 * when the device is refused during initialization, and on vblk_shutdown.
 * If the reset is not confirmed the frames stay allocated ("quarantined"),
 * because the device might still write them. A BROKEN device keeps its pool
 * for vblk_reinit.
 *
 * Waiting (N3.2). While it polls, vblk_wait yields the CPU every
 * VBLK_YIELD_POLLS polls (interrupts stay disabled; the kernel stays
 * non-preemptible). A wait ends after VBLK_POLL_LIMIT polls or, once other
 * threads ran meanwhile, after VBLK_WAIT_TICKS timer ticks, whichever comes
 * first. After a yield it re-checks that the device was not reset and its
 * slot is still its own.
 */
#ifndef VOS_X86_64_VIRTIO_BLK_H
#define VOS_X86_64_VIRTIO_BLK_H

#include <stdint.h>

#include "blockdev.h"
#include "pci.h"

#define VBLK_QUEUE_SIZE     16
#define VBLK_SLOTS          4
#define VBLK_MAX_SECTORS    8                           /* one 4 KiB data frame per request */
#define VBLK_POOL_FRAMES    (2 + VBLK_SLOTS)            /* ring, headers/status, data */
#define VBLK_POLL_LIMIT     50000000ULL                 /* used-ring polls per request */
#define VBLK_YIELD_POLLS    4096                        /* polls between yields while waiting */
#define VBLK_WAIT_TICKS     200                         /* timer ticks (about 2 s) per request */

/* Driver result codes (never seen outside the driver; vblk_to_blk maps them). */
#define VBLK_OK         0
#define VBLK_ENODEV     1   /* no usable device */
#define VBLK_EINVAL     2   /* bad sector count, buffer or capacity */
#define VBLK_ERANGE     3   /* sectors outside the device */
#define VBLK_EIO        4   /* the device reported an I/O error (VIRTIO_BLK_S_IOERR) */
#define VBLK_EUNSUPP    5   /* the device reported an unsupported request (VIRTIO_BLK_S_UNSUPP) */
#define VBLK_ETIMEDOUT  6   /* no completion within the bounds; the device was reset */
#define VBLK_EBUSY      7   /* every request slot is in use, or requests are outstanding */
#define VBLK_EBROKEN    8   /* the device was reset; it needs reinitialization */
#define VBLK_EPROTO     9   /* the device broke the queue protocol; it was reset */

#define VBLK_ABSENT     0       /* no supported device found */
#define VBLK_FAILED     1       /* a device was found but could not be initialized */
#define VBLK_READY      2
#define VBLK_BROKEN     3       /* reset after a timeout or protocol error */
#define VBLK_STOPPED    4       /* released by vblk_shutdown: reset, bus mastering off, pool freed */

/* The virtio structures located through the PCI capability list. */
struct vpci_region {
    int present;
    uint8_t bar;
    uint32_t offset, length;
};

struct vpci_caps {
    struct vpci_region common, notify, device;
    uint32_t notify_mult;
    unsigned cfg_cap;           /* config-space offset of the VIRTIO_PCI_CAP_PCI_CFG capability (0 = none) */
    unsigned seen;              /* capabilities visited */
};

struct vblk_info {
    int state;
    const char *why;            /* why the device is FAILED or BROKEN */
    struct pci_addr pci;
    uint16_t device_id, queue_max, queue_size;
    uint8_t revision, status;
    uint64_t capacity, offered, accepted;
    uint64_t pool[VBLK_POOL_FRAMES];
    int pool_held;              /* the pool frames are allocated */
    int quarantined;            /* the pool is kept because a reset was not confirmed */
    blk_id_t blk_id;            /* block-layer ID while registered, else 0 */
    /* Counters since boot. */
    uint64_t submitted, completed, ok, ioerr, unsupp, timeouts, protocol_errors, resets, inits;
    uint64_t pool_allocs, pool_frees, yields, tick_timeouts;
    uint64_t max_spins;         /* longest successful wait, in polls */
};

/* Probe bus 0, initialize the first supported device and register it with the block layer. Returns VBLK_* state. */
int vblk_init(void);
/*
 * Reset and initialize the present device again (any state but ABSENT; a
 * quarantined device only once a reset is confirmed), allocating the pool if
 * it was freed and registering the device if it is not registered. Refused
 * (state unchanged) while requests are outstanding. Returns the new state.
 */
int vblk_reinit(void);
/*
 * Release the device: unregister it, reset it, switch bus mastering off and
 * free the DMA pool. VBLK_OK (state STOPPED); VBLK_ENODEV without a device;
 * VBLK_EBUSY while requests are outstanding or cached lines are in use;
 * VBLK_EBROKEN if the reset was not confirmed (pool quarantined, state BROKEN).
 */
int vblk_shutdown(void);
const struct vblk_info *vblk_info(void);
const char *vblk_state_name(int state);
/* The block-layer error for a driver result. */
int vblk_to_blk(int vblk_err);

/* Synchronous read (the block layer's read operation, before mapping): validate, submit, wait, copy out. */
int vblk_read(uint64_t lba, uint32_t count, void *buf, uint64_t cap);
/*
 * The same in two steps, with up to VBLK_SLOTS requests outstanding:
 * vblk_submit validates the range and takes a slot (VBLK_EBUSY if none is
 * free); vblk_wait polls for that slot, validates the completion and copies
 * the data into buf (which vblk_wait validates).
 */
int vblk_submit(uint64_t lba, uint32_t count, unsigned *slot);
int vblk_wait(unsigned slot, void *buf, uint64_t cap);

/* Consistency of the driver state; NULL if consistent, else the first problem. */
const char *vblk_audit(void);

/* Pure helpers (also used on synthetic inputs by the self-tests). NULL = accepted. */
const char *vpci_parse_caps(uint8_t (*rd8)(void *ctx, unsigned off), uint32_t (*rd32)(void *ctx, unsigned off),
                            void *ctx, struct vpci_caps *c);
const char *vblk_choose_features(uint64_t offered, uint64_t *accept);
const char *vblk_choose_queue_size(uint16_t device_max, uint16_t *size);
/* 1 if [pa, pa + len) lies inside one frame of the DMA pool (len >= 1). */
int vblk_dma_ok(uint64_t pa, uint64_t len);
/* 1 if a transport access of len bytes at offset off of BAR bar lies inside a virtio structure. */
int vblk_window_ok(const struct vpci_caps *c, unsigned bar, uint32_t off, unsigned len);
/* 1 if a kernel buffer of len bytes at buf may receive data (present, writable, supervisor, not DMA). */
int vblk_buffer_ok(const void *buf, uint64_t len);

#define VIRTIO_F_VERSION_1          32
#define VIRTIO_F_ACCESS_PLATFORM    33
#define VIRTIO_BLK_F_RO             5

#ifdef VOS_TEST
/* Skip the device notification of the next n submissions (the request then times out). */
void vblk_test_skip_notify(unsigned n);
/* Poll bound for the next waits (0 restores VBLK_POLL_LIMIT). */
void vblk_test_poll_limit(uint64_t polls);
/* Polls spent by the last vblk_wait. */
uint64_t vblk_test_last_spins(void);
/* Yield every n polls while waiting (0: never yield); VBLK_YIELD_POLLS is the default. */
void vblk_test_yield_polls(uint64_t n);
/* Tick bound of the next waits (0 restores VBLK_WAIT_TICKS). */
void vblk_test_wait_ticks(uint64_t ticks);
/* Append a forged entry to the used ring, as if the device had written it. */
void vblk_test_inject_used(uint32_t id, uint32_t len);
/* The device status register as the device reports it now (0 after a completed reset). */
uint8_t vblk_test_device_status(void);
/* Submit a request of a non-writing type other than IN (allow-list; never a write). */
int vblk_test_submit_type(uint32_t type, uint64_t lba, uint32_t count, unsigned *slot);
#define VBLK_TEST_T_UNKNOWN 0xFEu   /* even (no OUT bit), not defined by the specification */
#endif

#endif
