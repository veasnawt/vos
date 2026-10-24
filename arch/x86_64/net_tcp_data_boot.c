#include "net_tcp_data_boot.h"
#include "net_tcp.h"
#include "virtio_net.h"
#include "thread.h"
#include "irq.h"
#include "pmm.h"
#include "screen.h"
#include "console.h"
#include "lib.h"
struct counts { unsigned data,bytes,corrections,invalid,controls,connected,synacks; };
static int fail(const char *where,int e)
{
    serial_puts("tcp-data: failed at ");serial_puts(where);serial_puts(" result ");serial_dec((uint64_t)(int64_t)e);serial_puts("\r\n");
    if (vnet_info()->state==VNET_READY) vnet_stop();
    return 0;
}
static int receive(uint8_t *p,uint32_t *n)
{
    int e;uint64_t until=irq_ticks()+200;
    do {e=vnet_receive(p,VNET_FRAME_MAX,n);if (e==VNET_EAGAIN) thread_sleep(1);} while (e==VNET_EAGAIN && irq_ticks()<until);
    return e;
}
static int one(struct net_tcp *c,uint8_t *rx,uint8_t *tx,struct counts *k)
{
    uint32_t n,before=c->rcv_next;size_t size;int e=receive(rx,&n);if (e) return -100;
    e=net_tcp_input(c,rx,n,irq_ticks(),tx,VNET_FRAME_MAX,&size);
    if (e==NET_TCP_SYNACK) k->synacks++;
    else if (e==NET_TCP_CONNECTED) k->connected++;
    else if (e==NET_TCP_DATA_ACK) {k->data++;k->bytes+=(uint32_t)(c->rcv_next-before);}
    else if (e==NET_TCP_ACK) k->corrections++;
    else if (e==NET_TCP_ACKED) k->controls++;
    else if (e<=0) k->invalid++;
    else return -101;
    if ((e==NET_TCP_SYNACK || e==NET_TCP_DATA_ACK || e==NET_TCP_ACK) && vnet_send(tx,(uint32_t)size)) return -102;
    return e;
}
static int advertise(struct net_tcp *c,uint8_t *tx)
{
    size_t n;int e=net_tcp_ack(c,irq_ticks(),tx,VNET_FRAME_MAX,&n);
    return e==NET_TCP_ACK?vnet_send(tx,(uint32_t)n):e;
}
static int transmit(struct net_tcp *c,const void *data,size_t n,uint8_t *tx)
{
    size_t size;int e=net_tcp_send(c,data,n,irq_ticks(),tx,VNET_FRAME_MAX,&size);
    return e==NET_TCP_TX_DATA?vnet_send(tx,(uint32_t)size):e;
}
int net_tcp_data_boot_run(void)
{
    const uint8_t local[]={10,0,2,15},peer[]={10,0,2,2};const uint32_t S=0x11223345,R=0x01020305;
    uint64_t before=pmm_free_count();screen_begin_input();
    serial_puts("VOS N5 - bounded TCP payload prototype\r\nOne kernel endpoint, RX=64 / TX=64 bytes; no application network grants\r\n");
    int e=vnet_init();if (e) return fail("adapter",e);
    struct net_tcp c;if (net_tcp_init(&c,vnet_info()->mac,local,4001,S-1) || net_tcp_enable_payload(&c)) return fail("endpoint",0);
    uint8_t rx[VNET_FRAME_MAX],tx[VNET_FRAME_MAX],read[64],data[64];size_t size;uint32_t n;struct counts k={0};
    if (net_arp_query(&c.ip.arp,peer,irq_ticks(),tx,sizeof(tx),&size)!=NET_ARP_REQUEST || vnet_send(tx,(uint32_t)size)) return fail("ARP query",0);
    if (receive(rx,&n) || net_tcp_input(&c,rx,n,irq_ticks(),tx,sizeof(tx),&size)!=NET_ARP_LEARNED) return fail("ARP resolution",0);
    if (one(&c,rx,tx,&k)!=NET_TCP_SYNACK || one(&c,rx,tx,&k)!=NET_TCP_CONNECTED || c.peer_window!=64) return fail("handshake",0);
    if (one(&c,rx,tx,&k)!=NET_TCP_DATA_ACK || c.rx_used!=64 || c.rcv_next!=R+64) return fail("full receive",0);
    for (unsigned i=0;i<64;i++) if (c.rx[i]!=(uint8_t)i) return fail("first payload",0);
    for (unsigned i=0;i<3;i++) if (one(&c,rx,tx,&k)!=NET_TCP_ACK || c.rx_used!=64 || c.rcv_next!=R+64) return fail("duplicate/overlap/window",0);
    if (net_tcp_receive(&c,read,16,&size,irq_ticks()) || size!=16 || c.rx_used!=48) return fail("partial read",0);
    for (unsigned i=0;i<16;i++) if (read[i]!=(uint8_t)i) return fail("partial bytes",0);
    if (advertise(&c,tx) || one(&c,rx,tx,&k)!=NET_TCP_DATA_ACK || c.rx_used!=64 || c.rcv_next!=R+80) return fail("window reopen",0);
    if (net_tcp_receive(&c,read,64,&size,irq_ticks()) || size!=64 || c.rx_used) return fail("drain",0);
    for (unsigned i=0;i<64;i++) if (read[i]!=(uint8_t)(16+i)) return fail("drained bytes",0);
    if (advertise(&c,tx)) return fail("window 64",0);
    if (one(&c,rx,tx,&k)!=NET_ARP_DROP || one(&c,rx,tx,&k)!=NET_TCP_DATA_ACK || c.rx_used!=9 || memcmp(c.rx,"peer-data",9) || c.peer_window!=4) return fail("future ACK/data",0);
    if (net_tcp_send(&c,"block",5,irq_ticks(),tx,sizeof(tx),&size)!=NET_TCP_WINDOW || c.tx_used || c.snd_next!=S) return fail("peer small window",0);
    if (transmit(&c,"VOS!",4,tx)) return fail("first TX",0);
    for (unsigned i=0;i<4;i++) {
        e=one(&c,rx,tx,&k);int expected=i==0?NET_ARP_DROP:(i==1?NET_ARP_EINVAL:NET_TCP_ACKED);
        if (e!=expected) return fail("partial ACK controls",e);
    }
    if (c.tx_used!=2 || c.snd_una!=S+2 || c.snd_next!=S+4 || memcmp(c.tx,"S!",2) || c.peer_window || k.invalid!=3) return fail("partial ACK",0);
    if (net_tcp_send(&c,"x",1,irq_ticks(),tx,sizeof(tx),&size)!=NET_ARP_BUSY) return fail("TX bound",0);
    serial_puts("tcp-data: partial ACK retained 2 bytes\r\n");
    if (one(&c,rx,tx,&k)!=NET_TCP_ACKED || one(&c,rx,tx,&k)!=NET_TCP_DATA_ACK || c.tx_used || c.snd_una!=S+4 || c.rx_used!=10 || c.rcv_next!=R+90) return fail("full ACK",0);
    if (net_tcp_send(&c,"x",1,irq_ticks(),tx,sizeof(tx),&size)!=NET_TCP_WINDOW) return fail("peer zero window",0);
    if (one(&c,rx,tx,&k)!=NET_TCP_ACKED || c.peer_window!=64) return fail("peer window update",0);
    for (unsigned i=0;i<64;i++) data[i]=(uint8_t)(128+i);
    if (transmit(&c,data,sizeof(data),tx)) return fail("bounded TX",0);
    for (unsigned i=0;i<3;i++) {e=one(&c,rx,tx,&k);if (e!=(i==2?NET_TCP_DATA_ACK:NET_TCP_ACK)) return fail("in-order ownership",e);}
    if (c.tx_used!=64 || c.snd_una!=S+4 || c.snd_next!=S+68 || c.rx_used!=64 || c.rcv_next!=R+144 || memcmp(c.tx,data,64) || memcmp(c.rx,"peer-dataZ",10)) return fail("pending buffers",0);
    for (unsigned i=0;i<54;i++) if (c.rx[10+i]!=(uint8_t)(200+i)) return fail("last payload",0);
    if (k.data!=5 || k.bytes!=144 || k.corrections!=5 || k.invalid!=3 || k.controls!=4 || k.connected!=1 || k.synacks!=1 || vnet_info()->transmitted!=16 || vnet_info()->received!=20) return fail("counts",0);
    serial_puts("TCP: 144 bytes accepted, 80 read; windows 64 -> 0 -> 16 -> 0 -> 64\r\nTCP: 68 bytes sent; partial/full ACK retired 4, retained 64 until timeout\r\nDuplicate/overlap/window corrections: 5; invalid packet/ACK drops: 3\r\n");
    while (irq_ticks()-c.tx_since<NET_TCP_TX_TTL) thread_sleep(1);
    if (net_tcp_send(&c,"x",1,irq_ticks(),tx,sizeof(tx),&size)!=NET_TCP_EXPIRED || net_tcp_expire(&c,irq_ticks())!=1 || c.state!=NET_TCP_LISTEN || c.rx_used || c.tx_used || c.snd_una || c.rcv_next || c.tx_since || c.peer_port) return fail("TX timeout",0);
    for (unsigned i=0;i<64;i++) if (c.rx[i] || c.tx[i]) return fail("timeout erase",0);
    serial_puts("TX timeout: explicit local abort erased 64 pending RX / 64 unacknowledged TX bytes\r\nTCP context bytes: ");serial_dec(sizeof(c));serial_puts("; private DMA: 44 KiB\r\n");
    if (vnet_stop() || vnet_info()->state!=VNET_STOPPED || vnet_info()->held || vnet_info()->quarantined || pmm_free_count()!=before || pmm_check_in(pmm_kernel())!=PMM_OK) return fail("DMA teardown",0);
    net_tcp_close(&c);for (size_t i=0;i<sizeof(c);i++) if (((uint8_t *)&c)[i]) return fail("endpoint erase",0);
    serial_puts("tcp-data: PASS RXbytes=144 TXbytes=68 TX=16 RX=20; timeout/reset erased buffers\r\nDMA frames released: 11. No retransmission, FIN/RST or production ISNs\r\n");return 1;
}
