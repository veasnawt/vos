#include "net_udp_boot.h"
#include "net_udp.h"
#include "virtio_net.h"
#include "thread.h"
#include "irq.h"
#include "pmm.h"
#include "screen.h"
#include "console.h"
#include "lib.h"
static int fail(const char *where,int e)
{
    serial_puts("udp: failed at ");serial_puts(where);serial_puts(" result ");serial_dec((uint64_t)(int64_t)e);serial_puts("\r\n");
    if (vnet_info()->state==VNET_READY) vnet_stop();
    return 0;
}
static int receive(uint8_t *p,uint32_t *n)
{
    int e;uint64_t until=irq_ticks()+200;
    do {e=vnet_receive(p,VNET_FRAME_MAX,n);if (e==VNET_EAGAIN) thread_sleep(1);} while (e==VNET_EAGAIN && irq_ticks()<until);
    return e;
}
static int send(struct net_udp *c,const uint8_t peer[4],const void *data,size_t n,uint8_t *tx)
{
    size_t length;int e=net_udp_send(c,peer,5000,data,n,irq_ticks(),tx,VNET_FRAME_MAX,&length);
    return e==NET_UDP_SENT?vnet_send(tx,(uint32_t)length):e;
}
int net_udp_boot_run(void)
{
    const uint8_t local[]={10,0,2,15},peer[]={10,0,2,2},zero_result[]={0xc4,0xa1};
    uint64_t before=pmm_free_count();screen_begin_input();
    serial_puts("VOS N5 - bounded native UDP\r\nLocal endpoint: 10.0.2.15:4000; isolated peer: 10.0.2.2:5000\r\n");
    int e=vnet_init();if (e) return fail("adapter",e);
    struct net_udp c;if (net_udp_init(&c,vnet_info()->mac,local,4000)) return fail("endpoint",0);
    uint8_t rx[VNET_FRAME_MAX],tx[VNET_FRAME_MAX];uint32_t n;size_t length;
    if (net_arp_query(&c.ip.arp,peer,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_REQUEST || vnet_send(tx,(uint32_t)length)) return fail("ARP query",0);
    if (receive(rx,&n) || net_udp_input(&c,rx,n,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_LEARNED) return fail("ARP resolution",0);
    e=send(&c,peer,"VOS UDP hello",13,tx);if (e) return fail("hello send",e);
    unsigned rejected=0,accepted=0,full=0;
    for (unsigned i=0;i<7;i++) {
        if (receive(rx,&n)) return fail("hello receive",0);
        e=net_udp_input(&c,rx,n,irq_ticks(),tx,sizeof(tx),&length);
        if (e==NET_UDP_QUEUED) accepted++;else if (e<=0) rejected++;else return fail("hello dispatch",e);
    }
    struct net_udp_datagram d;
    if (accepted!=1 || rejected!=6 || net_udp_receive(&c,&d) || d.port!=5000 || memcmp(d.source,peer,4) || !d.checksum_present || d.length!=15 || memcmp(d.data,"peer UDP reply!",15)) return fail("hello payload",0);
    serial_puts("UDP: checksummed odd-length request/reply verified\r\n");
    e=send(&c,peer,"queue-ready",11,tx);if (e) return fail("queue-ready send",e);
    for (unsigned i=0;i<5;i++) {
        if (receive(rx,&n)) return fail("burst receive",0);
        e=net_udp_input(&c,rx,n,irq_ticks(),tx,sizeof(tx),&length);
        if (e==NET_UDP_QUEUED) accepted++;else if (e==NET_UDP_FULL) full++;else return fail("burst dispatch",e);
    }
    if (c.count!=4 || full!=1 || net_udp_receive(&c,NULL)!=NET_ARP_EINVAL || c.count!=4) return fail("queue bound",0);
    for (unsigned i=0;i<4;i++) {
        if (net_udp_receive(&c,&d) || memcmp(d.source,peer,4)) return fail("queue drain",0);
        if (i==0 && (d.port!=5000 || !d.checksum_present || d.length!=1 || d.data[0]!='A')) return fail("FIFO first",0);
        if (i==1 && (d.port!=5000 || d.checksum_present || d.length!=13 || memcmp(d.data,"zero-checksum",13))) return fail("zero checksum",0);
        if (i==2 && (d.port!=0 || !d.checksum_present || d.length!=0)) return fail("optional source port",0);
        if (i==3) {
            if (d.port!=5000 || !d.checksum_present || d.length!=512) return fail("maximum datagram",0);
            for (unsigned j=0;j<512;j++) if (d.data[j]!=(uint8_t)j) return fail("maximum payload",0);
        }
    }
    if (net_udp_receive(&c,&d)!=NET_UDP_EMPTY) return fail("empty queue",0);
    serial_puts("UDP queue: 4 slots, overflow rejected, FIFO preserved; 512-byte payload verified\r\nUDP: zero checksum and optional source port accepted\r\n");
    e=send(&c,peer,zero_result,sizeof(zero_result),tx);if (e) return fail("zero-result send",e);
    if (tx[40]!=255 || tx[41]!=255) return fail("zero-result checksum",0);
    for (unsigned i=0;i<8;i++) {
        if (receive(rx,&n)) return fail("recovery receive",0);
        e=net_udp_input(&c,rx,n,irq_ticks(),tx,sizeof(tx),&length);
        if (e==NET_UDP_QUEUED) accepted++;else if (e<=0) rejected++;else return fail("recovery dispatch",e);
    }
    if (net_udp_receive(&c,&d) || d.length!=10 || memcmp(d.data,"after-full",10) || !d.checksum_present) return fail("queue recovery",0);
    if (net_udp_receive(&c,&d) || d.length!=2 || memcmp(d.data,zero_result,2) || !d.checksum_present) return fail("ffff receive",0);
    if (net_udp_receive(&c,&d)!=NET_UDP_EMPTY || accepted!=7 || rejected!=12 || full!=1) return fail("final counts",0);
    e=send(&c,peer,"UDP complete",12,tx);if (e) return fail("final send",e);
    if (vnet_info()->transmitted!=5 || vnet_info()->received!=21) return fail("adapter counts",0);
    if (vnet_stop() || vnet_info()->state!=VNET_STOPPED || vnet_info()->held || vnet_info()->quarantined || pmm_free_count()!=before || pmm_check_in(pmm_kernel())!=PMM_OK) return fail("DMA teardown",0);
    net_udp_close(&c);for (size_t i=0;i<sizeof(c);i++) if (((uint8_t *)&c)[i]) return fail("endpoint erase",0);
    serial_puts("UDP: computed-zero checksum transmitted as ffff; queue usable after overflow\r\nudp: PASS TX=5 RX=21 accepted=7 rejected=12 full=1; DMA pool released\r\nEndpoint closed and erased. No application network grants. TCP remains next.\r\n");return 1;
}
