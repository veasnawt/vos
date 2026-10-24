#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net_udp.h"
#include "net_boot.h"
static const uint8_t G[]={0x52,0x54,0,0x12,0x34,0x56},P[]={0x52,0x54,0,0xaa,0xbb,0xcc},GI[]={10,0,2,15},PI[]={10,0,2,2};
static unsigned checks;
#define CHECK(x) do {checks++;assert(x);} while (0)
static void put(uint8_t *p,unsigned x) {p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static unsigned get(const uint8_t *p) {return (unsigned)p[0]*256+p[1];}
/* Independent byte-at-a-time reference, folding after each byte. */
static uint16_t reference(const uint8_t *p,size_t n)
{
    unsigned sum=0;for (size_t i=0;i<n;i++) {sum+=(unsigned)p[i]<<((i&1)?0:8);sum=(sum&65535)+(sum>>16);}
    return (uint16_t)~sum;
}
static uint16_t udp_sum(const uint8_t *p,size_t n)
{
    uint8_t b[12+1480];memcpy(b,p+26,8);b[8]=0;b[9]=17;put(b+10,(unsigned)n);memcpy(b+12,p+34,n);return reference(b,12+n);
}
static void fix_ip(uint8_t *p) {put(p+24,0);put(p+24,reference(p+14,20));}
static void fix_udp(uint8_t *p,size_t n) {put(p+40,0);uint16_t x=udp_sum(p,n);put(p+40,x?x:65535);}
static size_t packet(uint8_t *p,const void *data,size_t n,unsigned src,unsigned dst,int checksum)
{
    size_t wire=42+n;if (wire<60) wire=60;
    memset(p,0,wire);memcpy(p,G,6);memcpy(p+6,P,6);put(p+12,0x0800);p[14]=0x45;put(p+16,(unsigned)(28+n));put(p+18,1);put(p+20,0x4000);p[22]=64;p[23]=17;memcpy(p+26,PI,4);memcpy(p+30,GI,4);fix_ip(p);
    put(p+34,src);put(p+36,dst);put(p+38,(unsigned)(8+n));if (n) memcpy(p+42,data,n);if (checksum) fix_udp(p,8+n);return wire;
}
static void reject(struct net_udp *c,const void *p,size_t n,int expected)
{
    struct net_udp saved=*c;uint8_t out[1516],guard[1516];memset(out,0xa5,sizeof(out));memcpy(guard,out,sizeof(out));size_t length=777;
    CHECK(net_udp_input(c,p,n,10,out+1,1514,&length)==expected);CHECK(length==777 && !memcmp(out,guard,sizeof(out)));CHECK(!memcmp(c,&saved,sizeof(*c)));
}
int main(void)
{
    struct net_udp c;uint8_t p[1514],saved[1514],out[1516],data[1472];size_t length=999;struct net_udp_datagram d;
    for (unsigned i=0;i<sizeof(data);i++) data[i]=(uint8_t)i;
    CHECK(net_udp_init(NULL,G,GI,4000)==NET_ARP_EINVAL);CHECK(net_udp_init(&c,G,GI,0)==NET_ARP_EINVAL);
    CHECK(net_udp_init(&c,G,GI,4000)==0);CHECK(net_udp_init(&c,c.ip.arp.mac,c.ip.arp.ip,4000)==0);
    CHECK(net_requested("vos.udp=1") && !net_requested("xvos.udp=1") && !net_requested("vos.udp=10") && !net_requested("vos.vsh=1") && !net_requested("vos.rixie=1"));
    memset(out,0xa5,sizeof(out));CHECK(net_udp_send(&c,PI,5000,data,13,1,out,1514,&length)==NET_IP_NEIGHBOR && length==999 && out[0]==0xa5);
    CHECK(net_arp_query(&c.ip.arp,PI,1,out,1514,&length)==NET_ARP_REQUEST);
    memset(p,0,60);memcpy(p,G,6);memcpy(p+6,P,6);put(p+12,0x0806);put(p+14,1);put(p+16,0x0800);p[18]=6;p[19]=4;put(p+20,2);memcpy(p+22,P,6);memcpy(p+28,PI,4);memcpy(p+32,G,6);memcpy(p+38,GI,4);
    CHECK(net_udp_input(&c,p,60,2,out,1514,&length)==NET_ARP_LEARNED);
    for (size_t n=0;n<=512;n++) {
        memset(out,0xa5,sizeof(out));length=999;
        CHECK(net_udp_send(&c,PI,5000,n?data:NULL,n,3,out+1,1514,&length)==NET_UDP_SENT);
        size_t wire=n+42;if (wire<60) wire=60;
        CHECK(length==wire && out[0]==0xa5 && out[wire+1]==0xa5 && !reference(out+15,20) && !udp_sum(out+1,8+n));
        CHECK(get(out+35)==4000 && get(out+37)==5000 && get(out+39)==8+n && get(out+41)!=0 && !memcmp(out+43,data,n));
        for (size_t i=42+n;i<wire;i++) CHECK(out[i+1]==0);
        wire=packet(p,data,n,5000,4000,1);
        CHECK(net_udp_input(&c,p,wire,4,out,1514,&length)==NET_UDP_QUEUED && c.count==1);
        CHECK(net_udp_receive(&c,&d)==0 && d.length==n && d.port==5000 && d.checksum_present && !memcmp(d.source,PI,4) && !memcmp(d.data,data,n));
        for (size_t i=n;i<512;i++) CHECK(d.data[i]==0);
    }
    size_t wire=packet(p,data,13,5000,4000,1);memcpy(saved,p,wire);
    for (size_t n=0;n<55;n++) reject(&c,p,n,NET_ARP_EINVAL);
    reject(&c,p,1515,NET_ARP_EINVAL);
    for (unsigned n=0;n<8;n++) {memcpy(p,saved,wire);put(p+38,n);reject(&c,p,wire,NET_ARP_EINVAL);}
    unsigned sizes[]={20,22,65535};for (unsigned i=0;i<3;i++) {memcpy(p,saved,wire);put(p+38,sizes[i]);reject(&c,p,wire,NET_ARP_EINVAL);}
    memcpy(p,saved,wire);p[40]^=1;reject(&c,p,wire,NET_ARP_EINVAL);
    memcpy(p,saved,wire);p[29]^=1;fix_ip(p);reject(&c,p,wire,NET_ARP_EINVAL);
    memcpy(p,saved,wire);put(p+36,0);fix_udp(p,21);reject(&c,p,wire,NET_ARP_EINVAL);
    memcpy(p,saved,wire);put(p+36,4001);fix_udp(p,21);reject(&c,p,wire,NET_ARP_DROP);
    wire=packet(p,data,513,5000,4000,1);reject(&c,p,wire,NET_ARP_NOSPACE);
    wire=packet(p,data,1472,5000,4000,1);reject(&c,p,wire,NET_ARP_NOSPACE);
    wire=packet(p,data,13,0,4000,0);CHECK(net_udp_input(&c,p,wire,5,out,1514,&length)==NET_UDP_QUEUED);
    CHECK(net_udp_receive(&c,&d)==0 && d.port==0 && !d.checksum_present && d.length==13);
    const uint8_t zero[]={0xc4,0xa1};wire=packet(p,zero,2,5000,4000,1);CHECK(get(p+40)==65535);
    CHECK(net_udp_input(&c,p,wire,6,out,1514,&length)==NET_UDP_QUEUED && !net_udp_receive(&c,&d) && d.checksum_present);
    CHECK(net_udp_send(&c,PI,5000,zero,2,6,out,1514,&length)==NET_UDP_SENT && get(out+40)==65535);
    /* FIFO wrap, full-queue rejection, invalid-before-full and recovery. */
    for (unsigned i=0;i<4;i++) {data[0]=(uint8_t)i;wire=packet(p,data,1,5000,4000,1);CHECK(net_udp_input(&c,p,wire,7,out,1514,&length)==NET_UDP_QUEUED);}
    reject(&c,p,wire,NET_UDP_FULL);p[40]^=1;reject(&c,p,wire,NET_ARP_EINVAL);
    CHECK(net_udp_receive(&c,NULL)==NET_ARP_EINVAL && c.count==4);
    for (unsigned i=0;i<2;i++) CHECK(net_udp_receive(&c,&d)==0 && d.data[0]==i);
    for (unsigned i=4;i<6;i++) {data[0]=(uint8_t)i;wire=packet(p,data,1,65535,4000,1);CHECK(net_udp_input(&c,p,wire,8,out,1514,&length)==NET_UDP_QUEUED);}
    for (unsigned i=2;i<6;i++) CHECK(net_udp_receive(&c,&d)==0 && d.data[0]==i);
    memset(&d,0xa5,sizeof(d));struct net_udp_datagram untouched=d;
    CHECK(net_udp_receive(&c,&d)==NET_UDP_EMPTY && !memcmp(&d,&untouched,sizeof(d)));
    struct net_udp before=c;memset(out,0xa5,sizeof(out));length=999;
    CHECK(net_udp_send(&c,PI,0,data,1,9,out,1514,&length)==NET_ARP_EINVAL);
    CHECK(net_udp_send(&c,GI,5000,data,1,9,out,1514,&length)==NET_ARP_EINVAL);
    CHECK(net_udp_send(&c,PI,5000,NULL,1,9,out,1514,&length)==NET_ARP_EINVAL);
    CHECK(net_udp_send(&c,PI,5000,data,513,9,out,1514,&length)==NET_ARP_EINVAL);
    CHECK(net_udp_send(&c,PI,5000,data,512,9,out,553,&length)==NET_ARP_NOSPACE);
    CHECK(net_udp_send(&c,PI,5000,data,1,9,NULL,1514,&length)==NET_ARP_NOSPACE);
    CHECK(net_udp_send(&c,PI,5000,data,1,9,out,1514,(size_t *)(void *)out)==NET_ARP_NOSPACE);
    CHECK(!memcmp(&c,&before,sizeof(c)) && length==999 && out[0]==0xa5);
    memcpy(out,"alias",5);CHECK(net_udp_send(&c,PI,65535,out,5,9,out,1514,&length)==NET_UDP_SENT && !memcmp(out+42,"alias",5) && !udp_sum(out,13));
    /* Existing ICMP responder still dispatches through the combined context. */
    wire=packet(p,data,1,0,0,0);p[23]=1;memset(p+34,0,9);p[34]=8;put(p+38,0x1234);put(p+40,1);p[42]='X';put(p+36,reference(p+34,9));fix_ip(p);
    CHECK(net_udp_input(&c,p,wire,10,out,1514,&length)==NET_ARP_REPLY && out[34]==0 && !reference(out+34,9) && c.count==0 && c.ip.answered==1);
    wire=packet(p,data,1,5000,4000,1);CHECK(net_udp_input(&c,p,wire,11,out,1514,&length)==NET_UDP_QUEUED);
    net_udp_close(&c);for (size_t i=0;i<sizeof(c);i++) CHECK(((uint8_t *)&c)[i]==0);
    CHECK(net_udp_input(&c,p,wire,11,out,1514,&length)==NET_ARP_EINVAL);
    CHECK(net_udp_send(&c,PI,5000,data,1,11,out,1514,&length)==NET_ARP_EINVAL);
    CHECK(net_udp_receive(&c,&d)==NET_ARP_EINVAL);net_udp_close(NULL);
    printf("UDP host: %u checks PASS; pseudoheader/zero checksums, lengths, ports, FIFO bounds and erasure\n",checks);return 0;
}
