#include "net_arp.h"
#include "rust_kernel.h"

/* R3-N1: Ethernet-II validation, ARP parsing/construction and the bounded
 * neighbor table are Rust-owned (kernel/rust/net). This file keeps the public
 * C ABI and the net_arp.h buffer/alias contract; no DMA, I/O or allocation. */
int net_arp_init(struct net_arp *a,const uint8_t mac[6],const uint8_t ip[4])
{
    return vos_rust_net_arp_init(a,mac,ip);
}
int net_neighbor_get(struct net_arp *a,const uint8_t ip[4],uint64_t now,uint8_t mac[6])
{
    return vos_rust_net_neighbor_get(a,ip,now,mac);
}
int net_arp_query(struct net_arp *a,const uint8_t ip[4],uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_arp_query(a,ip,now,out,capacity,length);
}
int net_ethernet_type(const struct net_arp *a,const void *frame,size_t length,uint16_t *output_type)
{
    return vos_rust_net_ethernet_type(a,frame,length,output_type);
}
int net_ethernet_input(struct net_arp *a,const void *frame,size_t length,uint64_t now,void *out,size_t capacity,size_t *output_length)
{
    return vos_rust_net_ethernet_input(a,frame,length,now,out,capacity,output_length);
}
