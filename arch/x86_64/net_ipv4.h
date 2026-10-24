#ifndef VOS_X86_64_NET_IPV4_H
#define VOS_X86_64_NET_IPV4_H
#include "net_arp.h"
#define NET_PING_DATA_MAX 64
#define NET_PING_TTL 200u
enum { NET_IP_ECHO_REQUEST=5, NET_IP_ECHO_MATCH=6, NET_IP_NEIGHBOR=-5 };
struct net_ipv4 {
    struct net_arp arp; /* The same bounded neighbor cache, not a second cache. */
    uint16_t identification,id,sequence,data_length;
    uint8_t peer[4],peer_mac[6],data[NET_PING_DATA_MAX];
    uint64_t since,sent,matched,answered;
    int pending;
};
/* Pure kernel protocol code. Buffers must obey net_arp.h's lifetime/alias
 * contract. No DMA, allocation, I/O or application network authority. */
uint16_t net_checksum(const void *bytes,size_t length); /* Caller bounds length <=1500. */
int net_ipv4_init(struct net_ipv4 *c,const uint8_t mac[6],const uint8_t ip[4]);
/* Caller resolves neighbors through c->arp first. No packet queued on a miss.
 * One originated echo at a time; maximum 64 payload bytes, timeout in PIT ticks. */
int net_ipv4_echo_request(struct net_ipv4 *c,const uint8_t peer[4],uint16_t id,uint16_t sequence,
                         const void *data,size_t data_length,uint64_t now,void *out,size_t capacity,size_t *length);
/* Dispatch existing ARP or local IPv4/ICMP. NET_ARP_REPLY means a generated
 * ARP/echo reply; NET_IP_ECHO_MATCH means the pending echo completed, no TX.
 * Reassembly, IP options, forwarding, ICMP errors and other IP protocols omitted. */
int net_ipv4_input(struct net_ipv4 *c,const void *frame,size_t length,uint64_t now,void *out,size_t capacity,size_t *output_length);
#endif
