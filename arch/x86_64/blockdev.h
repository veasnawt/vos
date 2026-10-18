/*
 * Block-device interface (N3.1, storage.md layer 2).
 *
 * Read-only by construction: the interface has no write, flush or discard
 * operation, and no driver behind it builds such a request. A device is
 * addressed in 512-byte sectors (the virtio-blk unit). Callers pass a
 * kernel buffer and its capacity; the driver validates the sector range,
 * the request size and the buffer before any request reaches the device.
 */
#ifndef VOS_X86_64_BLOCKDEV_H
#define VOS_X86_64_BLOCKDEV_H

#include <stdint.h>

#define BLK_SECTOR_SIZE 512

#define BLK_OK          0
#define BLK_ENODEV      1   /* no usable device */
#define BLK_EINVAL      2   /* bad sector count, buffer or capacity */
#define BLK_ERANGE      3   /* sectors outside the device */
#define BLK_EIO         4   /* the device reported an I/O error (VIRTIO_BLK_S_IOERR) */
#define BLK_EUNSUPP     5   /* the device reported an unsupported request (VIRTIO_BLK_S_UNSUPP) */
#define BLK_ETIMEDOUT   6   /* no completion within the poll bound; the device was reset */
#define BLK_EBUSY       7   /* every request slot is in use */
#define BLK_EBROKEN     8   /* the device was reset after an error; it needs reinitialization */
#define BLK_EPROTO      9   /* the device broke the queue protocol; it was reset */

struct blockdev {
    const char *name;
    uint64_t sectors;           /* capacity in 512-byte sectors */
    uint32_t max_sectors;       /* largest request */
    int device_read_only;       /* the device itself reported read-only (VIRTIO_BLK_F_RO) */
    /* Copy sectors [lba, lba + count) into buf (cap bytes, kernel memory). BLK_*. */
    int (*read)(uint64_t lba, uint32_t count, void *buf, uint64_t cap);
};

const char *blk_strerror(int err);

#endif
