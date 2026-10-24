#include "net_ipv4_boot.h"
#include "net_ipv4.h"
#include "virtio_net.h"
#include "thread.h"
#include "irq.h"
#include "pmm.h"
#include "screen.h"
#include "console.h"
#include "lib.h"

static int fail(const char *where,int error)
{
    serial_puts("ipv4: failed at ");serial_puts(where);serial_puts(" result ");serial_dec((uint64_t)(int64_t)error);serial_puts("\r\n");
    if (vnet_info()->state==VNET_READY) vnet_stop();
    return 0;
}
static int receive(uint8_t *p,uint32_t *length)
{
    int e;uint64_t until=irq_ticks()+200;
    do {e=vnet_receive(p,VNET_FRAME_MAX,length);if (e==VNET_EAGAIN) thread_sleep(1);} while (e==VNET_EAGAIN && irq_ticks()<until);
    return e;
}
int net_ipv4_boot_run(void)
{
    const uint8_t local[]={10,0,2,15},peer[]={10,0,2,2};
    const uint8_t data[]="VOS ping data";uint8_t large[NET_PING_DATA_MAX];
    for (unsigned i=0;i<sizeof(large);i++) large[i]=(uint8_t)i;
    uint64_t before=pmm_free_count();screen_begin_input();
    serial_puts("VOS N5 - native IPv4 / ICMP echo\r\nLocal IPv4: 10.0.2.15; isolated peer: 10.0.2.2\r\n");
    int e=vnet_init();if (e) return fail("adapter",e);
    struct net_ipv4 c;if (net_ipv4_init(&c,vnet_info()->mac,local)) return fail("context",0);
    uint8_t rx[VNET_FRAME_MAX],tx[VNET_FRAME_MAX];uint32_t received;size_t length;
    if (net_arp_query(&c.arp,peer,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_REQUEST || vnet_send(tx,(uint32_t)length)) return fail("ARP query",0);
    e=receive(rx,&received);if (e) return fail("ARP receive",e);
    if (net_ipv4_input(&c,rx,received,irq_ticks(),tx,sizeof(tx),&length)!=NET_ARP_LEARNED) return fail("ARP resolution",0);
    serial_puts("ARP neighbor resolved before originated ICMP traffic\r\n");
    if (net_ipv4_echo_request(&c,peer,0x564f,1,data,sizeof(data)-1,irq_ticks(),tx,sizeof(tx),&length)!=NET_IP_ECHO_REQUEST || vnet_send(tx,(uint32_t)length)) return fail("first echo",0);
    unsigned rejected=0;
    for (unsigned i=0;i<12 && c.pending;i++) {
        e=receive(rx,&received);if (e) return fail("first receive",e);
        e=net_ipv4_input(&c,rx,received,irq_ticks(),tx,sizeof(tx),&length);
        if (e==NET_ARP_REPLY) {if (vnet_send(tx,(uint32_t)length)) return fail("echo responder",0);}
        else if (e<=0) rejected++;
        else if (e!=NET_IP_ECHO_MATCH) return fail("first dispatch",e);
    }
    if (c.pending || c.matched!=1 || c.answered!=1 || rejected!=10) return fail("first correlation",0);
    serial_puts("ping 10.0.2.2: seq=1 reply verified (13 data bytes)\r\n");
    if (net_ipv4_echo_request(&c,peer,0x564f,2,large,sizeof(large),irq_ticks(),tx,sizeof(tx),&length)!=NET_IP_ECHO_REQUEST || vnet_send(tx,(uint32_t)length)) return fail("second echo",0);
    for (unsigned i=0;i<2 && c.pending;i++) {
        e=receive(rx,&received);if (e) return fail("second receive",e);
        e=net_ipv4_input(&c,rx,received,irq_ticks(),tx,sizeof(tx),&length);
        if (e<=0) rejected++;
        else if (e!=NET_IP_ECHO_MATCH) return fail("second dispatch",e);
    }
    if (c.pending || c.sent!=2 || c.matched!=2 || c.answered!=1 || rejected!=11 || vnet_info()->transmitted!=4 || vnet_info()->received!=15) return fail("counts/correlation",0);
    serial_puts("ping 10.0.2.2: seq=2 reply verified (64 data bytes)\r\nICMP responder: 1 valid echo reply transmitted\r\nMalformed/unsupported/unmatched packets rejected: 11\r\n");
    if (vnet_stop() || vnet_info()->held || pmm_free_count()!=before || pmm_check_in(pmm_kernel())!=PMM_OK) return fail("DMA teardown",0);
    serial_puts("ipv4: PASS ping=2/2 loss=0 TX=4 RX=15; DMA pool released\r\nNo application network grants. TCP / UDP remain next.\r\n");return 1;
}
