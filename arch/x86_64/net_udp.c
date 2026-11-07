#include "net_udp.h"
#include "rust_kernel.h"

/* R3-N1: UDP validation, sending, the bounded receive queue and close are
 * Rust-owned (kernel/rust/net). This file keeps the public C ABI and the
 * net_arp.h buffer/alias contract; no DMA, I/O or allocation. */
int net_udp_init(struct net_udp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port)
{
    return vos_rust_net_udp_init(c,mac,ip,port);
}
int net_udp_send(struct net_udp *c,const uint8_t peer[4],uint16_t port,const void *data,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_udp_send(c,peer,port,data,n,now,out,capacity,length);
}
int net_udp_input(struct net_udp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_udp_input(c,frame,n,now,out,capacity,length);
}
int net_udp_receive(struct net_udp *c,struct net_udp_datagram *out)
{
    return vos_rust_net_udp_receive(c,out);
}
void net_udp_close(struct net_udp *c)
{
    vos_rust_net_udp_close(c);
}
