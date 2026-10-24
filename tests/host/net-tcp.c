#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net_tcp.h"
#include "net_boot.h"
static const uint8_t G[]={0x52,0x54,0,0x12,0x34,0x56},P[]={0x52,0x54,0,0xaa,0xbb,0xcc},GI[]={10,0,2,15},PI[]={10,0,2,2};
static unsigned checks;
#define CHECK(x) do {checks++;assert(x);} while (0)
static void put(uint8_t *p,unsigned x) {p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void put32(uint8_t *p,uint32_t x) {p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint16_t reference(const uint8_t *p,size_t n)
{
    unsigned sum=0;for (size_t i=0;i<n;i++) {sum+=(unsigned)p[i]<<((i&1)?0:8);sum=(sum&65535)+(sum>>16);}
    return (uint16_t)~sum;
}
static uint16_t transport(const uint8_t source[4],const uint8_t dest[4],unsigned protocol,const uint8_t *p,size_t n)
{
    uint8_t b[12+1480];memcpy(b,source,4);memcpy(b+4,dest,4);b[8]=0;b[9]=(uint8_t)protocol;put(b+10,(unsigned)n);memcpy(b+12,p,n);return reference(b,12+n);
}
static void fix_ip(uint8_t *p) {put(p+24,0);put(p+24,reference(p+14,20));}
static void fix_tcp(uint8_t *p,size_t n) {put(p+50,0);put(p+50,transport(p+26,p+30,6,p+34,n));}
static size_t packet(uint8_t *p,unsigned flags,uint32_t seq,uint32_t ack,unsigned words,size_t extra)
{
    size_t wire=54+extra;if (wire<60) wire=60;
    memset(p,0,wire);memcpy(p,G,6);memcpy(p+6,P,6);put(p+12,0x0800);p[14]=0x45;put(p+16,(unsigned)(40+extra));put(p+20,0x4000);p[22]=64;p[23]=6;memcpy(p+26,PI,4);memcpy(p+30,GI,4);fix_ip(p);
    put(p+34,5001);put(p+36,4001);put32(p+38,seq);put32(p+42,ack);p[46]=(uint8_t)(words<<4);p[47]=(uint8_t)flags;put(p+48,1024);fix_tcp(p,20+extra);return wire;
}
static void reject(struct net_tcp *c,const void *p,size_t n,int expected,uint64_t now)
{
    struct net_tcp before=*c;uint8_t out[62],guard[62];memset(out,0xa5,sizeof(out));memcpy(guard,out,sizeof(out));size_t size=777;
    CHECK(net_tcp_input(c,p,n,now,out+1,60,&size)==expected);CHECK(size==777 && !memcmp(out,guard,sizeof(out)));CHECK(!memcmp(c,&before,sizeof(*c)));
}
int main(void)
{
    struct net_tcp c;uint8_t p[1514],saved[1514],out[62],data[1480];size_t size=999;
    CHECK(sizeof(c)<=512);CHECK(net_tcp_init(NULL,G,GI,4001,1)==NET_ARP_EINVAL);CHECK(net_tcp_init(&c,G,GI,0,1)==NET_ARP_EINVAL);
    CHECK(net_tcp_init(&c,G,GI,4001,0x11223344)==0);CHECK(net_tcp_init(&c,c.ip.arp.mac,c.ip.arp.ip,4001,0x11223344)==0);
    CHECK(net_requested("vos.tcp=1") && !net_requested("xvos.tcp=1") && !net_requested("vos.tcp=10") && !net_requested("vos.vsh=1") && !net_requested("vos.rixie=1"));
    for (unsigned i=0;i<sizeof(data);i++) data[i]=(uint8_t)i;
    for (size_t n=0;n<=sizeof(data);n++) for (unsigned protocol=6;protocol<=17;protocol+=11)
        CHECK(net_ipv4_transport_checksum(GI,PI,(uint8_t)protocol,data,n)==transport(GI,PI,protocol,data,n));
    size_t wire=packet(p,2,0xffffffffu,0,5,0);memcpy(saved,p,wire);
    for (size_t n=0;n<54;n++) reject(&c,p,n,NET_ARP_EINVAL,1);
    reject(&c,p,1515,NET_ARP_EINVAL,1);
    for (unsigned i=0;i<16;i++) if (i!=5) {memcpy(p,saved,wire);p[46]=(uint8_t)(i<<4);fix_tcp(p,20);reject(&c,p,wire,NET_ARP_EINVAL,1);}
    for (unsigned i=1;i<=8;i*=2) {memcpy(p,saved,wire);p[46]|=(uint8_t)i;fix_tcp(p,20);reject(&c,p,wire,NET_ARP_EINVAL,1);}
    unsigned flags[]={0,3,4,17,18,0xc2};
    for (unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);i++) {packet(p,flags[i],1,0,5,0);reject(&c,p,60,NET_ARP_UNSUPPORTED,1);}
    packet(p,16,1,1,5,0);reject(&c,p,60,NET_ARP_DROP,1);
    packet(p,2,1,1,5,0);reject(&c,p,60,NET_ARP_DROP,1);
    memcpy(p,saved,wire);p[50]^=1;reject(&c,p,wire,NET_ARP_EINVAL,1);
    memcpy(p,saved,wire);p[24]^=1;reject(&c,p,wire,NET_ARP_EINVAL,1);
    memcpy(p,saved,wire);p[29]^=1;fix_ip(p);reject(&c,p,wire,NET_ARP_EINVAL,1);
    for (unsigned offset=34;offset<=36;offset+=2) {memcpy(p,saved,wire);put(p+offset,0);fix_tcp(p,20);reject(&c,p,wire,NET_ARP_EINVAL,1);}
    memcpy(p,saved,wire);put(p+36,4002);fix_tcp(p,20);reject(&c,p,wire,NET_ARP_DROP,1);
    memcpy(p,saved,wire);put(p+52,1);fix_tcp(p,20);reject(&c,p,wire,NET_ARP_UNSUPPORTED,1);
    packet(p,2,1,0,6,4);reject(&c,p,60,NET_ARP_UNSUPPORTED,1);
    packet(p,2,1,0,5,1);reject(&c,p,60,NET_ARP_UNSUPPORTED,1);
    memcpy(p,saved,wire);put(p+16,39);fix_ip(p);reject(&c,p,wire,NET_ARP_EINVAL,1);
    memcpy(p,saved,wire);p[23]=17;fix_ip(p);reject(&c,p,wire,NET_ARP_UNSUPPORTED,1);
    memcpy(p,saved,wire);put(p+20,0x2000);fix_ip(p);reject(&c,p,wire,NET_ARP_UNSUPPORTED,1);
    struct net_tcp before=c;memcpy(p,saved,wire);memset(out,0xa5,sizeof(out));size=999;
    CHECK(net_tcp_input(&c,p,wire,2,out+1,59,&size)==NET_ARP_NOSPACE);
    CHECK(net_tcp_input(&c,p,wire,2,NULL,60,&size)==NET_ARP_NOSPACE);
    CHECK(net_tcp_input(&c,p,wire,2,out,60,(size_t *)(void *)out)==NET_ARP_NOSPACE);
    CHECK(!memcmp(&c,&before,sizeof(c)) && size==999 && out[0]==0xa5);
    CHECK(net_tcp_input(&c,p,wire,2,out+1,60,&size)==NET_TCP_SYNACK && size==60 && out[0]==0xa5 && out[61]==0xa5);
    CHECK(c.state==NET_TCP_SYN_RECEIVED && c.isn==0x11223344 && c.snd_next==0x11223345 && c.rcv_next==0 && c.peer_port==5001);
    CHECK(get32(out+39)==0x11223344 && get32(out+43)==0 && out[47]==0x50 && out[48]==18 && out[49]==0 && out[50]==0);
    CHECK(!reference(out+15,20) && !transport(GI,PI,6,out+35,20));for (unsigned i=55;i<=60;i++) CHECK(out[i]==0);
    packet(p,16,0,0x11223346,5,0);reject(&c,p,60,NET_ARP_DROP,3);
    packet(p,16,1,0x11223345,5,0);reject(&c,p,60,NET_ARP_DROP,3);
    packet(p,16,0,0x11223345,5,0);p[29]^=1;fix_ip(p);fix_tcp(p,20);reject(&c,p,60,NET_ARP_DROP,3);
    packet(p,16,0,0x11223345,5,0);p[6]^=4;reject(&c,p,60,NET_ARP_DROP,3);
    packet(p,2,55,0,5,0);put(p+34,5002);fix_tcp(p,20);reject(&c,p,60,NET_ARP_DROP,3);
    packet(p,16,0,0x11223345,5,0);CHECK(net_tcp_input(&c,p,60,201,out,60,&size)==NET_TCP_CONNECTED && c.state==NET_TCP_ESTABLISHED);
    reject(&c,p,60,NET_ARP_DROP,202);CHECK(net_tcp_expire(&c,9999)==0);net_tcp_close(&c);
    for (size_t i=0;i<sizeof(c);i++) CHECK(((uint8_t *)&c)[i]==0);
    CHECK(net_tcp_input(&c,p,60,203,out,60,&size)==NET_ARP_EINVAL);
    CHECK(net_tcp_expire(&c,203)==NET_ARP_EINVAL);
    /* Both sequence counters and tick arithmetic wrap without signed overflow. */
    CHECK(net_tcp_init(&c,G,GI,4001,0xffffffffu)==0);packet(p,2,0xffffffffu,0,5,0);
    CHECK(net_tcp_input(&c,p,60,UINT64_MAX-100,p,sizeof(p),&size)==NET_TCP_SYNACK && c.snd_next==0 && c.rcv_next==0);
    CHECK(net_tcp_expire(&c,98)==0);packet(p,16,0,0,5,0);reject(&c,p,60,NET_TCP_EXPIRED,99);
    CHECK(net_tcp_expire(&c,99)==1 && c.state==NET_TCP_LISTEN && !c.peer_port && !c.isn && !c.snd_next && !c.rcv_next && !c.since);
    CHECK(net_tcp_expire(&c,100)==0 && c.next_isn==0xffff);
    packet(p,2,12,0,5,0);CHECK(net_tcp_input(&c,p,60,101,out,60,&size)==NET_TCP_SYNACK && c.isn==0xffff);
    packet(p,16,0,0,5,0);reject(&c,p,60,NET_ARP_DROP,102);
    packet(p,16,13,0x10000,5,0);CHECK(net_tcp_input(&c,p,60,103,out,60,&size)==NET_TCP_CONNECTED);
    /* TCP checksum zero is mandatory checksum arithmetic, not UDP omission. */
    net_tcp_close(&c);CHECK(net_tcp_init(&c,G,GI,4001,1)==0);packet(p,2,1,0,5,0);int found=0;
    for (unsigned window=0;window<=65535;window++) {put(p+48,window);put(p+50,0);if (!transport(PI,GI,6,p+34,20)) {found=1;break;}}
    CHECK(found && p[50]==0 && p[51]==0);CHECK(net_tcp_input(&c,p,60,1,out,60,&size)==NET_TCP_SYNACK);
    net_tcp_close(&c);CHECK(net_tcp_init(&c,G,GI,65535,1)==0);packet(p,2,5,0,5,0);put(p+34,65535);put(p+36,65535);fix_tcp(p,20);
    CHECK(net_tcp_input(&c,p,60,1,out,60,&size)==NET_TCP_SYNACK && c.peer_port==65535);net_tcp_close(&c);net_tcp_close(NULL);
    printf("TCP host: %u checks PASS; strict headers/checksums, one connection, sequence wrap, ACK validation, expiry and erasure\n",checks);return 0;
}
