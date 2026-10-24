#ifndef VOS_X86_64_VIRTIO_NET_H
#define VOS_X86_64_VIRTIO_NET_H
#include <stdint.h>
#include "pci.h"

#define VNET_QUEUE 8
#define VNET_FRAME_MAX 1514
#define VNET_HEADER 12
#define VNET_POOL_FRAMES (2 + VNET_QUEUE + 1)
#define VNET_POLL_LIMIT 2000000u
#define VNET_RESET_POLLS 10000u
enum { VNET_OK, VNET_ENODEV, VNET_EINVAL, VNET_EBUSY, VNET_EAGAIN,
       VNET_EMSGSIZE, VNET_EUNSUPPORTED, VNET_ENOMEM, VNET_EPROTO, VNET_ETIMEOUT, VNET_EBROKEN };
enum { VNET_ABSENT, VNET_READY, VNET_FAILED, VNET_BROKEN, VNET_STOPPED };
struct vnet_info {
    int state, held, quarantined;
    struct pci_addr pci;
    uint8_t mac[6];
    uint8_t last_rx_header[VNET_HEADER];
    uint32_t last_rx_length;
    uint64_t pool[VNET_POOL_FRAMES], offered, accepted, transmitted, received;
    const char *why;
};
/* Single kernel-thread owner, IF=0; no userspace ABI or implicit grants.
 * Copies only: callers never receive or supply DMA memory. RX is nonblocking;
 * short capacity leaves the packet queued and all outputs unchanged. */
int vnet_init(void);
int vnet_send(const void *frame, uint32_t length);
int vnet_receive(void *frame, uint32_t capacity, uint32_t *length);
/* Confirm reset and disable bus mastering before freeing. On unconfirmed reset
 * keep every DMA frame quarantined; a later explicit stop may retry reset. */
int vnet_stop(void);
const struct vnet_info *vnet_info(void);
#endif
