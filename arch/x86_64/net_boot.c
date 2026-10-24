#include "net_boot.h"
#include "net_arp_boot.h"
#include "net_ipv4_boot.h"
#include "net_udp_boot.h"
#include "net_tcp_boot.h"
#include "net_tcp_data_boot.h"
#include "net_tcp_retry_boot.h"
#include "virtio_net.h"
#include "console.h"
#include "screen.h"
#include "thread.h"
#include "irq.h"
#include "pmm.h"
#include "lib.h"

static void display(const char *p) {while (*p) screen_putc(*p++);}
static int failed(const char *stage,int error)
{
    serial_puts("net: failed at ");serial_puts(stage);serial_puts(" error ");serial_dec(error);
    serial_puts(" TX=");serial_dec(vnet_info()->transmitted);serial_puts(" RX=");serial_dec(vnet_info()->received);
    serial_puts(" reason ");serial_puts(vnet_info()->why?vnet_info()->why:"none");
    serial_puts(" header ");for (unsigned i=0;i<VNET_HEADER;i++) {serial_hex(vnet_info()->last_rx_header[i]);serial_putc(' ');}
    serial_puts("\r\n");vnet_stop();return 0;
}

static int option(const char *p,const char *token)
{
    if (!p) return 0;
    while (*p) {
        while (*p==' ' || *p=='\t') p++;
        unsigned n=0;while (token[n] && p[n]==token[n]) n++;
        if (!token[n] && (!p[n] || p[n]==' ' || p[n]=='\t')) return 1;
        while (*p && *p!=' ' && *p!='\t') p++;
    }
    return 0;
}
static void make_frame(uint8_t *frame,unsigned sequence,int reply)
{
    const uint8_t peer[]={0x52,0x54,0,0xaa,0xbb,0xcc};
    memcpy(frame,reply?vnet_info()->mac:peer,6);
    memcpy(frame+6,reply?peer:vnet_info()->mac,6);
    frame[12]=0x88;frame[13]=0xb5;
    memcpy(frame+14,reply?"VOS-N5-RX":"VOS-N5-TX",9);frame[23]=(uint8_t)sequence;
    for (unsigned i=24;i<60;i++) frame[i]=(uint8_t)(sequence+i);
}
int net_requested(const char *p) {return option(p,"vos.net=1") || option(p,"vos.arp=1") || option(p,"vos.ipv4=1") || option(p,"vos.udp=1") || option(p,"vos.tcp=1") || option(p,"vos.tcp-data=1") || option(p,"vos.tcp-retry=1");}
int net_boot_run(const char *cmdline)
{
    if (option(cmdline,"vos.tcp-retry=1")) return net_tcp_retry_boot_run();
    if (option(cmdline,"vos.tcp-data=1")) return net_tcp_data_boot_run();
    if (option(cmdline,"vos.tcp=1")) return net_tcp_boot_run();
    if (option(cmdline,"vos.udp=1")) return net_udp_boot_run();
    if (option(cmdline,"vos.ipv4=1")) return net_ipv4_boot_run();
    if (option(cmdline,"vos.arp=1")) return net_arp_boot_run();
    uint64_t before=pmm_free_count();
    screen_begin_input();display("VOS N5 - bounded virtio-net adapter\n");
    int e=vnet_init();
    if (e==VNET_ENODEV) {
        serial_puts("net: absent; no DMA allocation\r\n");
        display("No supported adapter. Existing boot remains available.\n");
        return pmm_free_count()==before;
    }
    if (e) {serial_puts("net: initialization failed: ");serial_puts(vnet_info()->why?vnet_info()->why:"?");serial_puts("\r\n");return 0;}
    display("Modern PCI transport; RX/TX queues: 8/8, polling\nPrivate DMA pool: 44 KiB; no application network grants\n");
    serial_puts("net: ready VERSION_1 MAC, queues 8/8, pool 11 frames\r\n");
    uint8_t tx[60]={0},rx[VNET_FRAME_MAX],expected[60];
    if (vnet_send(tx,13)!=VNET_EINVAL || vnet_send(tx,VNET_FRAME_MAX+1)!=VNET_EINVAL) return failed("invalid send",0);
    for (unsigned sequence=0;sequence<16;sequence++) {
        make_frame(tx,sequence,0);make_frame(expected,sequence,1);
        e=vnet_send(tx,sizeof(tx));if (e) return failed("send",e);
        uint64_t deadline=irq_ticks()+200;uint32_t n=0xabcdef01;memset(rx,0xa5,sizeof(rx));
        do {
            e=vnet_receive(rx,sequence?sizeof(rx):8,&n);
            if (e==VNET_EAGAIN) thread_sleep(1);
        } while (e==VNET_EAGAIN && irq_ticks()<deadline);
        if (!sequence) {
            if (e!=VNET_EMSGSIZE || n!=0xabcdef01) return failed("short RX",e);
            for (unsigned i=0;i<sizeof(rx);i++) if (rx[i]!=0xa5) return failed("short RX changed output",e);
            e=vnet_receive(rx,sizeof(rx),&n);if (e) return failed("retained RX",e);
        } else if (e) return failed("RX",e);
        if (n!=60 || memcmp(rx,expected,60)) return failed("RX bytes",e);
    }
    if (vnet_info()->transmitted!=16 || vnet_info()->received!=16) return failed("counters",0);
    if (vnet_stop() || vnet_info()->held || pmm_free_count()!=before || pmm_check_in(pmm_kernel())!=PMM_OK) return failed("pool release",0);
    serial_puts("net: PASS TX=16 RX=16; short buffer retained; DMA pool released\r\n");
    display("Ethernet frame exchange: 16 sent / 16 received\nShort RX buffer retained safely; queue slots reused\nReset confirmed; bus mastering off; DMA frames released\n\nRaw adapter increment only: ARP, IPv4 and ping are next.\n");
    return 1;
}
