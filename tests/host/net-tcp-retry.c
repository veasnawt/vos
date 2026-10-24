#define main legacy_tcp_tests_main
#include "net-tcp.c"
#undef main
static const uint32_t S=0x11223345,R=0x01020305;
static void connect(struct net_tcp *c,uint32_t isn,uint32_t peer)
{
    uint8_t p[1514],out[128];size_t n;
    CHECK(net_tcp_init(c,G,GI,4001,isn)==0 && net_tcp_enable_payload(c)==0 && net_tcp_enable_retransmission(c)==0);
    packet(p,2,peer,0,5,0);CHECK(net_tcp_input(c,p,60,1,out,sizeof(out),&n)==NET_TCP_SYNACK);
    packet(p,16,peer+1u,isn+1u,5,0);put(p+48,64);fix_tcp(p,20);CHECK(net_tcp_input(c,p,60,2,out,sizeof(out),&n)==NET_TCP_CONNECTED);
}
static void control(uint8_t *p,uint32_t seq,uint32_t ack,unsigned window)
{
    packet(p,16,seq,ack,5,0);put(p+48,window);fix_tcp(p,20);
}
static void unchanged(struct net_tcp *c,uint64_t now,int expected)
{
    struct net_tcp saved=*c;uint8_t out[128],guard[128];memset(out,0xa5,sizeof(out));memcpy(guard,out,sizeof(out));size_t n=777;
    CHECK(net_tcp_retransmit(c,now,out,sizeof(out),&n)==expected);CHECK(n==777 && !memcmp(out,guard,sizeof(out)));CHECK(!memcmp(c,&saved,sizeof(*c)));
}
static void cleared(const struct net_tcp *c)
{
    CHECK(c->state==NET_TCP_LISTEN && c->retry_enabled && c->payload_enabled && !c->retry_count && !c->retry_after && !c->tx_since && !c->snd_una && !c->snd_next && !c->rx_used && !c->tx_used && !c->peer_port);
    for (unsigned i=0;i<64;i++) CHECK(!c->tx[i] && !c->rx[i]);
}
int main(void)
{
    struct net_tcp c;uint8_t p[1514],out[128],data[64];size_t n=777;
    for (unsigned i=0;i<64;i++) data[i]=(uint8_t)i;
    CHECK(sizeof(c)==504 && sizeof(c)<=512);CHECK(net_tcp_enable_retransmission(NULL)==NET_ARP_EINVAL);
    CHECK(net_tcp_init(&c,G,GI,4001,S-1)==0 && net_tcp_enable_retransmission(&c)==NET_ARP_EINVAL);
    connect(&c,S-1,R-1);CHECK(net_tcp_enable_retransmission(&c)==NET_ARP_BUSY);unchanged(&c,3,NET_TCP_EMPTY);
    CHECK(net_requested("vos.tcp-retry=1") && !net_requested("vos.tcp-retry=10") && !net_requested("xvos.tcp-retry=1") && !net_requested("vos.vsh=1") && !net_requested("vos.rixie=1"));
    for (unsigned bytes=1;bytes<=64;bytes++) {
        connect(&c,S-1,R-1);CHECK(net_tcp_send(&c,data,bytes,3,out,sizeof(out),&n)==NET_TCP_TX_DATA);
        unchanged(&c,52,NET_TCP_NOT_DUE);struct net_tcp before=c;n=777;memset(out,0xa5,sizeof(out));
        size_t needed=54+bytes;if (needed<60) needed=60;
        CHECK(net_tcp_retransmit(&c,53,out,needed-1,&n)==NET_ARP_NOSPACE && n==777 && out[0]==0xa5 && !memcmp(&c,&before,sizeof(c)));
        CHECK(net_tcp_retransmit(&c,53,out+1,sizeof(out)-1,&n)==NET_TCP_RETRANSMITTED && n==needed && out[0]==0xa5 && out[needed+1]==0xa5);
        CHECK(get32(out+39)==S && !memcmp(out+55,data,bytes) && !transport(GI,PI,6,out+35,20+bytes));
        CHECK(c.retry_count==1 && c.retry_after==100 && c.snd_next==S+bytes && c.snd_una==S && c.tx_used==bytes && !memcmp(c.tx,data,bytes) && c.tx_since==3);
        unchanged(&c,53,NET_TCP_NOT_DUE);
    }
    connect(&c,S-1,R-1);CHECK(net_tcp_send(&c,data,64,3,out,sizeof(out),&n)==NET_TCP_TX_DATA);
    CHECK(net_tcp_retransmit(&c,53,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED);
    control(p,R,S,64);CHECK(net_tcp_input(&c,p,60,54,out,sizeof(out),&n)==NET_TCP_ACKED && c.retry_after==100 && c.retry_count==1 && c.tx_since==3);
    control(p,R,S+65,64);reject(&c,p,60,NET_ARP_DROP,55);
    control(p,R,S+0x80000000u,64);reject(&c,p,60,NET_ARP_DROP,55);
    control(p,R,S+16,64);CHECK(net_tcp_input(&c,p,60,60,out,sizeof(out),&n)==NET_TCP_ACKED && c.tx_used==48 && c.retry_count==1 && c.retry_after==107 && c.tx_since==3);
    unchanged(&c,109,NET_TCP_NOT_DUE);
    CHECK(net_tcp_retransmit(&c,110,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && get32(out+38)==S+16 && !memcmp(out+54,data+16,48) && c.snd_next==S+64 && c.retry_count==2);
    control(p,R,S+64,64);CHECK(net_tcp_input(&c,p,60,111,out,sizeof(out),&n)==NET_TCP_ACKED && !c.tx_used && !c.retry_count && !c.retry_after && !c.tx_since);unchanged(&c,112,NET_TCP_EMPTY);
    CHECK(net_tcp_send(&c,data,8,120,out,sizeof(out),&n)==NET_TCP_TX_DATA);
    CHECK(net_tcp_retransmit(&c,259,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && c.retry_after==189 && c.retry_count==1);unchanged(&c,260,NET_TCP_NOT_DUE);
    CHECK(net_tcp_retransmit(&c,309,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && c.retry_after==239);
    CHECK(net_tcp_retransmit(&c,320,out,sizeof(out),&n)==NET_TCP_EXPIRED);cleared(&c);unchanged(&c,321,NET_ARP_EINVAL);
    connect(&c,S-1,R-1);CHECK(net_tcp_send(&c,data,16,3,out,sizeof(out),&n)==NET_TCP_TX_DATA);
    for (unsigned i=1;i<=3;i++) {unchanged(&c,3+i*50-1,NET_TCP_NOT_DUE);CHECK(net_tcp_retransmit(&c,3+i*50,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && c.retry_count==i);}
    unchanged(&c,202,NET_TCP_NOT_DUE);CHECK(net_tcp_retransmit(&c,203,out,sizeof(out),&n)==NET_TCP_EXPIRED);cleared(&c);unchanged(&c,204,NET_ARP_EINVAL);
    /* Window shrink limits the retry prefix; zero window is bounded by TX TTL. */
    connect(&c,S-1,R-1);CHECK(net_tcp_send(&c,data,16,3,out,sizeof(out),&n)==NET_TCP_TX_DATA);
    control(p,R,S,0);CHECK(net_tcp_input(&c,p,60,4,out,sizeof(out),&n)==NET_TCP_ACKED);unchanged(&c,53,NET_TCP_WINDOW);
    control(p,R,S,4);CHECK(net_tcp_input(&c,p,60,54,out,sizeof(out),&n)==NET_TCP_ACKED);
    CHECK(net_tcp_retransmit(&c,54,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && n==60 && get32(out+38)==S && !memcmp(out+54,data,4) && c.tx_used==16);
    control(p,R,S+4,4);CHECK(net_tcp_input(&c,p,60,55,out,sizeof(out),&n)==NET_TCP_ACKED);
    CHECK(net_tcp_retransmit(&c,105,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && get32(out+38)==S+4 && !memcmp(out+54,data+4,4) && c.snd_next==S+16);
    control(p,R,S+16,64);CHECK(net_tcp_input(&c,p,60,106,out,sizeof(out),&n)==NET_TCP_ACKED);unchanged(&c,107,NET_TCP_EMPTY);
    /* Peer/local sequence wrap and monotonic tick wrap. */
    connect(&c,0xfffffffdu,0xfffffffdu);c.since=UINT64_MAX-21;
    CHECK(net_tcp_send(&c,data,4,UINT64_MAX-20,out,sizeof(out),&n)==NET_TCP_TX_DATA);unchanged(&c,28,NET_TCP_NOT_DUE);
    CHECK(net_tcp_retransmit(&c,29,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && get32(out+38)==0xfffffffeu);
    control(p,0xfffffffeu,0,64);CHECK(net_tcp_input(&c,p,60,30,out,sizeof(out),&n)==NET_TCP_ACKED && c.tx_used==2);
    unchanged(&c,79,NET_TCP_NOT_DUE);CHECK(net_tcp_retransmit(&c,80,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && get32(out+38)==0 && !memcmp(out+54,data+2,2));
    control(p,0xfffffffeu,3,64);reject(&c,p,60,NET_ARP_DROP,81);
    control(p,0xfffffffeu,2,64);CHECK(net_tcp_input(&c,p,60,82,out,sizeof(out),&n)==NET_TCP_ACKED);unchanged(&c,83,NET_TCP_EMPTY);
    connect(&c,S-1,R-1);packet(p,24,R,S,5,3);memcpy(p+54,"ABC",3);fix_tcp(p,23);
    CHECK(net_tcp_input(&c,p,60,3,out,sizeof(out),&n)==NET_TCP_DATA_ACK && c.rx_used==3);
    CHECK(net_tcp_send(&c,data,8,4,out,sizeof(out),&n)==NET_TCP_TX_DATA && net_tcp_retransmit(&c,54,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED);
    net_tcp_abort(&c);cleared(&c);unchanged(&c,55,NET_ARP_EINVAL);
    connect(&c,S-1,R-1);CHECK(net_tcp_send(&c,data,8,3,out,sizeof(out),&n)==NET_TCP_TX_DATA);net_tcp_close(&c);
    for (size_t i=0;i<sizeof(c);i++) CHECK(((uint8_t *)&c)[i]==0);
    unchanged(&c,53,NET_ARP_EINVAL);
    printf("TCP retry host: %u checks PASS; context=%zu, partial/cumulative ACKs, finite timers, wrap, windows and destruction\n",checks,sizeof(c));return 0;
}
