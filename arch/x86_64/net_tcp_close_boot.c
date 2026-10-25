#include "net_tcp_close_boot.h"
#include "net_tcp.h"
#include "virtio_net.h"
#include "thread.h"
#include "irq.h"
#include "pmm.h"
#include "screen.h"
#include "console.h"
#include "lib.h"
static int fail(const char *where,int e)
{
    serial_puts("tcp-close: failed at ");serial_puts(where);serial_puts(" result ");serial_dec((uint64_t)(int64_t)e);serial_puts("\r\n");
    if(vnet_info()->state==VNET_READY) vnet_stop();
    return 0;
}
static int receive(uint8_t *p,uint32_t *n)
{
    int e;uint64_t start=irq_ticks();
    do {e=vnet_receive(p,VNET_FRAME_MAX,n);if(e==VNET_EAGAIN) thread_sleep(1);} while(e==VNET_EAGAIN && irq_ticks()-start<200);
    return e;
}
static int input(struct net_tcp *c,uint8_t *rx,uint8_t *tx)
{
    uint32_t n;size_t length=0;int e=receive(rx,&n);if(e) return -100;
    e=net_tcp_input(c,rx,n,irq_ticks(),tx,VNET_FRAME_MAX,&length);
    if((e==NET_TCP_SYNACK || e==NET_TCP_ACK || e==NET_TCP_FIN_ACK || e==NET_TCP_DATA_ACK) &&
       vnet_send(tx,(uint32_t)length)) return -101;
    return e;
}
static int finish(struct net_tcp *c,uint8_t *tx)
{
    size_t n;int e=net_tcp_finish(c,irq_ticks(),tx,VNET_FRAME_MAX,&n);
    return e==NET_TCP_FIN_SENT?vnet_send(tx,(uint32_t)n):e;
}
static int send(struct net_tcp *c,const char *p,size_t n,uint8_t *tx)
{
    size_t length;int e=net_tcp_send(c,p,n,irq_ticks(),tx,VNET_FRAME_MAX,&length);
    return e==NET_TCP_TX_DATA?vnet_send(tx,(uint32_t)length):e;
}
static int retry(struct net_tcp *c,uint8_t *tx)
{
    int e;size_t n;
    do {e=net_tcp_retransmit(c,irq_ticks(),tx,VNET_FRAME_MAX,&n);if(e==NET_TCP_NOT_DUE) thread_sleep(1);} while(e==NET_TCP_NOT_DUE);
    if(e==NET_TCP_RETRANSMITTED && vnet_send(tx,(uint32_t)n)) return -102;
    return e;
}
static int clear(const struct net_tcp *c)
{
    if(c->state!=NET_TCP_LISTEN || c->rx_used || c->tx_used || c->peer_port || c->since || c->tx_since ||
       c->snd_una || c->snd_next || c->rcv_next || c->retry_count || c->retry_after) return 0;
    for(unsigned i=0;i<64;i++) if(c->rx[i] || c->tx[i]) return 0;
    return 1;
}
int net_tcp_close_boot_run(void)
{
    const uint8_t local[]={10,0,2,15},peer[]={10,0,2,2};const uint32_t S=0x11223345,R=0x01020305;
    uint64_t before=pmm_free_count();screen_begin_input();
    serial_puts("VOS N5 - bounded TCP connection closure prototype\r\nOne endpoint; RX/TX 64/64 bytes; FIN retry limit=3, RTO=50 ticks\r\n");
    int e=vnet_init();if(e) return fail("adapter",e);
    struct net_tcp c;if(net_tcp_init(&c,vnet_info()->mac,local,4001,S-1) || net_tcp_enable_payload(&c) ||
        net_tcp_enable_retransmission(&c) || net_tcp_enable_closure(&c)) return fail("endpoint",0);
    uint8_t rx[VNET_FRAME_MAX],tx[VNET_FRAME_MAX];uint32_t n;size_t length;
    if(net_arp_query(&c.ip.arp,peer,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_REQUEST || vnet_send(tx,(uint32_t)length)) return fail("ARP query",0);
    if(receive(rx,&n) || net_tcp_input(&c,rx,n,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_LEARNED) return fail("ARP resolution",0);
    if(input(&c,rx,tx)!=NET_TCP_SYNACK || input(&c,rx,tx)!=NET_TCP_CONNECTED) return fail("passive handshake",0);
    if(send(&c,"retain",6,tx) || input(&c,rx,tx)!=NET_TCP_FIN_ACK || c.state!=NET_TCP_CLOSE_WAIT ||
       c.snd_una!=S+2 || c.tx_used!=4 || c.rcv_next!=R+1) return fail("peer half close",0);
    if(net_tcp_finish(&c,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_BUSY ||
       retry(&c,tx)!=NET_TCP_RETRANSMITTED || input(&c,rx,tx)!=NET_TCP_ACKED || c.tx_used) return fail("retained tail retry",0);
    if(finish(&c,tx) || c.state!=NET_TCP_LAST_ACK || retry(&c,tx)!=NET_TCP_RETRANSMITTED ||
       input(&c,rx,tx)!=NET_TCP_ACK || input(&c,rx,tx)!=NET_TCP_DONE || !clear(&c)) return fail("passive FIN retry/completion",0);
    if(net_tcp_retransmit(&c,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_EINVAL) return fail("post completion",0);
    serial_puts("Peer FIN: retained tail recovered, lost FIN ACK retried, LAST-ACK complete\r\n");
    if(input(&c,rx,tx)!=NET_TCP_SYNACK || input(&c,rx,tx)!=NET_TCP_CONNECTED || finish(&c,tx)) return fail("active handshake/FIN",0);
    if(input(&c,rx,tx)!=NET_ARP_DROP || input(&c,rx,tx)!=NET_TCP_ACK || input(&c,rx,tx)!=NET_TCP_ACK ||
       input(&c,rx,tx)!=NET_ARP_DROP) return fail("invalid ACK/FIN/reset",0);
    if(input(&c,rx,tx)!=NET_TCP_FIN_ACK || c.state!=NET_TCP_CLOSING ||
       input(&c,rx,tx)!=NET_TCP_ACK || c.state!=NET_TCP_TIME_WAIT ||
       input(&c,rx,tx)!=NET_ARP_DROP || input(&c,rx,tx)!=NET_TCP_ACK || input(&c,rx,tx)!=NET_ARP_DROP ||
       net_tcp_expire(&c,irq_ticks())!=0 || c.rcv_next!=R+0x1000u+1u ||
       net_tcp_retransmit(&c,irq_ticks(),tx,sizeof(tx),&length)!=NET_TCP_EMPTY) return fail("simultaneous/TIME-WAIT",0);
    serial_puts("Active/simultaneous FIN: CLOSING -> TIME-WAIT; duplicates ACKed once\r\nInvalid controls rejected; reset cannot destroy TIME-WAIT; SYN reuse blocked\r\n");
    net_tcp_abort(&c);if(!clear(&c)) return fail("explicit linger abort",0);
    serial_puts("TIME-WAIT not shortened: live check then explicit local abort (2xMSL host-tested)\r\n");
    if(input(&c,rx,tx)!=NET_TCP_SYNACK || input(&c,rx,tx)!=NET_TCP_CONNECTED ||
       input(&c,rx,tx)!=NET_TCP_DATA_ACK || c.rx_used!=2 || send(&c,"tx",2,tx) ||
       input(&c,rx,tx)!=NET_ARP_DROP || input(&c,rx,tx)!=NET_ARP_DROP || input(&c,rx,tx)!=NET_TCP_RESET || !clear(&c)) return fail("bound-peer reset/erase",0);
    serial_puts("Exact-sequence peer reset: RX=2 and TX=2 explicitly discarded and erased\r\n");
    if(input(&c,rx,tx)!=NET_TCP_SYNACK || input(&c,rx,tx)!=NET_TCP_CONNECTED || finish(&c,tx)) return fail("timeout setup",0);
    for(unsigned i=1;i<=3;i++) if(retry(&c,tx)!=NET_TCP_RETRANSMITTED || c.retry_count!=i) return fail("FIN retry limit",0);
    if(retry(&c,tx)!=NET_TCP_EXPIRED || !clear(&c) ||
       net_tcp_retransmit(&c,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_EINVAL) return fail("FIN exhaustion/erase",0);
    serial_puts("Unanswered FIN: 3 retries then terminal timeout; no packet after destruction\r\nTCP context bytes: ");serial_dec(sizeof(c));serial_puts("; DMA pool unchanged: 44 KiB\r\n");
    if(vnet_info()->transmitted!=23 || vnet_info()->received!=26) return fail("wire counts",0);
    if(vnet_stop() || vnet_info()->state!=VNET_STOPPED || vnet_info()->held || vnet_info()->quarantined ||
       pmm_free_count()!=before || pmm_check_in(pmm_kernel())!=PMM_OK) return fail("DMA teardown",0);
    net_tcp_close(&c);for(size_t i=0;i<sizeof(c);i++) if(((uint8_t*)&c)[i]) return fail("endpoint erase",0);
    if(net_tcp_retransmit(&c,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_EINVAL) return fail("post destruction",0);
    serial_puts("tcp-close: PASS TX=23 RX=26; FIN/reset/timeout cleanup; DMA frames released=11\r\nKernel-only; bounded diagnostic TCP, not production-ready\r\n");return 1;
}
