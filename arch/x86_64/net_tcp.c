#include "net_tcp.h"
#include "lib.h"
static uint16_t get16(const uint8_t *p) {return (uint16_t)((unsigned)p[0]*256+p[1]);}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static void put32(uint8_t *p,uint32_t n) {p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;}
int net_tcp_init(struct net_tcp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port,uint32_t initial_sequence)
{
    if (!c || !port) return NET_ARP_EINVAL;
    struct net_ipv4 ipctx;int e=net_ipv4_init(&ipctx,mac,ip);if (e) return e;
    memset(c,0,sizeof(*c));memcpy(&c->ip,&ipctx,sizeof(ipctx));c->port=port;c->next_isn=initial_sequence;c->state=NET_TCP_LISTEN;return 0;
}
int net_tcp_expire(struct net_tcp *c,uint64_t now)
{
    if (!c || !c->port) return NET_ARP_EINVAL;
    if (c->state!=NET_TCP_SYN_RECEIVED || now-c->since<NET_TCP_HANDSHAKE_TTL) return 0;
    memset(c->peer,0,sizeof(c->peer));memset(c->peer_mac,0,sizeof(c->peer_mac));c->peer_port=0;
    c->isn=c->peer_isn=c->snd_next=c->rcv_next=0;c->since=0;c->state=NET_TCP_LISTEN;return 1;
}
int net_tcp_input(struct net_tcp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    if (!c || !c->port) return NET_ARP_EINVAL;
    struct net_ipv4_payload view;int e=net_ipv4_payload(&c->ip,frame,n,6,&view);
    if (e==NET_ARP_UNSUPPORTED) return net_ipv4_input(&c->ip,frame,n,now,out,capacity,length);
    if (e!=1) return e;
    const uint8_t *tcp=view.bytes,*p=frame;
    if (view.length<20) return NET_ARP_EINVAL;
    size_t header=(tcp[12]>>4)*4u;
    if (header<20 || header>view.length || (tcp[12]&15) || !get16(tcp) || !get16(tcp+2) ||
        net_ipv4_transport_checksum(view.source,c->ip.arp.ip,6,tcp,view.length)) return NET_ARP_EINVAL;
    if (header!=20 || view.length!=20 || get16(tcp+18)) return NET_ARP_UNSUPPORTED;
    if (get16(tcp+2)!=c->port) return NET_ARP_DROP;
    uint8_t flags=tcp[13];uint32_t seq=get32(tcp+4),ack=get32(tcp+8);
    if (flags!=2 && flags!=16) return NET_ARP_UNSUPPORTED;
    if (c->state==NET_TCP_LISTEN) {
        if (flags!=2 || ack) return NET_ARP_DROP;
        uint8_t packet[60],peer[4],mac[6];size_t size;
        memcpy(peer,view.source,4);memcpy(mac,p+6,6);
        e=net_ipv4_begin(&c->ip,mac,peer,6,20,packet,sizeof(packet),&size);if (e) return e;
        /* Validate caller capacity before changing any connection or output. */
        uintptr_t a=(uintptr_t)out,b=(uintptr_t)length;
        if (!out || !length || capacity<size || a+size<=a || b+sizeof(*length)<=b ||
            (a<b+sizeof(*length) && b<a+size)) return NET_ARP_NOSPACE;
        uint8_t *reply=packet+34;put16(reply,c->port);put16(reply+2,get16(tcp));
        put32(reply+4,c->next_isn);put32(reply+8,seq+1u);reply[12]=0x50;reply[13]=18;
        /* Window zero: this establishment-only profile has no receive stream. */
        put16(reply+16,net_ipv4_transport_checksum(c->ip.arp.ip,peer,6,reply,20));
        memcpy(c->peer,peer,4);memcpy(c->peer_mac,mac,6);c->peer_port=get16(tcp);
        c->isn=c->next_isn;c->next_isn+=0x10000u;c->peer_isn=seq;c->snd_next=c->isn+1u;c->rcv_next=seq+1u;
        c->since=now;c->state=NET_TCP_SYN_RECEIVED;c->ip.identification++;
        memcpy(out,packet,size);*length=size;return NET_TCP_SYNACK;
    }
    if (c->state==NET_TCP_SYN_RECEIVED && now-c->since>=NET_TCP_HANDSHAKE_TTL) return NET_TCP_EXPIRED;
    if (c->state!=NET_TCP_SYN_RECEIVED || flags!=16 || get16(tcp)!=c->peer_port ||
        memcmp(view.source,c->peer,4) || memcmp(p+6,c->peer_mac,6) || seq!=c->rcv_next || ack!=c->snd_next) return NET_ARP_DROP;
    c->state=NET_TCP_ESTABLISHED;return NET_TCP_CONNECTED;
}
void net_tcp_close(struct net_tcp *c) {if (c) memset(c,0,sizeof(*c));}
