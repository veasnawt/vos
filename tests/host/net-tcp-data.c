/* Reuse independent wire fixtures; run the accepted legacy suite separately. */
#define main tcp_legacy_tests_main
#include "net-tcp.c"
#undef main
static const uint32_t S=0x11223345,R=0x01020305;
static size_t payload(uint8_t *p,uint32_t seq,uint32_t ack,const void *data,size_t n,unsigned window)
{
    size_t wire=packet(p,24,seq,ack,5,n);if (n) memcpy(p+54,data,n);put(p+48,window);fix_tcp(p,20+n);return wire;
}
static void connect(struct net_tcp *c,uint32_t isn,uint32_t peer)
{
    uint8_t p[1514],out[128];size_t n;
    CHECK(net_tcp_init(c,G,GI,4001,isn)==0 && net_tcp_enable_payload(c)==0);
    packet(p,2,peer,0,5,0);CHECK(net_tcp_input(c,p,60,1,out,sizeof(out),&n)==NET_TCP_SYNACK && out[48]==0 && out[49]==64);
    packet(p,16,peer+1u,isn+1u,5,0);put(p+48,64);fix_tcp(p,20);
    CHECK(net_tcp_input(c,p,60,2,out,sizeof(out),&n)==NET_TCP_CONNECTED && c->peer_window==64 && c->snd_una==isn+1u);
}
static void control(uint8_t *p,uint32_t seq,uint32_t ack,unsigned window)
{
    packet(p,16,seq,ack,5,0);put(p+48,window);fix_tcp(p,20);
}
static void erased(const struct net_tcp *c)
{
    CHECK(c->state==NET_TCP_LISTEN && !c->rx_used && !c->tx_used && !c->snd_una && !c->snd_next && !c->rcv_next && !c->rcv_adv && !c->tx_since && !c->peer_port);
    for (unsigned i=0;i<64;i++) CHECK(!c->rx[i] && !c->tx[i]);
}
int main(void)
{
    struct net_tcp c;uint8_t p[1514],out[128],data[128],read[128];size_t n=777;
    for (unsigned i=0;i<sizeof(data);i++) data[i]=(uint8_t)i;
    CHECK(sizeof(c)<=512);CHECK(net_requested("vos.tcp-data=1") && !net_requested("xvos.tcp-data=1") && !net_requested("vos.tcp-data=10") && !net_requested("vos.vsh=1") && !net_requested("vos.rixie=1"));
    connect(&c,S-1,R-1);CHECK(net_tcp_enable_payload(&c)==NET_ARP_BUSY);
    CHECK(net_tcp_receive(&c,read,sizeof(read),&n,3)==NET_TCP_EMPTY && n==777);
    /* Every accepted segment fits both actual and last-advertised capacity. */
    for (unsigned bytes=1;bytes<=64;bytes++) {
        connect(&c,S-1,R-1);size_t wire=payload(p,R,S,data,bytes,64);memset(out,0xa5,sizeof(out));n=777;
        struct net_tcp before=c;
        CHECK(net_tcp_input(&c,p,wire,3,out,59,&n)==NET_ARP_NOSPACE && n==777 && !memcmp(&c,&before,sizeof(c)) && out[0]==0xa5);
        CHECK(net_tcp_input(&c,p,wire,3,out+1,60,&n)==NET_TCP_DATA_ACK && n==60 && out[0]==0xa5 && out[61]==0xa5);
        CHECK(c.rx_used==bytes && c.rcv_next==R+bytes && get32(out+43)==R+bytes && out[50]==64-bytes && !transport(GI,PI,6,out+35,20));
        CHECK(net_tcp_receive(&c,read,bytes,&n,4)==0 && n==bytes && !memcmp(read,data,bytes) && !c.rx_used && c.window_dirty);
        for (unsigned i=0;i<64;i++) CHECK(c.rx[i]==0);
        CHECK(net_tcp_ack(&c,5,out,sizeof(out),&n)==NET_TCP_ACK && out[49]==64 && c.rcv_adv==c.rcv_next+64);
    }
    connect(&c,S-1,R-1);size_t wire=payload(p,R,S,data,64,64);
    wire=payload(p,R,S,data,65,64);CHECK(net_tcp_input(&c,p,wire,3,out,sizeof(out),&n)==NET_TCP_ACK && !c.rx_used && c.rcv_next==R && out[49]==64);
    wire=payload(p,R,S,data,64,64);
    CHECK(net_tcp_input(&c,p,wire,3,p,sizeof(p),&n)==NET_TCP_DATA_ACK && c.rx_used==64);
    /* Fully duplicate, overlapping, future and full-buffer segments never ACK new bytes. */
    const uint32_t seqs[]={R,R+60,R+64,R+65,R-1};
    for (unsigned i=0;i<5;i++) {
        uint32_t next=c.rcv_next;uint8_t saved[64];memcpy(saved,c.rx,64);wire=payload(p,seqs[i],S,data,8,64);
        CHECK(net_tcp_input(&c,p,wire,4,out,sizeof(out),&n)==NET_TCP_ACK && get32(out+42)==next && out[49]==0 && c.rcv_next==next && c.rx_used==64 && !memcmp(c.rx,saved,64));
    }
    CHECK(net_tcp_receive(&c,read,0,&n,5)==NET_ARP_NOSPACE && c.rx_used==64);
    CHECK(net_tcp_receive(&c,read,16,&n,5)==0 && n==16 && !memcmp(read,data,16) && c.rx_used==48);
    wire=payload(p,R+64,S,data+64,16,64);
    /* Freed space is not remotely usable until advertised. First attempt updates the window only. */
    CHECK(net_tcp_input(&c,p,wire,6,out,sizeof(out),&n)==NET_TCP_ACK && c.rcv_next==R+64 && c.rx_used==48 && out[49]==16);
    CHECK(net_tcp_input(&c,p,wire,7,out,sizeof(out),&n)==NET_TCP_DATA_ACK && c.rx_used==64 && c.rcv_next==R+80 && out[49]==0);
    CHECK(net_tcp_receive(&c,read,64,&n,8)==0 && n==64 && !memcmp(read,data+16,64));
    CHECK(net_tcp_ack(&c,9,out,sizeof(out),&n)==NET_TCP_ACK && out[49]==64);
    /* TX owns a copy until acknowledged, including partial ACK and zero window. */
    CHECK(net_tcp_send(&c,data,65,10,out,sizeof(out),&n)==NET_ARP_EINVAL);
    struct net_tcp before=c;CHECK(net_tcp_send(&c,data,64,10,out,117,&n)==NET_ARP_NOSPACE && !memcmp(&c,&before,sizeof(c)));
    memcpy(out,data,64);CHECK(net_tcp_send(&c,out,64,10,out,sizeof(out),&n)==NET_TCP_TX_DATA && n==118 && c.tx_used==64 && c.snd_next==S+64 && !memcmp(c.tx,data,64) && !transport(GI,PI,6,out+34,84));
    CHECK(net_tcp_send(&c,data,1,11,out,sizeof(out),&n)==NET_ARP_BUSY);
    control(p,R+80,S+65,64);reject(&c,p,60,NET_ARP_DROP,11);
    control(p,R+80,S+0x80000000u,64);reject(&c,p,60,NET_ARP_DROP,11);
    control(p,R+80,S+16,0);CHECK(net_tcp_input(&c,p,60,11,out,sizeof(out),&n)==NET_TCP_ACKED && c.tx_used==48 && c.snd_una==S+16 && c.peer_window==0 && !memcmp(c.tx,data+16,48));
    for (unsigned i=48;i<64;i++) CHECK(c.tx[i]==0);
    uint64_t since=c.since;CHECK(net_tcp_input(&c,p,60,12,out,sizeof(out),&n)==NET_TCP_ACKED && c.since==since && c.tx_since==10);
    /* Stale ACK ignored: valid RX data still accepted, without retiring TX or changing peer window. */
    wire=payload(p,R+80,S,data,2,64);CHECK(net_tcp_input(&c,p,wire,13,out,sizeof(out),&n)==NET_TCP_DATA_ACK && c.rx_used==2 && c.tx_used==48 && c.peer_window==0);
    control(p,R+82,S+64,0);CHECK(net_tcp_input(&c,p,60,14,out,sizeof(out),&n)==NET_TCP_ACKED && !c.tx_used && c.snd_una==S+64 && !c.tx_since);
    CHECK(net_tcp_send(&c,data,1,15,out,sizeof(out),&n)==NET_TCP_WINDOW);
    control(p,R+82,S+64,4);CHECK(net_tcp_input(&c,p,60,16,out,sizeof(out),&n)==NET_TCP_ACKED);
    CHECK(net_tcp_send(&c,data,5,17,out,sizeof(out),&n)==NET_TCP_WINDOW);
    CHECK(net_tcp_send(&c,data,4,17,out,sizeof(out),&n)==NET_TCP_TX_DATA);
    /* Out-of-order data cannot acknowledge our pending bytes. */
    wire=payload(p,R+83,S+68,data,1,64);CHECK(net_tcp_input(&c,p,wire,18,out,sizeof(out),&n)==NET_TCP_ACK && c.tx_used==4 && c.snd_una==S+64 && c.rx_used==2 && get32(out+42)==R+82);
    /* Malformed and future ACK-bearing data leave RX and TX state untouched. */
    wire=payload(p,R+82,S+69,data,3,64);reject(&c,p,wire,NET_ARP_DROP,18);
    wire=payload(p,R+82,S+64,data,3,64);p[50]^=1;reject(&c,p,wire,NET_ARP_EINVAL,18);
    wire=payload(p,R+82,S+64,data,3,64);p[6]^=4;reject(&c,p,wire,NET_ARP_DROP,18);
    CHECK(net_tcp_expire(&c,216)==0);CHECK(net_tcp_send(&c,data,1,217,out,sizeof(out),&n)==NET_TCP_EXPIRED);
    CHECK(net_tcp_expire(&c,217)==1 && c.payload_enabled);erased(&c);
    connect(&c,S-1,R-1);CHECK(net_tcp_expire(&c,401)==0 && net_tcp_expire(&c,402)==1);erased(&c);
    connect(&c,S-1,R-1);wire=payload(p,R,S,data,7,64);CHECK(net_tcp_input(&c,p,wire,3,out,sizeof(out),&n)==NET_TCP_DATA_ACK);
    CHECK(net_tcp_send(&c,data,8,4,out,sizeof(out),&n)==NET_TCP_TX_DATA);net_tcp_abort(&c);erased(&c);
    /* Sequence wrap and stale/current/future ACK classification use bounded unsigned arithmetic. */
    connect(&c,0xfffffffdu,0xfffffffdu);wire=payload(p,0xfffffffeu,0xfffffffeu,data,4,64);
    CHECK(net_tcp_input(&c,p,wire,3,out,sizeof(out),&n)==NET_TCP_DATA_ACK && c.rcv_next==2);
    CHECK(net_tcp_send(&c,data,4,4,out,sizeof(out),&n)==NET_TCP_TX_DATA && c.snd_next==2);
    control(p,2,0,64);CHECK(net_tcp_input(&c,p,60,5,out,sizeof(out),&n)==NET_TCP_ACKED && c.tx_used==2);
    control(p,2,0xffffffffu,64);CHECK(net_tcp_input(&c,p,60,6,out,sizeof(out),&n)==NET_TCP_ACKED && c.tx_used==2);
    control(p,2,3,64);reject(&c,p,60,NET_ARP_DROP,7);
    control(p,2,2,64);CHECK(net_tcp_input(&c,p,60,8,out,sizeof(out),&n)==NET_TCP_ACKED && !c.tx_used);
    net_tcp_close(&c);for (size_t i=0;i<sizeof(c);i++) CHECK(((uint8_t *)&c)[i]==0);
    printf("TCP payload host: %u checks PASS; context=%zu, RX/TX=64, windows, ACK ownership, overlap, wrap and explicit abort\n",checks,sizeof(c));return 0;
}
