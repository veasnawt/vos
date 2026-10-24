#include "net_arp.h"
#include "virtio_net.h"
#include "lib.h"

static const uint8_t broadcast[6]={255,255,255,255,255,255};
static const uint8_t zero[6]={0};
static uint16_t be16(const uint8_t *p) {return (uint16_t)((unsigned)p[0]*256+p[1]);}
static void put16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static int same(const void *a,const void *b,size_t n) {return !memcmp(a,b,n);}
static int mac_ok(const uint8_t *p) {return !(p[0]&1) && !same(p,zero,6);}
static int ip_ok(const uint8_t *p) {return p[0]!=0 && p[0]!=127 && p[0]<224;}
static int output_ok(void *out,size_t capacity,size_t *length)
{
    uintptr_t p=(uintptr_t)out,n=(uintptr_t)length;
    return out && length && capacity>=NET_ARP_WIRE && p+NET_ARP_WIRE>p && n+sizeof(*length)>n &&
           !(p<n+sizeof(*length) && n<p+NET_ARP_WIRE);
}
static void expire(struct net_arp *a,uint64_t now)
{
    for (unsigned i=0;i<NET_ARP_NEIGHBORS;i++)
        if (a->neighbors[i].valid && now-a->neighbors[i].seen>=NET_ARP_TTL) a->neighbors[i].valid=0;
    if (a->pending && now-a->pending_since>=NET_ARP_QUERY_TTL) a->pending=0;
}
int net_arp_init(struct net_arp *a,const uint8_t mac[6],const uint8_t ip[4])
{
    if (!a || !mac || !ip || !mac_ok(mac) || !ip_ok(ip)) return NET_ARP_EINVAL;
    uint8_t m[6],p[4];memcpy(m,mac,6);memcpy(p,ip,4);
    memset(a,0,sizeof(*a));memcpy(a->mac,m,6);memcpy(a->ip,p,4);return 0;
}
int net_neighbor_get(struct net_arp *a,const uint8_t ip[4],uint64_t now,uint8_t mac[6])
{
    if (!a || !ip || !mac) return 0;
    expire(a,now);
    for (unsigned i=0;i<NET_ARP_NEIGHBORS;i++)
        if (a->neighbors[i].valid && same(a->neighbors[i].ip,ip,4)) {memcpy(mac,a->neighbors[i].mac,6);return 1;}
    return 0;
}
static void learn(struct net_arp *a,const uint8_t ip[4],const uint8_t mac[6],uint64_t now)
{
    expire(a,now);unsigned slot=0;uint64_t oldest=0;
    for (unsigned i=0;i<NET_ARP_NEIGHBORS;i++) {
        if (a->neighbors[i].valid && same(a->neighbors[i].ip,ip,4)) {slot=i;goto store;}
    }
    for (unsigned i=0;i<NET_ARP_NEIGHBORS;i++) {
        if (!a->neighbors[i].valid) {slot=i;goto store;}
        uint64_t age=now-a->neighbors[i].seen;if (age>=oldest) {oldest=age;slot=i;}
    }
store:
    memcpy(a->neighbors[slot].ip,ip,4);memcpy(a->neighbors[slot].mac,mac,6);
    a->neighbors[slot].seen=now;a->neighbors[slot].valid=1;
    if (a->pending && same(a->pending_ip,ip,4)) a->pending=0;
}
static void packet(uint8_t out[NET_ARP_WIRE],const struct net_arp *a,unsigned op,
                   const uint8_t dest[6],const uint8_t target_mac[6],const uint8_t target_ip[4])
{
    memset(out,0,NET_ARP_WIRE);memcpy(out,dest,6);memcpy(out+6,a->mac,6);put16(out+12,0x0806);
    put16(out+14,1);put16(out+16,0x0800);out[18]=6;out[19]=4;put16(out+20,(uint16_t)op);
    memcpy(out+22,a->mac,6);memcpy(out+28,a->ip,4);memcpy(out+32,target_mac,6);memcpy(out+38,target_ip,4);
}
int net_arp_query(struct net_arp *a,const uint8_t ip[4],uint64_t now,void *out,size_t capacity,size_t *length)
{
    if (!a || !ip || !ip_ok(ip) || same(ip,a->ip,4)) return NET_ARP_EINVAL;
    if (!output_ok(out,capacity,length)) return NET_ARP_NOSPACE;
    uint8_t mac[6],target[4];memcpy(target,ip,4);
    if (net_neighbor_get(a,target,now,mac)) return NET_ARP_CACHED;
    if (a->pending) return NET_ARP_BUSY;
    uint8_t reply[NET_ARP_WIRE];packet(reply,a,1,broadcast,zero,target);
    memcpy(out,reply,sizeof(reply));*length=sizeof(reply);
    memcpy(a->pending_ip,target,4);a->pending_since=now;a->pending=1;return NET_ARP_REQUEST;
}
int net_ethernet_input(struct net_arp *a,const void *frame,size_t length,uint64_t now,void *out,size_t capacity,size_t *output_length)
{
    if (!a || !frame || length<14 || length>VNET_FRAME_MAX) return NET_ARP_EINVAL;
    const uint8_t *p=frame;
    if (!same(p,a->mac,6) && !same(p,broadcast,6)) return NET_ARP_DROP;
    if (!mac_ok(p+6) || same(p+6,a->mac,6)) return NET_ARP_EINVAL;
    unsigned type=be16(p+12);
    if (type<=1500) return NET_ARP_UNSUPPORTED; /* IEEE 802.3 length/LLC, not Ethernet-II. */
    if (type<0x0600) return NET_ARP_EINVAL;
    if (type!=0x0806) return NET_ARP_UNSUPPORTED; /* IPv4/VLAN/IPv6 are not dispatched yet. */
    if (length<42) return NET_ARP_EINVAL;
    if (be16(p+14)!=1 || be16(p+16)!=0x0800) return NET_ARP_UNSUPPORTED;
    if (p[18]!=6 || p[19]!=4 || (be16(p+20)!=1 && be16(p+20)!=2)) return NET_ARP_EINVAL;
    const uint8_t *sha=p+22,*spa=p+28,*tha=p+32,*tpa=p+38;
    unsigned op=be16(p+20);int probe=same(spa,zero,4);
    if (!same(sha,p+6,6) || (!ip_ok(spa) && !(probe && op==1)) || same(spa,a->ip,4)) return NET_ARP_EINVAL;
    if (!same(tpa,a->ip,4)) return NET_ARP_DROP;
    if (op==2) {
        if (!same(p,a->mac,6) || !same(tha,a->mac,6) || !a->pending || now-a->pending_since>=NET_ARP_QUERY_TTL || !same(spa,a->pending_ip,4)) return NET_ARP_DROP;
        learn(a,spa,sha,now);return NET_ARP_LEARNED;
    }
    if (!output_ok(out,capacity,output_length)) return NET_ARP_NOSPACE;
    /* Build before writing output/state, including when frame and out alias. */
    uint8_t reply[NET_ARP_WIRE];packet(reply,a,2,sha,sha,spa);
    if (!probe) learn(a,spa,sha,now);
    memcpy(out,reply,sizeof(reply));*output_length=sizeof(reply);return NET_ARP_REPLY;
}
