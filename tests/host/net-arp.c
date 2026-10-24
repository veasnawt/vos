#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "net_arp.h"
#include "virtio_net.h"
#include "net_boot.h"
static const uint8_t local_mac[]={0x52,0x54,0,0x12,0x34,0x56},peer_mac[]={0x52,0x54,0,0xaa,0xbb,0xcc};
static const uint8_t local_ip[]={10,0,2,15},peer_ip[]={10,0,2,2},broadcast[]={255,255,255,255,255,255},zero[6]={0};
static unsigned checks;
#define CHECK(x) do {checks++;assert(x);} while(0)
static void put(uint8_t *p,unsigned n) {p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static void incoming(uint8_t *p,unsigned op,const uint8_t *spa,const uint8_t *tpa)
{
    memset(p,0,60);memcpy(p,op==1?broadcast:local_mac,6);memcpy(p+6,peer_mac,6);put(p+12,0x0806);
    put(p+14,1);put(p+16,0x0800);p[18]=6;p[19]=4;put(p+20,op);memcpy(p+22,peer_mac,6);memcpy(p+28,spa,4);
    if (op==2) memcpy(p+32,local_mac,6);
    memcpy(p+38,tpa,4);
}
static void rejected(struct net_arp *a,const uint8_t *p,size_t len,int expected)
{
    struct net_arp before=*a;uint8_t guarded[62];memset(guarded,0xa5,sizeof(guarded));size_t n=777;
    CHECK(net_ethernet_input(a,p,len,10,guarded+1,60,&n)==expected);
    CHECK(n==777 && !memcmp(a,&before,sizeof(*a)));
    for (unsigned i=0;i<sizeof(guarded);i++) CHECK(guarded[i]==0xa5);
}
int main(void)
{
    struct net_arp a;uint8_t p[60],out[62],mac[6];size_t n=999;
    CHECK(!net_requested(NULL) && !net_requested("vos.vsh=1") && !net_requested("vos.rixie=1"));
    CHECK(net_requested("vos.net=1") && net_requested("x vos.arp=1\tother"));
    CHECK(!net_requested("xvos.arp=1") && !net_requested("vos.arp=10"));
    CHECK(net_arp_init(&a,local_mac,local_ip)==0);
    struct net_arp before=a;uint8_t badmac[6]={1};CHECK(net_arp_init(&a,badmac,local_ip)==NET_ARP_EINVAL && !memcmp(&a,&before,sizeof(a)));
    CHECK(net_arp_init(&a,local_mac,zero)==NET_ARP_EINVAL);
    CHECK(net_arp_init(&a,a.mac,a.ip)==0); /* Safe reinitialization with aliased inputs. */
    memset(out,0xa5,sizeof(out));CHECK(net_arp_query(&a,peer_ip,1,out+1,59,&n)==NET_ARP_NOSPACE && n==999 && !a.pending);
    for (unsigned i=0;i<sizeof(out);i++) CHECK(out[i]==0xa5);
    CHECK(net_arp_query(&a,peer_ip,1,out+1,60,&n)==NET_ARP_REQUEST && n==60 && a.pending);
    CHECK(out[0]==0xa5 && out[61]==0xa5 && !memcmp(out+1,broadcast,6) && !memcmp(out+7,local_mac,6));
    CHECK(out[13]==8 && out[14]==6 && out[21]==0 && out[22]==1 && !memcmp(out+39,peer_ip,4));
    for (unsigned i=43;i<61;i++) CHECK(out[i]==0);
    CHECK(net_arp_query(&a,peer_ip,2,out,60,&n)==NET_ARP_BUSY);
    incoming(p,2,peer_ip,local_ip);
    for (size_t len=0;len<42;len++) {
        uint8_t exact[42];memcpy(exact,p,len);rejected(&a,exact,len,NET_ARP_EINVAL);
    }
    rejected(&a,p,VNET_FRAME_MAX+1,NET_ARP_EINVAL);
    uint8_t saved[60];memcpy(saved,p,60);
    const unsigned fields[]={18,19,20,21};const uint8_t values[]={5,5,1,3};
    for (unsigned i=0;i<4;i++) {memcpy(p,saved,60);p[fields[i]]=values[i];rejected(&a,p,60,NET_ARP_EINVAL);}
    memcpy(p,saved,60);p[15]=2;rejected(&a,p,60,NET_ARP_UNSUPPORTED);
    memcpy(p,saved,60);p[17]=1;rejected(&a,p,60,NET_ARP_UNSUPPORTED);
    unsigned types[]={0,1500,1501,1535,0x0800,0x86dd,0x8100};
    for (unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++) {memcpy(p,saved,60);put(p+12,types[i]);rejected(&a,p,60,types[i]>1500 && types[i]<1536?NET_ARP_EINVAL:NET_ARP_UNSUPPORTED);}
    memcpy(p,saved,60);p[0]=0x50;rejected(&a,p,60,NET_ARP_DROP);
    memcpy(p,saved,60);p[6]|=1;rejected(&a,p,60,NET_ARP_EINVAL);
    memcpy(p,saved,60);memset(p+6,0,6);memset(p+22,0,6);rejected(&a,p,60,NET_ARP_EINVAL);
    memcpy(p,saved,60);p[22]^=2;rejected(&a,p,60,NET_ARP_EINVAL);
    memcpy(p,saved,60);memcpy(p+28,local_ip,4);rejected(&a,p,60,NET_ARP_EINVAL);
    unsigned first[]={0,127,224,255};
    for (unsigned i=0;i<4;i++) {memcpy(p,saved,60);p[28]=(uint8_t)first[i];rejected(&a,p,60,NET_ARP_EINVAL);}
    memcpy(p,saved,60);p[41]^=1;rejected(&a,p,60,NET_ARP_DROP);
    memcpy(p,saved,60);p[32]^=2;rejected(&a,p,60,NET_ARP_DROP);
    memcpy(p,saved,60);p[31]=9;rejected(&a,p,60,NET_ARP_DROP);
    memcpy(p,saved,60);CHECK(net_ethernet_input(&a,p,42,2,out,60,&n)==NET_ARP_LEARNED && !a.pending);
    CHECK(net_neighbor_get(&a,peer_ip,3,mac) && !memcmp(mac,peer_mac,6));
    n=888;CHECK(net_arp_query(&a,peer_ip,3,out,60,&n)==NET_ARP_CACHED && n==888);
    rejected(&a,p,60,NET_ARP_DROP); /* Unsolicited replay does not refresh the cache. */
    CHECK(net_neighbor_get(&a,peer_ip,NET_ARP_TTL+1,mac));
    memset(mac,0x5a,6);CHECK(!net_neighbor_get(&a,peer_ip,NET_ARP_TTL+2,mac) && mac[0]==0x5a);
    CHECK(net_arp_query(&a,peer_ip,7000,out,60,&n)==NET_ARP_REQUEST);
    CHECK(net_arp_query(&a,peer_ip,7000+NET_ARP_QUERY_TTL,out,60,&n)==NET_ARP_REQUEST); /* Explicit retry after expiry. */
    incoming(p,1,peer_ip,local_ip);before=a;n=123;memset(out,0x5a,sizeof(out));
    CHECK(net_ethernet_input(&a,p,60,7201,out,59,&n)==NET_ARP_NOSPACE && n==123 && !memcmp(&a,&before,sizeof(a)));
    for (unsigned i=0;i<sizeof(out);i++) CHECK(out[i]==0x5a);
    CHECK(net_ethernet_input(&a,p,60,7201,out+1,60,&n)==NET_ARP_REPLY && n==60 && out[0]==0x5a && out[61]==0x5a);
    CHECK(!memcmp(out+1,peer_mac,6) && !memcmp(out+7,local_mac,6) && out[22]==2 && !memcmp(out+33,peer_mac,6));
    CHECK(!a.pending);incoming(p,1,peer_ip,local_ip);CHECK(net_ethernet_input(&a,p,60,7202,p,60,&n)==NET_ARP_REPLY && !memcmp(p,peer_mac,6));
    incoming(p,1,zero,local_ip);CHECK(net_ethernet_input(&a,p,60,7203,out,60,&n)==NET_ARP_REPLY && !memcmp(out+38,zero,4));
    CHECK(!net_neighbor_get(&a,zero,7203,mac));
    CHECK(net_arp_init(&a,local_mac,local_ip)==0);
    for (unsigned i=1;i<=5;i++) {uint8_t ip[]={10,0,2,(uint8_t)(20+i)};incoming(p,1,ip,local_ip);CHECK(net_ethernet_input(&a,p,60,i,out,60,&n)==NET_ARP_REPLY);}
    const uint8_t oldest[]={10,0,2,21},newest[]={10,0,2,25};
    CHECK(!net_neighbor_get(&a,oldest,6,mac) && net_neighbor_get(&a,newest,6,mac));
    unsigned live=0;for (unsigned i=0;i<NET_ARP_NEIGHBORS;i++) live+=a.neighbors[i].valid!=0;CHECK(live==NET_ARP_NEIGHBORS);
    CHECK(net_arp_init(&a,local_mac,local_ip)==0);incoming(p,1,peer_ip,local_ip);
    CHECK(net_ethernet_input(&a,p,60,UINT64_MAX-20,out,60,&n)==NET_ARP_REPLY);
    CHECK(net_neighbor_get(&a,peer_ip,20,mac));CHECK(!net_neighbor_get(&a,peer_ip,NET_ARP_TTL+20,mac));
    CHECK(net_arp_query(&a,local_ip,0,out,60,&n)==NET_ARP_EINVAL);
    CHECK(net_ethernet_input(NULL,p,60,0,out,60,&n)==NET_ARP_EINVAL);
    printf("Ethernet/ARP host: %u checks PASS; malformed input, bounded cache, expiry and output guards\n",checks);
    return 0;
}
