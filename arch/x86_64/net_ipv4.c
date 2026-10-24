#include "net_ipv4.h"
#include "virtio_net.h"
#include "lib.h"

static uint16_t get16(const uint8_t *p) {return (uint16_t)((unsigned)p[0]*256+p[1]);}
static void put16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static int same(const void *a,const void *b,size_t n) {return !memcmp(a,b,n);}
static int unicast(const uint8_t *ip) {return ip[0]!=0 && ip[0]!=127 && ip[0]<224;}
uint16_t net_checksum(const void *bytes,size_t length)
{
    const uint8_t *p=bytes;uint32_t sum=0;
    while (length>=2) {sum+=get16(p);p+=2;length-=2;}
    if (length) sum+=(unsigned)*p<<8;
    while (sum>>16) sum=(sum&0xffff)+(sum>>16);
    return (uint16_t)~sum;
}
uint16_t net_ipv4_transport_checksum(const uint8_t source[4],const uint8_t dest[4],uint8_t protocol,const void *bytes,size_t n)
{
    uint32_t sum=(uint16_t)~net_checksum(source,4)+(uint32_t)(uint16_t)~net_checksum(dest,4)+protocol+(uint32_t)n+(uint16_t)~net_checksum(bytes,n);
    while (sum>>16) sum=(sum&65535)+(sum>>16);
    return (uint16_t)~sum;
}
static int output_ok(void *out,size_t capacity,size_t *length,size_t needed)
{
    uintptr_t a=(uintptr_t)out,b=(uintptr_t)length;
    return out && length && capacity>=needed && a+needed>a && b+sizeof(*length)>b &&
           !(a<b+sizeof(*length) && b<a+needed);
}
int net_ipv4_init(struct net_ipv4 *c,const uint8_t mac[6],const uint8_t ip[4])
{
    if (!c) return NET_ARP_EINVAL;
    struct net_arp arp;int e=net_arp_init(&arp,mac,ip);if (e) return e;
    memset(c,0,sizeof(*c));memcpy(&c->arp,&arp,sizeof(arp));c->identification=1;return 0;
}
int net_ipv4_begin(const struct net_ipv4 *c,const uint8_t mac[6],const uint8_t ip[4],uint8_t protocol,
                   size_t n,void *buffer,size_t capacity,size_t *length)
{
    if (!c || !mac || !ip || n>1480) return NET_ARP_EINVAL;
    size_t total=20+n,wire=14+total;if (wire<60) wire=60;
    if (!output_ok(buffer,capacity,length,wire)) return NET_ARP_NOSPACE;
    uint8_t m[6],target[4];memcpy(m,mac,6);memcpy(target,ip,4);
    uint8_t *out=buffer;
    memset(out,0,wire);memcpy(out,m,6);memcpy(out+6,c->arp.mac,6);put16(out+12,0x0800);
    uint8_t *h=out+14;h[0]=0x45;put16(h+2,(uint16_t)total);put16(h+4,c->identification);
    put16(h+6,0x4000);h[8]=64;h[9]=protocol;memcpy(h+12,c->arp.ip,4);memcpy(h+16,target,4);put16(h+10,net_checksum(h,20));
    *length=wire;return 0;
}
static size_t build(uint8_t *out,const struct net_ipv4 *c,const uint8_t mac[6],const uint8_t ip[4],
                    unsigned type,uint16_t id,uint16_t seq,const void *data,size_t n)
{
    size_t wire=42+n;if (wire<60) wire=60;
    net_ipv4_begin(c,mac,ip,1,8+n,out,wire,&wire);
    uint8_t *icmp=out+34;icmp[0]=(uint8_t)type;put16(icmp+4,id);put16(icmp+6,seq);
    if (n) memcpy(icmp+8,data,n);
    put16(icmp+2,net_checksum(icmp,8+n));return wire;
}
int net_ipv4_echo_request(struct net_ipv4 *c,const uint8_t peer[4],uint16_t id,uint16_t seq,
                         const void *data,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    if (!c || !peer || !unicast(peer) || same(peer,c->arp.ip,4) || n>NET_PING_DATA_MAX || (!data && n)) return NET_ARP_EINVAL;
    size_t needed=42+n;if (needed<60) needed=60;
    if (!output_ok(out,capacity,length,needed)) return NET_ARP_NOSPACE;
    if (c->pending && now-c->since<NET_PING_TTL) return NET_ARP_BUSY;
    uint8_t mac[6],target[4];memcpy(target,peer,4);
    if (!net_neighbor_get(&c->arp,target,now,mac)) return NET_IP_NEIGHBOR;
    uint8_t packet[42+NET_PING_DATA_MAX];size_t wire=build(packet,c,mac,target,8,id,seq,data,n);
    /* Capture correlation bytes from the constructed packet, even if data/out alias. */
    if (n) memcpy(c->data,packet+42,n);
    memcpy(c->peer,target,4);memcpy(c->peer_mac,mac,6);c->id=id;c->sequence=seq;c->data_length=(uint16_t)n;
    c->since=now;c->pending=1;c->sent++;c->identification++;
    memcpy(out,packet,wire);*length=wire;return NET_IP_ECHO_REQUEST;
}
int net_ipv4_payload(const struct net_ipv4 *c,const void *frame,size_t length,uint8_t protocol,struct net_ipv4_payload *view)
{
    if (!c || !view) return NET_ARP_EINVAL;
    uint16_t type;int e=net_ethernet_type(&c->arp,frame,length,&type);if (e!=1) return e;
    if (type!=0x0800) return NET_ARP_UNSUPPORTED;
    const uint8_t *p=frame;
    if (!same(p,c->arp.mac,6)) return NET_ARP_DROP; /* No broadcast echo replies. */
    size_t available=length-14;if (available<20) return NET_ARP_EINVAL;
    const uint8_t *ip=p+14;size_t header=(ip[0]&15)*4u;
    if ((ip[0]>>4)!=4 || header<20 || header>available) return NET_ARP_EINVAL;
    size_t total=get16(ip+2);
    if (total<header || total>available || total>1500 || !ip[8] || net_checksum(ip,header)) return NET_ARP_EINVAL;
    uint16_t fragments=get16(ip+6);
    if (fragments&0x8000) return NET_ARP_EINVAL;
    if (header!=20 || (fragments&0x3fff) || ip[9]!=protocol) return NET_ARP_UNSUPPORTED;
    if (!same(ip+16,c->arp.ip,4)) return NET_ARP_DROP;
    if (!unicast(ip+12) || same(ip+12,c->arp.ip,4)) return NET_ARP_EINVAL;
    *view=(struct net_ipv4_payload){ip+12,ip+header,total-header};return 1;
}
int net_ipv4_input(struct net_ipv4 *c,const void *frame,size_t length,uint64_t now,void *out,size_t capacity,size_t *output_length)
{
    if (!c) return NET_ARP_EINVAL;
    uint16_t type;int e=net_ethernet_type(&c->arp,frame,length,&type);if (e!=1) return e;
    if (type==0x0806) return net_ethernet_input(&c->arp,frame,length,now,out,capacity,output_length);
    struct net_ipv4_payload view;e=net_ipv4_payload(c,frame,length,1,&view);if (e!=1) return e;
    const uint8_t *p=frame,*ip=p+14,*icmp=view.bytes;size_t icmp_length=view.length;
    if (icmp_length<8 || icmp[1] || net_checksum(icmp,icmp_length)) return NET_ARP_EINVAL;
    if (icmp[0]!=0 && icmp[0]!=8) return NET_ARP_UNSUPPORTED;
    size_t data_length=icmp_length-8;
    if (icmp[0]==0) {
        if (!c->pending || now-c->since>=NET_PING_TTL || !same(ip+12,c->peer,4) || !same(p+6,c->peer_mac,6) ||
            get16(icmp+4)!=c->id || get16(icmp+6)!=c->sequence || data_length!=c->data_length ||
            (data_length && !same(icmp+8,c->data,data_length))) return NET_ARP_DROP;
        c->pending=0;c->matched++;return NET_IP_ECHO_MATCH;
    }
    size_t needed=14+20+icmp_length;if (needed<60) needed=60;
    if (!output_ok(out,capacity,output_length,needed)) return NET_ARP_NOSPACE;
    uint8_t packet[VNET_FRAME_MAX];size_t wire=build(packet,c,p+6,ip+12,0,get16(icmp+4),get16(icmp+6),icmp+8,data_length);
    memcpy(out,packet,wire);*output_length=wire;c->answered++;c->identification++;return NET_ARP_REPLY;
}
