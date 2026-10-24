#include "net_tcp_retry_boot.h"
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
    serial_puts("tcp-retry: failed at ");serial_puts(where);serial_puts(" result ");serial_dec((uint64_t)(int64_t)e);serial_puts("\r\n");
    if (vnet_info()->state==VNET_READY) vnet_stop();
    return 0;
}
static int receive(uint8_t *p,uint32_t *n)
{
    int e;uint64_t until=irq_ticks()+200;
    do {e=vnet_receive(p,VNET_FRAME_MAX,n);if (e==VNET_EAGAIN) thread_sleep(1);} while (e==VNET_EAGAIN && irq_ticks()<until);
    return e;
}
static int input(struct net_tcp *c,uint8_t *rx,uint8_t *tx)
{
    uint32_t n;size_t length;int e=receive(rx,&n);if (e) return -100;
    e=net_tcp_input(c,rx,n,irq_ticks(),tx,VNET_FRAME_MAX,&length);
    if (e==NET_TCP_SYNACK && vnet_send(tx,(uint32_t)length)) return -101;
    return e;
}
static int transmit(struct net_tcp *c,const void *data,size_t n,uint8_t *tx)
{
    size_t length;int e=net_tcp_send(c,data,n,irq_ticks(),tx,VNET_FRAME_MAX,&length);
    return e==NET_TCP_TX_DATA?vnet_send(tx,(uint32_t)length):e;
}
static int retry(struct net_tcp *c,uint8_t *tx)
{
    int e;size_t length;
    do {e=net_tcp_retransmit(c,irq_ticks(),tx,VNET_FRAME_MAX,&length);if (e==NET_TCP_NOT_DUE) thread_sleep(1);} while (e==NET_TCP_NOT_DUE);
    if (e==NET_TCP_RETRANSMITTED && vnet_send(tx,(uint32_t)length)) return -102;
    return e;
}
int net_tcp_retry_boot_run(void)
{
    const uint8_t local[]={10,0,2,15},peer[]={10,0,2,2};const uint32_t S=0x11223345;
    uint64_t before=pmm_free_count();screen_begin_input();
    serial_puts("VOS N5 - bounded TCP retransmission prototype\r\nOne retained TX buffer; fixed RTO=50 ticks, retry limit=3, TX horizon=200 ticks\r\n");
    int e=vnet_init();if (e) return fail("adapter",e);
    struct net_tcp c;if (net_tcp_init(&c,vnet_info()->mac,local,4001,S-1) || net_tcp_enable_payload(&c) || net_tcp_enable_retransmission(&c)) return fail("endpoint",0);
    uint8_t rx[VNET_FRAME_MAX],tx[VNET_FRAME_MAX];uint32_t n;size_t length;
    if (net_arp_query(&c.ip.arp,peer,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_REQUEST || vnet_send(tx,(uint32_t)length)) return fail("ARP query",0);
    if (receive(rx,&n) || net_tcp_input(&c,rx,n,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_LEARNED) return fail("ARP resolution",0);
    if (input(&c,rx,tx)!=NET_TCP_SYNACK || input(&c,rx,tx)!=NET_TCP_CONNECTED) return fail("handshake",0);
    if (transmit(&c,"dataLOSS",8,tx) || retry(&c,tx)!=NET_TCP_RETRANSMITTED || c.retry_count!=1 || input(&c,rx,tx)!=NET_TCP_ACKED || c.tx_used || c.snd_una!=S+8) return fail("data loss",0);
    if (net_tcp_retransmit(&c,irq_ticks(),tx,sizeof(tx),&length)!=NET_TCP_EMPTY) return fail("ACKed no retry",0);
    serial_puts("Dropped data recovered: 8 bytes, one retry\r\n");
    if (transmit(&c,"ack-LOSS",8,tx) || retry(&c,tx)!=NET_TCP_RETRANSMITTED || c.retry_count!=1 || input(&c,rx,tx)!=NET_TCP_ACKED || c.tx_used || c.snd_una!=S+16) return fail("ACK loss",0);
    serial_puts("Dropped ACK recovered: duplicate segment, peer delivered bytes once\r\n");
    if (transmit(&c,"partial-data",12,tx) || input(&c,rx,tx)!=NET_TCP_ACKED || c.tx_used!=8 || c.snd_una!=S+20 || memcmp(c.tx,"ial-data",8)) return fail("partial ACK",0);
    uint32_t deadline=c.retry_after;uint64_t anchor=c.tx_since;
    if (input(&c,rx,tx)!=NET_TCP_ACKED || input(&c,rx,tx)!=NET_ARP_DROP || c.retry_after!=deadline || c.tx_since!=anchor || c.retry_count || c.tx_used!=8) return fail("duplicate/invalid ACK",0);
    if (retry(&c,tx)!=NET_TCP_RETRANSMITTED || c.retry_count!=1 || c.snd_next!=S+28 || input(&c,rx,tx)!=NET_TCP_ACKED || c.tx_used || c.snd_una!=S+28) return fail("partial tail retry",0);
    serial_puts("Partial/cumulative ACKs: only 8 remaining bytes retried; invalid ACK rejected\r\n");
    if (transmit(&c,"timeout-retained",16,tx)) return fail("exhaustion send",0);
    for (unsigned i=1;i<=NET_TCP_RETRY_MAX;i++) if (retry(&c,tx)!=NET_TCP_RETRANSMITTED || c.retry_count!=i || c.tx_used!=16 || c.snd_next!=S+44 || c.snd_una!=S+28) return fail("retry bound",0);
    if (retry(&c,tx)!=NET_TCP_EXPIRED || c.state!=NET_TCP_LISTEN || c.tx_used || c.rx_used || c.retry_count || c.retry_after || c.tx_since || c.snd_next || c.snd_una || c.peer_port) return fail("terminal timeout",0);
    for (unsigned i=0;i<64;i++) if (c.tx[i] || c.rx[i]) return fail("buffer erase",0);
    length=777;if (net_tcp_retransmit(&c,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_EINVAL || length!=777) return fail("post-abort retry",0);
    if (vnet_info()->transmitted!=12 || vnet_info()->received!=9) return fail("wire counts",0);
    serial_puts("Retry exhaustion: 3 retries, local abort, retained 16 bytes explicitly discarded\r\nTCP context bytes: ");serial_dec(sizeof(c));serial_puts("; no allocation or DMA pool growth\r\n");
    if (vnet_stop() || vnet_info()->state!=VNET_STOPPED || vnet_info()->held || vnet_info()->quarantined || pmm_free_count()!=before || pmm_check_in(pmm_kernel())!=PMM_OK) return fail("DMA teardown",0);
    net_tcp_close(&c);for (size_t i=0;i<sizeof(c);i++) if (((uint8_t *)&c)[i]) return fail("endpoint erase",0);
    if (net_tcp_retransmit(&c,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_EINVAL) return fail("post-close retry",0);
    serial_puts("tcp-retry: PASS delivered=28 retries=6 TX=12 RX=9; timeout/reset erased state\r\nDMA frames released: 11. Kernel-only; fixed test timer/ISNs, not production TCP\r\n");return 1;
}
