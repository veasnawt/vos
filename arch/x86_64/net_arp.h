#ifndef VOS_X86_64_NET_ARP_H
#define VOS_X86_64_NET_ARP_H
#include <stdint.h>
#include <stddef.h>
#define NET_ARP_NEIGHBORS 4
#define NET_ARP_TTL 6000u       /* Existing 100 Hz clock: lazy 60-second expiry. */
#define NET_ARP_QUERY_TTL 200u
#define NET_ARP_WIRE 60
enum { NET_ARP_DROP=0, NET_ARP_REPLY=1, NET_ARP_LEARNED=2,
       NET_ARP_REQUEST=3, NET_ARP_CACHED=4,
       NET_ARP_EINVAL=-1, NET_ARP_UNSUPPORTED=-2, NET_ARP_NOSPACE=-3, NET_ARP_BUSY=-4 };
struct net_neighbor { uint8_t ip[4],mac[6]; uint64_t seen; int valid; };
struct net_arp {
    uint8_t mac[6],ip[4],pending_ip[4];
    struct net_neighbor neighbors[NET_ARP_NEIGHBORS];
    uint64_t pending_since;
    int pending;
};
/* Kernel-owned protocol context. No allocation, DMA, I/O or userspace grants.
 * Inputs/outputs may alias each other, but must not alias the context; length
 * output must be separate from packet output. Only successful packet generation
 * changes output bytes/length. Invalid input never changes protocol state. */
int net_arp_init(struct net_arp *a,const uint8_t mac[6],const uint8_t ip[4]);
int net_neighbor_get(struct net_arp *a,const uint8_t ip[4],uint64_t now,uint8_t mac[6]);
int net_arp_query(struct net_arp *a,const uint8_t ip[4],uint64_t now,void *out,size_t capacity,size_t *length);
/* Untagged Ethernet-II dispatch. Only Ethernet/IPv4 ARP is supported here.
 * Unsupported types, unrelated targets and unsolicited replies produce no TX. */
int net_ethernet_input(struct net_arp *a,const void *frame,size_t length,uint64_t now,void *out,size_t capacity,size_t *output_length);
#endif
