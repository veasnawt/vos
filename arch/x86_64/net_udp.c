#include "net_udp.h"
#include "lib.h"
static uint16_t get16(const uint8_t *p) {return (uint16_t)((unsigned)p[0]*256+p[1]);}
static void put16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static uint16_t checksum(const uint8_t source[4],const uint8_t dest[4],const uint8_t *udp,size_t n)
{
    return net_ipv4_transport_checksum(source,dest,17,udp,n);
}
int net_udp_init(struct net_udp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port)
{
    if (!c || !port) return NET_ARP_EINVAL;
    struct net_ipv4 ipctx;int e=net_ipv4_init(&ipctx,mac,ip);if (e) return e;
    memset(c,0,sizeof(*c));memcpy(&c->ip,&ipctx,sizeof(ipctx));c->port=port;return 0;
}
int net_udp_send(struct net_udp *c,const uint8_t peer[4],uint16_t port,const void *data,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    if (!c || !c->port || !peer || !port || peer[0]==0 || peer[0]==127 || peer[0]>=224 ||
        !memcmp(peer,c->ip.arp.ip,4) || n>NET_UDP_DATA_MAX || (!data && n)) return NET_ARP_EINVAL;
    size_t wire=42+n;if (wire<60) wire=60;
    uintptr_t a=(uintptr_t)out,b=(uintptr_t)length;
    if (!out || !length || capacity<wire || a+wire<=a || b+sizeof(*length)<=b ||
        (a<b+sizeof(*length) && b<a+wire)) return NET_ARP_NOSPACE;
    uint8_t target[4],mac[6];memcpy(target,peer,4);
    if (!net_neighbor_get(&c->ip.arp,target,now,mac)) return NET_IP_NEIGHBOR;
    uint8_t packet[42+NET_UDP_DATA_MAX];size_t size;
    int e=net_ipv4_begin(&c->ip,mac,target,17,8+n,packet,sizeof(packet),&size);if (e) return e;
    uint8_t *udp=packet+34;put16(udp,c->port);put16(udp+2,port);put16(udp+4,(uint16_t)(8+n));
    if (n) memcpy(udp+8,data,n);
    uint16_t sum=checksum(c->ip.arp.ip,target,udp,8+n);put16(udp+6,sum?sum:65535);
    memcpy(out,packet,size);*length=size;c->ip.identification++;return NET_UDP_SENT;
}
int net_udp_input(struct net_udp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    if (!c || !c->port) return NET_ARP_EINVAL;
    struct net_ipv4_payload view;int e=net_ipv4_payload(&c->ip,frame,n,17,&view);
    if (e==NET_ARP_UNSUPPORTED) return net_ipv4_input(&c->ip,frame,n,now,out,capacity,length);
    if (e!=1) return e;
    const uint8_t *udp=view.bytes;
    if (view.length<8) return NET_ARP_EINVAL;
    size_t size=get16(udp+4);
    /* This bounded profile has no UDP options or IP payload trailer. */
    if (size<8 || size!=view.length || !get16(udp+2)) return NET_ARP_EINVAL;
    if (get16(udp+2)!=c->port) return NET_ARP_DROP;
    int present=get16(udp+6)!=0;
    if (present && checksum(view.source,c->ip.arp.ip,udp,size)) return NET_ARP_EINVAL;
    if (size-8>NET_UDP_DATA_MAX) return NET_ARP_NOSPACE;
    if (c->count==NET_UDP_QUEUE) return NET_UDP_FULL;
    struct net_udp_datagram *d=&c->queue[(c->head+c->count)%NET_UDP_QUEUE];
    memset(d,0,sizeof(*d));memcpy(d->source,view.source,4);d->port=get16(udp);d->length=(uint16_t)(size-8);d->checksum_present=present;
    if (d->length) memcpy(d->data,udp+8,d->length);
    c->count++;return NET_UDP_QUEUED;
}
int net_udp_receive(struct net_udp *c,struct net_udp_datagram *out)
{
    if (!c || !c->port || !out) return NET_ARP_EINVAL;
    if (!c->count) return NET_UDP_EMPTY;
    memcpy(out,&c->queue[c->head],sizeof(*out));memset(&c->queue[c->head],0,sizeof(*out));
    c->head=(c->head+1)%NET_UDP_QUEUE;c->count--;return 0;
}
void net_udp_close(struct net_udp *c) {if (c) memset(c,0,sizeof(*c));}
