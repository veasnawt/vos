#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net_ipv4.h"
#include "virtio_net.h"
#include "net_boot.h"
static const uint8_t G[]={0x52,0x54,0,0x12,0x34,0x56},P[]={0x52,0x54,0,0xaa,0xbb,0xcc};
static const uint8_t GI[]={10,0,2,15},PI[]={10,0,2,2},DATA[]="VOS ping data";
static unsigned checks;
#define CHECK(x) do {checks++;assert(x);} while(0)
static void put(uint8_t *p,unsigned n) {p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
/* Separate byte-at-a-time reference with immediate carry folding. */
static uint16_t reference(const uint8_t *p,size_t n)
{
    unsigned sum=0;for (size_t i=0;i<n;i++) {sum+=(unsigned)p[i]<<((i&1)?0:8);sum=(sum&65535)+(sum>>16);}
    return (uint16_t)~sum;
}
static void fix_ip(uint8_t *p,size_t n) {p[24]=p[25]=0;put(p+24,reference(p+14,n));}
static void fix_icmp(uint8_t *p,size_t n) {p[36]=p[37]=0;put(p+36,reference(p+34,n));}
static size_t packet(uint8_t *p,unsigned type,unsigned id,unsigned seq,const uint8_t *data,size_t n)
{
    size_t wire=42+n;if (wire<60) wire=60;
    memset(p,0,wire);memcpy(p,G,6);memcpy(p+6,P,6);put(p+12,0x0800);p[14]=0x45;put(p+16,(unsigned)(28+n));
    put(p+18,0x1122);put(p+20,0x4000);p[22]=64;p[23]=1;memcpy(p+26,PI,4);memcpy(p+30,GI,4);
    p[34]=(uint8_t)type;put(p+38,id);put(p+40,seq);if (n) memcpy(p+42,data,n);fix_ip(p,20);fix_icmp(p,8+n);return wire;
}
static void reject(struct net_ipv4 *c,const uint8_t *p,size_t n,int expected,uint64_t now)
{
    uint8_t before[sizeof(*c)],out[VNET_FRAME_MAX+2],sentinel[sizeof(out)];memcpy(before,c,sizeof(*c));memset(out,0xa5,sizeof(out));memset(sentinel,0xa5,sizeof(sentinel));size_t length=777;
    CHECK(net_ipv4_input(c,p,n,now,out+1,VNET_FRAME_MAX,&length)==expected);
    CHECK(length==777 && !memcmp(out,sentinel,sizeof(out)));CHECK(!memcmp(before,c,sizeof(*c)));
}
int main(void)
{
    const uint8_t vector[]={0,1,0xf2,3,0xf4,0xf5,0xf6,0xf7},odd[]={1,2,3};
    CHECK(net_checksum(vector,8)==0x220d);CHECK(net_checksum(odd,3)==0xfbfd);CHECK(net_checksum(NULL,0)==0xffff);
    uint8_t sweep[1500];for (unsigned i=0;i<sizeof(sweep);i++) sweep[i]=(uint8_t)i;
    for (size_t i=0;i<=sizeof(sweep);i++) CHECK(net_checksum(sweep,i)==reference(sweep,i));
    CHECK(net_requested("vos.ipv4=1") && !net_requested("xvos.ipv4=1") && !net_requested("vos.ipv4=10"));
    struct net_ipv4 c;CHECK(net_ipv4_init(&c,G,GI)==0);CHECK(net_ipv4_init(&c,c.arp.mac,c.arp.ip)==0);
    uint8_t p[VNET_FRAME_MAX],saved[VNET_FRAME_MAX],out[VNET_FRAME_MAX+2];size_t length=999;
    size_t wire=packet(p,8,0x1234,7,DATA,13);memcpy(saved,p,wire);
    for (size_t i=0;i<55;i++) reject(&c,p,i,NET_ARP_EINVAL,10);
    reject(&c,p,VNET_FRAME_MAX+1,NET_ARP_EINVAL,10);
    const unsigned offsets[]={14,14,16,22};const unsigned values[]={0x65,0x44,0xff,0};
    for (unsigned i=0;i<4;i++) {memcpy(p,saved,wire);p[offsets[i]]=(uint8_t)values[i];fix_ip(p,20);reject(&c,p,wire,NET_ARP_EINVAL,10);}
    memcpy(p,saved,wire);put(p+16,19);fix_ip(p,20);reject(&c,p,wire,NET_ARP_EINVAL,10);
    memcpy(p,saved,wire);put(p+16,1500);fix_ip(p,20);reject(&c,p,wire,NET_ARP_EINVAL,10);
    memcpy(p,saved,wire);p[14]=0x4f;reject(&c,p,wire,NET_ARP_EINVAL,10);
    memcpy(p,saved,wire);p[24]^=1;reject(&c,p,wire,NET_ARP_EINVAL,10);
    unsigned flags[]={0x8000,0x2000,1,0x6000};
    for (unsigned i=0;i<4;i++) {memcpy(p,saved,wire);put(p+20,flags[i]);fix_ip(p,20);reject(&c,p,wire,i?NET_ARP_UNSUPPORTED:NET_ARP_EINVAL,10);}
    memcpy(p,saved,wire);p[23]=17;fix_ip(p,20);reject(&c,p,wire,NET_ARP_UNSUPPORTED,10);
    memcpy(p,saved,wire);memmove(p+38,p+34,21);memset(p+34,0,4);p[14]=0x46;put(p+16,45);fix_ip(p,24);reject(&c,p,60,NET_ARP_UNSUPPORTED,10);
    memcpy(p,saved,wire);p[33]^=1;fix_ip(p,20);reject(&c,p,wire,NET_ARP_DROP,10);
    unsigned classes[]={0,127,224,255};
    for (unsigned i=0;i<4;i++) {memcpy(p,saved,wire);p[26]=(uint8_t)classes[i];fix_ip(p,20);reject(&c,p,wire,NET_ARP_EINVAL,10);}
    memcpy(p,saved,wire);memcpy(p+26,GI,4);fix_ip(p,20);reject(&c,p,wire,NET_ARP_EINVAL,10);
    memcpy(p,saved,wire);memset(p,255,6);reject(&c,p,wire,NET_ARP_DROP,10);
    memcpy(p,saved,wire);put(p+12,0x86dd);reject(&c,p,wire,NET_ARP_UNSUPPORTED,10);
    memcpy(p,saved,wire);p[6]|=1;reject(&c,p,wire,NET_ARP_EINVAL,10);
    memcpy(p,saved,wire);p[36]^=1;reject(&c,p,wire,NET_ARP_EINVAL,10);
    memcpy(p,saved,wire);p[35]=1;fix_icmp(p,21);reject(&c,p,wire,NET_ARP_EINVAL,10);
    memcpy(p,saved,wire);p[34]=3;fix_icmp(p,21);reject(&c,p,wire,NET_ARP_UNSUPPORTED,10);
    memcpy(p,saved,wire);put(p+16,27);fix_ip(p,20);reject(&c,p,wire,NET_ARP_EINVAL,10);
    packet(p,0,0x564f,1,DATA,13);reject(&c,p,60,NET_ARP_DROP,10);
    memset(out,0xa5,sizeof(out));length=999;
    CHECK(net_ipv4_echo_request(&c,PI,0x564f,1,DATA,13,1,out,1514,&length)==NET_IP_NEIGHBOR && length==999 && !c.pending && out[0]==0xa5);
    CHECK(net_arp_query(&c.arp,PI,1,out,1514,&length)==NET_ARP_REQUEST);
    memset(p,0,60);memcpy(p,G,6);memcpy(p+6,P,6);put(p+12,0x0806);put(p+14,1);put(p+16,0x0800);p[18]=6;p[19]=4;put(p+20,2);memcpy(p+22,P,6);memcpy(p+28,PI,4);memcpy(p+32,G,6);memcpy(p+38,GI,4);
    CHECK(net_ipv4_input(&c,p,60,2,out,1514,&length)==NET_ARP_LEARNED);
    memset(out,0xa5,sizeof(out));
    CHECK(net_ipv4_echo_request(&c,PI,0x564f,1,DATA,13,3,out+1,1514,&length)==NET_IP_ECHO_REQUEST && length==60 && c.pending);
    CHECK(!net_checksum(out+15,20) && !net_checksum(out+35,21) && !memcmp(out+43,DATA,13));
    CHECK(out[0]==0xa5 && out[61]==0xa5);for (size_t i=56;i<=60;i++) CHECK(out[i]==0);
    CHECK(net_ipv4_echo_request(&c,PI,0,0,DATA,13,4,out,1514,&length)==NET_ARP_BUSY);
    packet(p,0,0x564f,99,DATA,13);reject(&c,p,60,NET_ARP_DROP,4);
    packet(p,0,0x564e,1,DATA,13);reject(&c,p,60,NET_ARP_DROP,4);
    packet(p,0,0x564f,1,DATA,14);reject(&c,p,60,NET_ARP_DROP,4);
    packet(p,0,0x564f,1,DATA,13);p[42]^=1;fix_icmp(p,21);reject(&c,p,60,NET_ARP_DROP,4);
    packet(p,0,0x564f,1,sweep,1472);reject(&c,p,1514,NET_ARP_DROP,4);
    packet(p,0,0x564f,1,DATA,13);p[6]^=2;reject(&c,p,60,NET_ARP_DROP,4);
    packet(p,0,0x564f,1,DATA,13);reject(&c,p,60,NET_ARP_DROP,3+NET_PING_TTL);
    length=999;CHECK(net_ipv4_input(&c,p,60,5,out,1514,&length)==NET_IP_ECHO_MATCH && !c.pending && c.matched==1 && length==999);
    uint8_t large[1472];for (unsigned i=0;i<sizeof(large);i++) large[i]=(uint8_t)i;
    wire=packet(p,8,9,10,large,sizeof(large));struct net_ipv4 before=c;length=777;memset(out,0xa5,sizeof(out));
    CHECK(net_ipv4_input(&c,p,wire,6,out+1,1513,&length)==NET_ARP_NOSPACE && length==777 && !memcmp(&c,&before,sizeof(c)) && out[1]==0xa5);
    CHECK(net_ipv4_input(&c,p,wire,6,out+1,1514,&length)==NET_ARP_REPLY && length==1514 && out[0]==0xa5 && out[1515]==0xa5);
    CHECK(!net_checksum(out+15,20) && !net_checksum(out+35,1480) && !memcmp(out+43,large,sizeof(large)));
    CHECK(net_ipv4_input(&c,p,wire,7,p,1514,&length)==NET_ARP_REPLY && !memcmp(p,P,6));
    CHECK(net_ipv4_echo_request(&c,PI,1,2,large,65,8,out,1514,&length)==NET_ARP_EINVAL);
    before=c;CHECK(net_ipv4_echo_request(&c,PI,1,2,large,64,8,out,105,&length)==NET_ARP_NOSPACE && !memcmp(&c,&before,sizeof(c)));
    CHECK(net_ipv4_echo_request(&c,GI,1,2,NULL,0,8,out,1514,&length)==NET_ARP_EINVAL);
    CHECK(net_ipv4_echo_request(&c,PI,2,2,NULL,0,8,out,1514,&length)==NET_IP_ECHO_REQUEST && length==60);
    packet(p,0,2,2,NULL,0);CHECK(net_ipv4_input(&c,p,60,9,out,1514,&length)==NET_IP_ECHO_MATCH);
    memcpy(out,DATA,13);CHECK(net_ipv4_echo_request(&c,PI,3,3,out,13,10,out,1514,&length)==NET_IP_ECHO_REQUEST);
    packet(p,0,3,3,DATA,13);CHECK(net_ipv4_input(&c,p,60,11,out,1514,&length)==NET_IP_ECHO_MATCH);
    CHECK(net_ipv4_echo_request(&c,PI,1,1,DATA,13,NET_ARP_TTL+3,out,1514,&length)==NET_IP_NEIGHBOR);
    CHECK(net_ipv4_input(NULL,p,60,0,out,1514,&length)==NET_ARP_EINVAL);
    printf("IPv4/ICMP host: %u checks PASS; bounds, checksums, correlation and maximum echo response\n",checks);return 0;
}
