#ifndef VOS_X86_64_NET_UDP_H
#define VOS_X86_64_NET_UDP_H
#include "net_ipv4.h"
#define NET_UDP_DATA_MAX 512u
#define NET_UDP_QUEUE 4u
enum { NET_UDP_QUEUED=7, NET_UDP_SENT=8, NET_UDP_FULL=-6, NET_UDP_EMPTY=-7 };
struct net_udp_datagram {
    uint8_t source[4];
    uint16_t port,length;
    int checksum_present;
    uint8_t data[NET_UDP_DATA_MAX];
};
struct net_udp {
    struct net_ipv4 ip; /* Shared ARP, IPv4 and ICMP context. */
    uint16_t port;
    unsigned head,count;
    struct net_udp_datagram queue[NET_UDP_QUEUE];
};
/* One explicitly configured kernel endpoint; single owner, IF=0. No allocation,
 * DMA, scheduler, syscalls or app authority. ARP's buffer/alias contract applies.
 * Receive copies into a full caller-owned datagram object outside the context.
 * A source port of zero is legal RX metadata (RFC 768), never a reply endpoint. */
int net_udp_init(struct net_udp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port);
int net_udp_send(struct net_udp *c,const uint8_t peer[4],uint16_t port,const void *data,size_t n,uint64_t now,void *out,size_t capacity,size_t *length);
/* ARP and ICMP retain their existing return codes; UDP queues copied payloads.
 * Invalid, oversized and queue-full UDP never modifies state/output. */
int net_udp_input(struct net_udp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length);
int net_udp_receive(struct net_udp *c,struct net_udp_datagram *out);
/* Disable endpoint and erase queued data. Caller owns adapter reset/teardown. */
void net_udp_close(struct net_udp *c);
#endif
