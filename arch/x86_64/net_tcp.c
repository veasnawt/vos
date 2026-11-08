#include "net_tcp.h"
#include "rust_kernel.h"
_Static_assert(sizeof(struct net_tcp)==504 && _Alignof(struct net_tcp)==8,"bounded TCP context (504 B)");
_Static_assert(NET_TCP_RETRY_TICKS*(NET_TCP_RETRY_MAX+1)<=NET_TCP_TX_TTL,"bounded retry horizon");

/* R3-N2: the passive TCP state machine (handshake, payload, retransmission,
 * closure and timeouts) is Rust-owned (kernel/rust/net/tcp.rs). This file
 * keeps the public C ABI and the net_tcp.h buffer/alias contract; no DMA, I/O
 * or allocation. */
int net_tcp_init(struct net_tcp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port,uint32_t initial_sequence)
{
    return vos_rust_net_tcp_init(c,mac,ip,port,initial_sequence);
}
int net_tcp_input(struct net_tcp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_tcp_input(c,frame,n,now,out,capacity,length);
}
int net_tcp_expire(struct net_tcp *c,uint64_t now)
{
    return vos_rust_net_tcp_expire(c,now);
}
void net_tcp_close(struct net_tcp *c)
{
    vos_rust_net_tcp_close(c);
}
int net_tcp_enable_payload(struct net_tcp *c)
{
    return vos_rust_net_tcp_enable_payload(c);
}
int net_tcp_send(struct net_tcp *c,const void *data,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_tcp_send(c,data,n,now,out,capacity,length);
}
int net_tcp_receive(struct net_tcp *c,void *out,size_t capacity,size_t *length,uint64_t now)
{
    return vos_rust_net_tcp_receive(c,out,capacity,length,now);
}
int net_tcp_ack(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_tcp_ack(c,now,out,capacity,length);
}
void net_tcp_abort(struct net_tcp *c)
{
    vos_rust_net_tcp_abort(c);
}
int net_tcp_enable_retransmission(struct net_tcp *c)
{
    return vos_rust_net_tcp_enable_retransmission(c);
}
int net_tcp_retransmit(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_tcp_retransmit(c,now,out,capacity,length);
}
int net_tcp_enable_closure(struct net_tcp *c)
{
    return vos_rust_net_tcp_enable_closure(c);
}
int net_tcp_finish(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length)
{
    return vos_rust_net_tcp_finish(c,now,out,capacity,length);
}
