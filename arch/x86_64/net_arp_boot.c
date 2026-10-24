#include "net_arp_boot.h"
#include "net_arp.h"
#include "virtio_net.h"
#include "thread.h"
#include "irq.h"
#include "pmm.h"
#include "screen.h"
#include "console.h"
#include "lib.h"

static void text(const char *s) {serial_puts(s);}
static int fail(const char *where,int error)
{
    text("arp: failed at ");text(where);text(" result ");serial_dec((uint64_t)(int64_t)error);text("\r\n");
    if (vnet_info()->state==VNET_READY) vnet_stop();
    return 0;
}
static int receive(uint8_t *p,uint32_t *n)
{
    int e;uint64_t until=irq_ticks()+200;
    do {e=vnet_receive(p,VNET_FRAME_MAX,n);if (e==VNET_EAGAIN) thread_sleep(1);} while (e==VNET_EAGAIN && irq_ticks()<until);
    return e;
}
int net_arp_boot_run(void)
{
    const uint8_t ip[]={10,0,2,15},peer[]={10,0,2,2},second[]={10,0,2,3},unknown[]={10,0,2,9},zero[4]={0};
    const uint8_t peer_mac[]={0x52,0x54,0,0xaa,0xbb,0xcc},second_mac[]={0x52,0x54,0,0xaa,0xbb,0xdd};
    uint64_t before=pmm_free_count();screen_begin_input();
    text("VOS N5 - Ethernet / ARP\r\nLocal protocol address: 10.0.2.15 (ARP only)\r\n");
    int e=vnet_init();if (e) return fail("adapter init",e);
    struct net_arp a;if (net_arp_init(&a,vnet_info()->mac,ip)) return fail("ARP init",0);
    uint8_t rx[VNET_FRAME_MAX],tx[NET_ARP_WIRE],mac[6];uint32_t received=0;size_t length=0;
    if (net_arp_query(&a,peer,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_REQUEST || vnet_send(tx,(uint32_t)length)) return fail("query",0);
    unsigned dropped=0,learned=0;
    for (unsigned i=0;i<8;i++) {
        e=receive(rx,&received);if (e) return fail("resolution RX",e);
        e=net_ethernet_input(&a,rx,received,irq_ticks(),tx,sizeof(tx),&length);
        if (e==NET_ARP_LEARNED) learned++;
        else if (e<=NET_ARP_DROP) dropped++;
        else return fail("unexpected response",e);
    }
    if (learned!=1 || dropped!=7 || !net_neighbor_get(&a,peer,irq_ticks(),mac) || memcmp(mac,peer_mac,6) ||
        net_neighbor_get(&a,unknown,irq_ticks(),mac)) return fail("resolved neighbor",0);
    if (net_arp_query(&a,peer,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_CACHED) return fail("cache hit",0);
    text("arp: resolved 10.0.2.2 -> 52:54:00:aa:bb:cc\r\nCache hit: no repeated request\r\n");
    if (net_arp_query(&a,second,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_REQUEST || vnet_send(tx,(uint32_t)length)) return fail("second query",0);
    for (unsigned i=0;i<2;i++) {
        e=receive(rx,&received);if (e) return fail("request RX",e);
        e=net_ethernet_input(&a,rx,received,irq_ticks(),tx,sizeof(tx),&length);
        if (e!=NET_ARP_REPLY || length!=NET_ARP_WIRE || vnet_send(tx,(uint32_t)length)) return fail("reply TX",e);
    }
    e=receive(rx,&received);if (e) return fail("unsupported RX",e);
    if (net_ethernet_input(&a,rx,received,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_EINVAL) return fail("EtherType gap",0);
    if (!net_neighbor_get(&a,second,irq_ticks(),mac) || memcmp(mac,second_mac,6) || net_neighbor_get(&a,zero,irq_ticks(),mac) || a.pending) return fail("cache state",0);
    if (vnet_info()->transmitted!=4 || vnet_info()->received!=11) return fail("frame counts",0);
    if (vnet_stop() || vnet_info()->held || pmm_free_count()!=before || pmm_check_in(pmm_kernel())!=PMM_OK) return fail("DMA teardown",0);
    text("ARP request/reply and zero-address probe response: PASS\r\nRejected malformed, unsupported and unrelated input: 8 frames\r\nNeighbor cache: 4 bounded slots, timed expiry\r\n");
    text("arp: PASS TX=4 RX=11; DMA pool released\r\nNo userspace network grants. IPv4 / ICMP echo are next.\r\n");return 1;
}
