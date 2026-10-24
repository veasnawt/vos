#include "net_tcp_boot.h"
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
    serial_puts("tcp: failed at ");serial_puts(where);serial_puts(" result ");serial_dec((uint64_t)(int64_t)e);serial_puts("\r\n");
    if (vnet_info()->state==VNET_READY) vnet_stop();
    return 0;
}
static int receive(uint8_t *p,uint32_t *n)
{
    int e;uint64_t until=irq_ticks()+200;
    do {e=vnet_receive(p,VNET_FRAME_MAX,n);if (e==VNET_EAGAIN) thread_sleep(1);} while (e==VNET_EAGAIN && irq_ticks()<until);
    return e;
}
static int batch(struct net_tcp *c,unsigned count,uint8_t *rx,uint8_t *tx,unsigned *rejected,unsigned *synacks,unsigned *connected)
{
    for (unsigned i=0;i<count;i++) {
        uint32_t n;size_t size;int e=receive(rx,&n);if (e) return e;
        e=net_tcp_input(c,rx,n,irq_ticks(),tx,VNET_FRAME_MAX,&size);
        if (e==NET_TCP_SYNACK) {if (vnet_send(tx,(uint32_t)size)) return -100;(*synacks)++;}
        else if (e==NET_TCP_CONNECTED) (*connected)++;
        else if (e<=0) (*rejected)++;
        else return -101;
    }
    return 0;
}
int net_tcp_boot_run(void)
{
    const uint8_t local[]={10,0,2,15},peer[]={10,0,2,2};
    uint64_t before=pmm_free_count();screen_begin_input();
    serial_puts("VOS N5 - bounded TCP establishment prototype\r\nLocal endpoint: 10.0.2.15:4001; isolated peer: 10.0.2.2:5001\r\n");
    int e=vnet_init();if (e) return fail("adapter",e);
    struct net_tcp c;if (net_tcp_init(&c,vnet_info()->mac,local,4001,0x11223344)) return fail("endpoint",0);
    uint8_t rx[VNET_FRAME_MAX],tx[VNET_FRAME_MAX];uint32_t n;size_t size;
    if (net_arp_query(&c.ip.arp,peer,irq_ticks(),tx,sizeof(tx),&size)!=NET_ARP_REQUEST || vnet_send(tx,(uint32_t)size)) return fail("ARP query",0);
    if (receive(rx,&n) || net_tcp_input(&c,rx,n,irq_ticks(),tx,sizeof(tx),&size)!=NET_ARP_LEARNED) return fail("ARP resolution",0);
    unsigned rejected=0,synacks=0,connected=0;
    e=batch(&c,7,rx,tx,&rejected,&synacks,&connected);if (e) return fail("first SYN batch",e);
    if (c.state!=NET_TCP_SYN_RECEIVED || c.peer_isn!=0xffffffffu || c.rcv_next!=0 || synacks!=1 || rejected!=6) return fail("first SYN state",0);
    e=batch(&c,2,rx,tx,&rejected,&synacks,&connected);if (e) return fail("bad ACK batch",e);
    if (c.state!=NET_TCP_SYN_RECEIVED || rejected!=8 || connected) return fail("bad ACK transition",0);
    while (irq_ticks()-c.since<NET_TCP_HANDSHAKE_TTL) thread_sleep(1);
    if (net_tcp_expire(&c,irq_ticks())!=1 || c.state!=NET_TCP_LISTEN || c.peer_port || c.snd_next || c.rcv_next || c.isn || c.peer_isn || c.since) return fail("half-open timeout",0);
    for (unsigned i=0;i<sizeof(c.peer);i++) if (c.peer[i]) return fail("peer erase",0);
    for (unsigned i=0;i<sizeof(c.peer_mac);i++) if (c.peer_mac[i]) return fail("MAC erase",0);
    serial_puts("tcp: half-open timeout cleared; listening\r\n");
    e=batch(&c,7,rx,tx,&rejected,&synacks,&connected);if (e) return fail("second SYN batch",e);
    if (c.state!=NET_TCP_SYN_RECEIVED || c.isn!=0x11233344 || c.rcv_next!=0x01020305 || synacks!=2 || rejected!=14) return fail("second SYN state",0);
    e=batch(&c,7,rx,tx,&rejected,&synacks,&connected);if (e) return fail("final ACK batch",e);
    if (c.state!=NET_TCP_ESTABLISHED || connected!=1 || synacks!=2 || rejected!=20 || c.snd_next!=0x11233345 || c.peer_port!=5001 || memcmp(c.peer,peer,4)) return fail("establishment",0);
    serial_puts("TCP: SYN -> SYN-ACK -> ACK verified; ESTABLISHED\r\nTCP: exact peer/sequence/acknowledgment checks; 20 rejected inputs\r\nOne connection record; half-open expiry erased state before retry\r\n");
    if (vnet_info()->transmitted!=3 || vnet_info()->received!=24) return fail("adapter counts",0);
    if (vnet_stop() || vnet_info()->state!=VNET_STOPPED || vnet_info()->held || vnet_info()->quarantined || pmm_free_count()!=before || pmm_check_in(pmm_kernel())!=PMM_OK) return fail("DMA teardown",0);
    net_tcp_close(&c);for (size_t i=0;i<sizeof(c);i++) if (((uint8_t *)&c)[i]) return fail("endpoint erase",0);
    serial_puts("tcp: PASS handshake=1 timeout=1 rejected=20 TX=3 RX=24; DMA pool released\r\nEndpoint locally closed/erased; no FIN or RST exchange\r\nNo streaming, retransmission, TCP options or app network grants\r\nDeterministic test ISNs only; not production TCP\r\n");return 1;
}
