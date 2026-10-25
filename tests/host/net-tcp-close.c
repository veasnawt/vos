#define main legacy_tcp_tests_main
#include "net-tcp.c"
#undef main
static const uint32_t S=0x11223345,R=0x01020305;
static void connect_close(struct net_tcp *c,uint32_t s,uint32_t r)
{
    uint8_t p[1514],out[128];size_t n;
    CHECK(net_tcp_init(c,G,GI,4001,s-1)==0 && net_tcp_enable_payload(c)==0 &&
          net_tcp_enable_retransmission(c)==0 && net_tcp_enable_closure(c)==0);
    packet(p,2,r-1,0,5,0);CHECK(net_tcp_input(c,p,60,1,out,sizeof(out),&n)==NET_TCP_SYNACK);
    packet(p,16,r,s,5,0);put(p+48,64);fix_tcp(p,20);
    CHECK(net_tcp_input(c,p,60,2,out,sizeof(out),&n)==NET_TCP_CONNECTED);
}
static size_t control(uint8_t *p,unsigned flags,uint32_t seq,uint32_t ack,const char *data,size_t n)
{
    size_t size=packet(p,flags,seq,ack,5,n);put(p+48,64);
    if(n) memcpy(p+54,data,n);
    fix_tcp(p,20+n);return size;
}
static void erased(struct net_tcp *c)
{
    CHECK(c->state==NET_TCP_LISTEN && c->closure_enabled && c->payload_enabled && c->retry_enabled);
    CHECK(!c->tx_used && !c->rx_used && !c->peer_port && !c->since && !c->tx_since && !c->retry_count && !c->retry_after && !c->snd_next && !c->snd_una && !c->rcv_next);
    for(unsigned i=0;i<64;i++) CHECK(!c->rx[i] && !c->tx[i]);
}
static void valid_fin(const uint8_t *out,uint32_t seq,uint32_t ack,unsigned window)
{
    CHECK(out[47]==17 && get32(out+38)==seq && get32(out+42)==ack &&
          out[48]==window/256 && out[49]==window%256 && !reference(out+14,20) && !transport(GI,PI,6,out+34,20));
}
int main(void)
{
    struct net_tcp c;uint8_t p[1514],out[128],data[64];size_t n=777;
    CHECK(sizeof(c)==504 && sizeof(c)<=512);
    CHECK(net_tcp_enable_closure(NULL)==NET_ARP_EINVAL && net_tcp_finish(NULL,0,out,sizeof(out),&n)==NET_ARP_EINVAL);
    CHECK(net_tcp_init(&c,G,GI,4001,S-1)==0 && net_tcp_enable_closure(&c)==NET_ARP_EINVAL);
    connect_close(&c,S,R);CHECK(net_tcp_enable_closure(&c)==NET_ARP_BUSY);
    /* Active close, precise FIN accounting, duplicate FIN, protected TIME-WAIT. */
    struct net_tcp saved=c;memset(out,0xa5,sizeof(out));
    CHECK(net_tcp_finish(&c,3,out,59,&n)==NET_ARP_NOSPACE && n==777 && out[0]==0xa5 && !memcmp(&c,&saved,sizeof(c)));
    CHECK(net_tcp_finish(&c,3,out,sizeof(out),&n)==NET_TCP_FIN_SENT && n==60 && c.state==NET_TCP_FIN_WAIT_1 && c.snd_next==S+1 && c.snd_una==S);
    valid_fin(out,S,R,64);saved=c;
    CHECK(net_tcp_finish(&c,4,out,sizeof(out),&n)==NET_ARP_BUSY && !memcmp(&c,&saved,sizeof(c)));
    control(p,16,R,S+2,NULL,0);reject(&c,p,60,NET_ARP_DROP,4);
    control(p,16,R,S+1,NULL,0);CHECK(net_tcp_input(&c,p,60,4,out,sizeof(out),&n)==NET_TCP_ACKED && c.state==NET_TCP_FIN_WAIT_2 && c.snd_una==S+1);
    control(p,17,R,S+1,NULL,0);CHECK(net_tcp_input(&c,p,60,5,out,sizeof(out),&n)==NET_TCP_FIN_ACK && c.state==NET_TCP_TIME_WAIT && c.rcv_next==R+1 && get32(out+42)==R+1);
    CHECK(net_tcp_input(&c,p,60,6,out,sizeof(out),&n)==NET_TCP_ACK && c.rcv_next==R+1 && c.tx_since==6 && c.since==3);
    control(p,4,R+1,0,NULL,0);reject(&c,p,60,NET_ARP_DROP,7);
    CHECK(net_tcp_retransmit(&c,7,out,sizeof(out),&n)==NET_TCP_EMPTY);
    CHECK(net_tcp_expire(&c,6+NET_TCP_TIME_WAIT_TTL-1)==0);
    CHECK(net_tcp_expire(&c,6+NET_TCP_TIME_WAIT_TTL)==NET_TCP_DONE);erased(&c);
    n=777;CHECK(net_tcp_retransmit(&c,25000,out,sizeof(out),&n)==NET_ARP_EINVAL && n==777);
    /* FIN plus data preserves bytes until read, peer half-close keeps TX retry. */
    connect_close(&c,S,R);control(p,25,R,S,"abc",3);saved=c;n=777;
    CHECK(net_tcp_input(&c,p,60,3,out,59,&n)==NET_ARP_NOSPACE && n==777 && !memcmp(&c,&saved,sizeof(c)));
    CHECK(net_tcp_input(&c,p,60,3,out,sizeof(out),&n)==NET_TCP_FIN_ACK && c.state==NET_TCP_CLOSE_WAIT && c.rx_used==3 && c.rcv_next==R+4);
    CHECK(net_tcp_finish(&c,4,out,sizeof(out),&n)==NET_ARP_BUSY);
    CHECK(net_tcp_receive(&c,data,sizeof(data),&n,4)==0 && n==3 && !memcmp(data,"abc",3));
    CHECK(net_tcp_receive(&c,data,sizeof(data),&n,4)==NET_TCP_EOF);
    CHECK(net_tcp_send(&c,"ok",2,5,out,sizeof(out),&n)==NET_TCP_TX_DATA && c.since==3);
    CHECK(net_tcp_finish(&c,6,out,sizeof(out),&n)==NET_ARP_BUSY);
    CHECK(net_tcp_retransmit(&c,55,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && get32(out+38)==S && out[49]==0);
    control(p,16,R+4,S+1,NULL,0);CHECK(net_tcp_input(&c,p,60,56,out,sizeof(out),&n)==NET_TCP_ACKED && c.tx_used==1 && c.since==3);
    CHECK(net_tcp_retransmit(&c,106,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && out[54]=='k' && get32(out+38)==S+1);
    control(p,16,R+4,S+2,NULL,0);CHECK(net_tcp_input(&c,p,60,107,out,sizeof(out),&n)==NET_TCP_ACKED && !c.tx_used);
    CHECK(net_tcp_finish(&c,108,out,sizeof(out),&n)==NET_TCP_FIN_SENT && c.state==NET_TCP_LAST_ACK);valid_fin(out,S+2,R+4,0);
    control(p,17,R+3,S+2,NULL,0);CHECK(net_tcp_input(&c,p,60,109,out,sizeof(out),&n)==NET_TCP_ACK && c.rcv_next==R+4 && c.retry_after==50 && c.tx_since==108);
    CHECK(net_tcp_retransmit(&c,158,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED);valid_fin(out,S+2,R+4,0);
    control(p,16,R+4,S+3,NULL,0);CHECK(net_tcp_input(&c,p,60,159,out,sizeof(out),&n)==NET_TCP_DONE);erased(&c);
    /* Simultaneous FIN: ACK cannot retire unsent bytes or consume FIN twice. */
    connect_close(&c,S,R);CHECK(net_tcp_finish(&c,3,out,sizeof(out),&n)==NET_TCP_FIN_SENT);
    control(p,17,R,S,NULL,0);CHECK(net_tcp_input(&c,p,60,4,out,sizeof(out),&n)==NET_TCP_FIN_ACK && c.state==NET_TCP_CLOSING && c.snd_una==S);
    CHECK(net_tcp_retransmit(&c,53,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED);valid_fin(out,S,R+1,0);
    control(p,17,R,S+1,NULL,0);CHECK(net_tcp_input(&c,p,60,54,out,sizeof(out),&n)==NET_TCP_ACK && c.state==NET_TCP_TIME_WAIT && c.snd_una==S+1 && c.rcv_next==R+1);
    net_tcp_abort(&c);erased(&c);
    /* Exact-sequence, tuple-bound reset; in-window inaccurate reset challenged. */
    for(unsigned trial=0;trial<32;trial++) {
        connect_close(&c,S+trial,R+trial);uint32_t r=c.rcv_next,s=c.snd_next;
        CHECK(net_tcp_send(&c,"private",7,3,out,sizeof(out),&n)==NET_TCP_TX_DATA);
        control(p,4,r+1,0,NULL,0);CHECK(net_tcp_input(&c,p,60,4,out,sizeof(out),&n)==NET_TCP_ACK && c.tx_used==7 && c.state==NET_TCP_ESTABLISHED);
        control(p,4,r-1,0,NULL,0);reject(&c,p,60,NET_ARP_DROP,5);
        control(p,4,r,0,NULL,0);p[6]^=4;reject(&c,p,60,NET_ARP_DROP,5);p[6]^=4;
        put(p+34,5002);fix_tcp(p,20);reject(&c,p,60,NET_ARP_DROP,5);
        control(p,20,r,s,NULL,0);CHECK(net_tcp_input(&c,p,60,6,out,sizeof(out),&n)==NET_TCP_RESET);erased(&c);
        n=777;CHECK(net_tcp_retransmit(&c,60,out,sizeof(out),&n)==NET_ARP_EINVAL && n==777);
    }
    /* Invalid FIN gets a current ACK without advancing receive sequence. */
    connect_close(&c,S,R);control(p,17,R+1,S,NULL,0);
    CHECK(net_tcp_input(&c,p,60,3,out,sizeof(out),&n)==NET_TCP_ACK && c.rcv_next==R && c.state==NET_TCP_ESTABLISHED);
    control(p,17,R,S+1,NULL,0);reject(&c,p,60,NET_ARP_DROP,4);
    control(p,21,R,S,NULL,0);reject(&c,p,60,NET_ARP_DROP,4);
    control(p,4,R,0,"x",1);reject(&c,p,60,NET_ARP_DROP,4);
    control(p,17,R,S,NULL,0);p[50]^=1;reject(&c,p,60,NET_ARP_EINVAL,4);
    /* Finite FIN retries: duplicate ACKs do not change the original anchor. */
    connect_close(&c,S,R);CHECK(net_tcp_finish(&c,3,out,sizeof(out),&n)==NET_TCP_FIN_SENT);
    for(unsigned i=1;i<=3;i++) {
        saved=c;n=777;CHECK(net_tcp_retransmit(&c,3+i*50-1,out,sizeof(out),&n)==NET_TCP_NOT_DUE && n==777 && !memcmp(&c,&saved,sizeof(c)));
        CHECK(net_tcp_retransmit(&c,3+i*50,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED && c.retry_count==i && c.snd_next==S+1);
        control(p,16,R,S,NULL,0);CHECK(net_tcp_input(&c,p,60,4+i*50,out,sizeof(out),&n)==NET_TCP_ACKED && c.tx_since==3 && c.retry_count==i);
    }
    CHECK(net_tcp_expire(&c,202)==0);CHECK(net_tcp_retransmit(&c,203,out,sizeof(out),&n)==NET_TCP_EXPIRED);erased(&c);
    connect_close(&c,S,R);CHECK(net_tcp_finish(&c,3,out,sizeof(out),&n)==NET_TCP_FIN_SENT);
    control(p,16,R,S+1,NULL,0);CHECK(net_tcp_input(&c,p,60,4,out,sizeof(out),&n)==NET_TCP_ACKED);
    CHECK(net_tcp_expire(&c,402)==0 && net_tcp_expire(&c,403)==1);erased(&c);
    connect_close(&c,S,R);control(p,17,R,S,NULL,0);CHECK(net_tcp_input(&c,p,60,3,out,sizeof(out),&n)==NET_TCP_FIN_ACK);
    CHECK(net_tcp_expire(&c,402)==0 && net_tcp_expire(&c,403)==1);erased(&c);
    connect_close(&c,S,R);control(p,17,R,S,NULL,0);CHECK(net_tcp_input(&c,p,60,3,out,sizeof(out),&n)==NET_TCP_FIN_ACK);
    CHECK(net_tcp_finish(&c,4,out,sizeof(out),&n)==NET_TCP_FIN_SENT && net_tcp_expire(&c,203)==0 && net_tcp_expire(&c,204)==1);erased(&c);
    /* RX remains readable after local FIN. Linger completion never drops data. */
    connect_close(&c,S,R);CHECK(net_tcp_finish(&c,3,out,sizeof(out),&n)==NET_TCP_FIN_SENT);
    control(p,24,R,S+1,"rx",2);CHECK(net_tcp_input(&c,p,60,4,out,sizeof(out),&n)==NET_TCP_DATA_ACK && c.state==NET_TCP_FIN_WAIT_2);
    control(p,17,R+2,S+1,NULL,0);CHECK(net_tcp_input(&c,p,60,5,out,sizeof(out),&n)==NET_TCP_FIN_ACK);
    CHECK(net_tcp_expire(&c,5+NET_TCP_TIME_WAIT_TTL)==0 && c.rx_used==2);
    CHECK(net_tcp_receive(&c,data,sizeof(data),&n,6+NET_TCP_TIME_WAIT_TTL)==0 && n==2 && !memcmp(data,"rx",2));
    CHECK(net_tcp_expire(&c,6+NET_TCP_TIME_WAIT_TTL)==NET_TCP_DONE);erased(&c);
    /* A finite user deadline bounds duplicate-FIN-driven linger extension. */
    connect_close(&c,S,R);CHECK(net_tcp_finish(&c,3,out,sizeof(out),&n)==NET_TCP_FIN_SENT);
    control(p,17,R,S+1,NULL,0);CHECK(net_tcp_input(&c,p,60,4,out,sizeof(out),&n)==NET_TCP_FIN_ACK);
    for(unsigned t=10000;t<NET_TCP_CLOSE_HARD_TTL;t+=10000)
        CHECK(net_tcp_input(&c,p,60,t,out,sizeof(out),&n)==NET_TCP_ACK);
    CHECK(net_tcp_expire(&c,3+NET_TCP_CLOSE_HARD_TTL)==1);erased(&c);
    /* FIN at a full window edge cannot consume a nonexistent buffer slot. */
    for(unsigned bytes=0;bytes<=64;bytes++) {
        connect_close(&c,S,R);memset(data,0x5a,sizeof(data));
        control(p,25,R,S,(const char*)data,bytes);
        CHECK(net_tcp_input(&c,p,bytes+54<60?60:bytes+54,3,out,sizeof(out),&n)==(bytes==64?NET_TCP_DATA_ACK:NET_TCP_FIN_ACK));
        CHECK(c.rx_used==bytes && c.rcv_next==R+bytes+(bytes!=64) && get32(out+42)==c.rcv_next);
        if(bytes) CHECK(net_tcp_receive(&c,data,sizeof(data),&n,4)==0 && n==bytes);
        if(bytes==64) {
            CHECK(c.state==NET_TCP_ESTABLISHED && net_tcp_ack(&c,4,out,sizeof(out),&n)==NET_TCP_ACK);
            control(p,17,R+64,S,NULL,0);CHECK(net_tcp_input(&c,p,60,5,out,sizeof(out),&n)==NET_TCP_FIN_ACK);
        }
        CHECK(c.state==NET_TCP_CLOSE_WAIT && net_tcp_receive(&c,data,sizeof(data),&n,6)==NET_TCP_EOF);
    }
    connect_close(&c,S,R);control(p,16,R,S,NULL,0);put(p+48,0);fix_tcp(p,20);
    CHECK(net_tcp_input(&c,p,60,3,out,sizeof(out),&n)==NET_TCP_ACKED);
    saved=c;CHECK(net_tcp_finish(&c,4,out,sizeof(out),&n)==NET_TCP_WINDOW && !memcmp(&c,&saved,sizeof(c)));
    connect_close(&c,S,R);CHECK(net_tcp_finish(&c,3,out,sizeof(out),&n)==NET_TCP_FIN_SENT);
    control(p,16,R,S,NULL,0);put(p+48,0);fix_tcp(p,20);
    CHECK(net_tcp_input(&c,p,60,4,out,sizeof(out),&n)==NET_TCP_ACKED);
    saved=c;n=777;CHECK(net_tcp_retransmit(&c,53,out,sizeof(out),&n)==NET_TCP_WINDOW && n==777 && !memcmp(&c,&saved,sizeof(c)));
    CHECK(net_tcp_expire(&c,203)==1);erased(&c);
    CHECK(net_requested("vos.tcp-close=1") && !net_requested("vos.tcp-close=10") && !net_requested("xvos.tcp-close=1"));
    /* Sequence wrap for FIN and ACK and uint64 monotonic-clock wrap. */
    uint32_t starts[]={0,1,0xfffffffeu,0xffffffffu};
    for(unsigned i=0;i<4;i++) for(unsigned j=0;j<4;j++) {
        uint32_t s=starts[i],r=starts[j];connect_close(&c,s,r);c.since=UINT64_MAX-11;
        CHECK(net_tcp_finish(&c,UINT64_MAX-10,out,sizeof(out),&n)==NET_TCP_FIN_SENT && c.snd_next==s+1);
        CHECK(net_tcp_retransmit(&c,39,out,sizeof(out),&n)==NET_TCP_RETRANSMITTED);valid_fin(out,s,r,64);
        control(p,17,r,s+1,NULL,0);CHECK(net_tcp_input(&c,p,60,40,out,sizeof(out),&n)==NET_TCP_FIN_ACK && c.state==NET_TCP_TIME_WAIT && c.rcv_next==r+1);
        CHECK(net_tcp_expire(&c,40+NET_TCP_TIME_WAIT_TTL)==NET_TCP_DONE);erased(&c);
    }
    /* Local abort or adapter teardown may erase any closing state, no retry. */
    for(unsigned state=NET_TCP_CLOSE_WAIT;state<=NET_TCP_TIME_WAIT;state++) {
        connect_close(&c,S,R);c.state=state;c.rx_used=c.tx_used=4;memset(c.rx,0x5a,4);memset(c.tx,0xa5,4);
        net_tcp_abort(&c);erased(&c);CHECK(net_tcp_retransmit(&c,99,out,sizeof(out),&n)==NET_ARP_EINVAL);
        net_tcp_close(&c);for(size_t i=0;i<sizeof(c);i++) CHECK(((uint8_t*)&c)[i]==0);
        CHECK(net_tcp_retransmit(&c,99,out,sizeof(out),&n)==NET_ARP_EINVAL);
    }
    printf("TCP closure host: %u checks PASS; context=%zu, active/passive/simultaneous FIN, reset validation, retained data, timers, wrap and cleanup\n",checks,sizeof(c));return 0;
}
