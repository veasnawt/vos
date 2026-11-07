#include "net_ipv4.h"
#include "rust_kernel.h"

/* R3-N1: checksums, local IPv4 validation, the envelope builder and the ICMP
 * echo state are Rust-owned (kernel/rust/net). This file keeps the public C
 * ABI and the net_arp.h buffer/alias contract; no DMA, I/O or allocation. */
uint16_t net_checksum(const void *bytes,size_t length)
{
    return vos_rust_net_checksum(bytes,length);
}
uint16_t net_ipv4_transport_checksum(const uint8_t source[4],const uint8_t dest[4],uint8_t protocol,const void *bytes,size_t n)
{
    return vos_rust_net_transport_checksum(source,dest,protocol,bytes,n);
}
int net_ipv4_init(struct net_ipv4 *c,const uint8_t mac[6],const uint8_t ip[4])
{
    return vos_rust_net_ipv4_init(c,mac,ip);
}
int net_ipv4_begin(const struct net_ipv4 *c,const uint8_t mac[6],const uint8_t ip[4],uint8_t protocol,
                   size_t n,void *buffer,size_t capacity,size_t *length)
{
    return vos_rust_net_ipv4_begin(c,mac,ip,protocol,n,buffer,capacity,length);
}
int net_ipv4_echo_request(struct net_ipv4 *c,const uint8_t peer[4],uint16_t id,uint16_t seq,
                         const void *data,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_ipv4_echo_request(c,peer,id,seq,data,n,now,out,capacity,length);
}
int net_ipv4_payload(const struct net_ipv4 *c,const void *frame,size_t length,uint8_t protocol,struct net_ipv4_payload *view)
{
    return vos_rust_net_ipv4_payload(c,frame,length,protocol,view);
}
int net_ipv4_input(struct net_ipv4 *c,const void *frame,size_t length,uint64_t now,void *out,size_t capacity,size_t *output_length)
{
    return vos_rust_net_ipv4_input(c,frame,length,now,out,capacity,output_length);
}
