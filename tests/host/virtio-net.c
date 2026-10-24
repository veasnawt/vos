/* Execute the actual driver against a bounded PCI/DMA device model. No
 * production test hooks: replace only the physical alias for host execution. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "memlayout.h"
#include "virtio_net.h"
static void *host_alias(uint64_t pa);
#define phys_to_virt host_alias
#undef KERNEL_VMA
#define KERNEL_VMA 1ULL
#undef VNET_POLL_LIMIT
#define VNET_POLL_LIMIT 32
#undef VNET_RESET_POLLS
#define VNET_RESET_POLLS 16
#include "../../arch/x86_64/virtio_net.c"

static uint8_t cfg[256],common[56],device[8],memory[VNET_POOL_FRAMES][4096] __attribute__((aligned(4096)));
static uint32_t window,window_size,dfsel,gfsel,qsel;
static uint64_t offered,accepted;
static uint16_t command_word,qsize[2],qenable[2];
static unsigned allocated,freed,checks;
static int found=1,hang,stall,budget=VNET_POOL_FRAMES,map_flags=VMM_WRITE;
static struct thread caller;
#define CHECK(x) do {checks++;assert(x);} while(0)
static void *host_alias(uint64_t pa) {assert(pa>=0x100000 && pa<0x100000+sizeof(memory));return (uint8_t *)memory+pa-0x100000;}
static uint32_t get(const void *p,unsigned n) {uint32_t v=0;memcpy(&v,p,n);return v;}
static void put(void *p,uint32_t v,unsigned n) {memcpy(p,&v,n);}
static uint32_t reg_read(void)
{
    if (window>=0x3000) return get(device+window-0x3000,window_size);
    if (window==DF) return (uint32_t)(offered>>(32*dfsel));
    if (window==GF) return (uint32_t)(accepted>>(32*gfsel));
    if (window==QSIZE) return qsize[qsel];
    if (window==QENABLE) return qenable[qsel];
    if (window==QNOTIFY) return qsel;
    assert(window+window_size<=sizeof(common));return get(common+window,window_size);
}
static void reg_write(uint32_t v)
{
    if (window>=0x1000) {
        if (v==1 && !stall) {
            struct net_queue *q=&queues[1];unsigned at=q->used[1]%VNET_QUEUE;
            q->entries[at]=(struct net_used){0,0};q->used[1]++;
        }
        return;
    }
    if (window==DFSELECT) dfsel=v;
    else if (window==GFSELECT) gfsel=v;
    else if (window==GF) {accepted&=~(0xffffffffULL<<(32*gfsel));accepted|=(uint64_t)v<<(32*gfsel);}
    else if (window==QSELECT) {assert(v<2);qsel=v;}
    else if (window==QSIZE) qsize[qsel]=(uint16_t)v;
    else if (window==QENABLE) qenable[qsel]=(uint16_t)v;
    else if (window==STATUS && v==0) {
        if (!hang) {common[STATUS]=0;qenable[0]=qenable[1]=0;accepted=0;}
    } else {assert(window+window_size<=sizeof(common));put(common+window,v,window_size);}
}
uint8_t pci_read8(struct pci_addr a,unsigned o) {(void)a;return cfg[o];}
uint16_t pci_read16(struct pci_addr a,unsigned o) {(void)a;return o==PCI_COMMAND?command_word:(uint16_t)get(cfg+o,2);}
uint32_t pci_read32(struct pci_addr a,unsigned o) {(void)a;return o==0x94?reg_read():get(cfg+o,4);}
void pci_write8(struct pci_addr a,unsigned o,uint8_t v) {(void)a;if(o==0x94)reg_write(v);else cfg[o]=v;}
void pci_write16(struct pci_addr a,unsigned o,uint16_t v) {(void)a;if(o==PCI_COMMAND)command_word=v;else if(o==0x94)reg_write(v);else put(cfg+o,v,2);}
void pci_write32(struct pci_addr a,unsigned o,uint32_t v) {(void)a;if(o==0x8c)window=v;else if(o==0x90)window_size=v;else if(o==0x94)reg_write(v);else put(cfg+o,v,4);}
int pci_scan_bus0(int (*f)(struct pci_addr,void *),void *c,struct pci_addr *a) {*a=(struct pci_addr){0,1,0};return found && f(*a,c);}
int pmm_alloc_frame(uint64_t *p) {if ((int)allocated>=budget) return PMM_ENOMEM;*p=0x100000+4096*allocated;memset(memory[allocated++],0,4096);return PMM_OK;}
int pmm_free_frame(uint64_t p) {CHECK(!(command_word&PCI_COMMAND_MASTER) && common[STATUS]==0);CHECK(p>=0x100000 && p<0x100000+4096*allocated);freed++;return PMM_OK;}
uint64_t vmm_kernel_root(void) {return 0;}
int vmm_lookup(uint64_t root,uint64_t va,struct vmm_mapping *m)
{
    (void)root;m->pa=va;m->flags=(unsigned)map_flags;
    if (va>=(uintptr_t)memory && va<(uintptr_t)memory+sizeof(memory)) m->pa=0x100000+va-(uintptr_t)memory;
    return VMM_OK;
}
int irq_in_handler(void) {return 0;}
struct thread *thread_current(void) {return &caller;}
void thread_yield(void) {}
static void fixture(void)
{
    memset(&net,0,sizeof(net));memset(queues,0,sizeof(queues));memset(cfg,0,sizeof(cfg));memset(common,0,sizeof(common));
    command_word=0;allocated=freed=0;found=1;hang=stall=0;budget=VNET_POOL_FRAMES;map_flags=VMM_WRITE;accepted=0;offered=FEATURES;
    qsize[0]=qsize[1]=8;qenable[0]=qenable[1]=0;put(cfg,0x10411af4,4);cfg[PCI_STATUS]=PCI_STATUS_CAP_LIST;cfg[PCI_CAP_PTR]=0x40;
    unsigned ptr[]={0x40,0x50,0x64,0x74,0x84};unsigned types[]={1,2,3,4,5};unsigned off[]={0,0x1000,0x2000,0x3000,0};unsigned len[]={56,4096,1,8,0};
    for (unsigned n=0;n<5;n++) {unsigned p=ptr[n];cfg[p]=9;cfg[p+1]=n==4?0:ptr[n+1];cfg[p+2]=(n==1 || n==4)?20:16;cfg[p+3]=types[n];put(cfg+p+8,off[n],4);put(cfg+p+12,len[n],4);}
    put(cfg+0x60,4,4); /* notify offset multiplier */
    put(common+NUMQ,2,2);memcpy(device,"\x52\x54\x00\x12\x34\x56",6);
}
static void receive_packet(unsigned id,uint32_t bytes)
{
    uint8_t *p=host_alias(net.pool[RX_BASE+id]);memset(p,0,PAGE_SIZE);p[10]=1;memset(p+VNET_HEADER,0x42,60);
    struct net_queue *q=&queues[0];q->entries[q->used[1]%VNET_QUEUE]=(struct net_used){id,bytes};q->used[1]++;
}
int main(void)
{
    CHECK(!choose_features(1ULL<<5));CHECK(!choose_features(FEATURES|(1ULL<<33)));CHECK(choose_features(FEATURES|0xffff));
    CHECK(!queue_size_ok(0) && !queue_size_ok(4) && !queue_size_ok(9) && queue_size_ok(8) && queue_size_ok(32768));
    uint8_t h[12]={0};CHECK(header_ok(h));h[10]=1;CHECK(header_ok(h));h[10]=255;h[11]=255;CHECK(header_ok(h));
    h[0]=1;CHECK(!header_ok(h));h[0]=0;h[1]=1;CHECK(!header_ok(h));h[1]=0;h[2]=255;CHECK(header_ok(h));
    fixture();found=0;CHECK(vnet_init()==VNET_ENODEV && !allocated);
    fixture();offered=0;CHECK(vnet_init()==VNET_EUNSUPPORTED && !allocated && !(command_word&PCI_COMMAND_MASTER));
    fixture();hang=1;common[STATUS]=15;CHECK(vnet_init()==VNET_EBROKEN && !allocated && !(command_word&PCI_COMMAND_MASTER));
    fixture();budget=3;CHECK(vnet_init()==VNET_ENOMEM && allocated==3 && freed==3 && !net.held);
    fixture();CHECK(vnet_init()==0 && net.held==VNET_POOL_FRAMES);CHECK(vnet_init()==VNET_EBUSY);
    uint8_t packet[VNET_FRAME_MAX],output[VNET_FRAME_MAX];memset(packet,0x55,sizeof(packet));
    CHECK(vnet_send(packet,13)==VNET_EINVAL && vnet_send(packet,sizeof(packet)+1)==VNET_EINVAL);
    CHECK(vnet_send(packet,14)==0);uint8_t *tx=host_alias(net.pool[TX_BUFFER]);CHECK(queues[1].desc[0].len==72);
    for (unsigned n=26;n<72;n++) CHECK(tx[n]==0);
    CHECK(vnet_send(packet,sizeof(packet))==0);
    uint32_t n=0xabcdef;memset(output,0xa5,sizeof(output));CHECK(vnet_receive(output,sizeof(output),&n)==VNET_EAGAIN && n==0xabcdef);
    receive_packet(0,72);CHECK(vnet_receive(output,8,&n)==VNET_EMSGSIZE && n==0xabcdef && output[0]==0xa5);
    CHECK(vnet_receive(output,sizeof(output),&n)==0 && n==60 && output[0]==0x42 && queues[0].published==9);
    map_flags=VMM_USER|VMM_WRITE;CHECK(vnet_send(packet,60)==VNET_EINVAL);map_flags=0;CHECK(vnet_receive(output,sizeof(output),&n)==VNET_EINVAL);map_flags=VMM_WRITE;
    CHECK(vnet_send(host_alias(net.pool[TX_BUFFER]),60)==VNET_EINVAL);
    CHECK(vnet_receive(output,60,(void *)output)==VNET_EINVAL);
    CHECK(vnet_stop()==0 && !net.held && freed==allocated);
    fixture();CHECK(vnet_init()==0);stall=1;CHECK(vnet_send(packet,60)==VNET_ETIMEOUT && net.state==VNET_BROKEN && !net.held);
    fixture();CHECK(vnet_init()==0);receive_packet(0,72);receive_packet(0,72);CHECK(vnet_receive(output,sizeof(output),&n)==VNET_EPROTO && !net.held);
    fixture();CHECK(vnet_init()==0);receive_packet(1,VNET_HEADER+VNET_FRAME_MAX+1);CHECK(vnet_receive(output,sizeof(output),&n)==VNET_EPROTO);
    fixture();CHECK(vnet_init()==0);receive_packet(0,72);((uint8_t *)host_alias(net.pool[RX_BASE]))[0]=1;hang=1;
    CHECK(vnet_receive(output,sizeof(output),&n)==VNET_EBROKEN && net.quarantined && net.held==VNET_POOL_FRAMES && !freed);
    CHECK(!(command_word&PCI_COMMAND_MASTER) && vnet_init()==VNET_EBUSY);hang=0;CHECK(vnet_stop()==0 && freed==allocated && !net.quarantined);
    fixture();CHECK(vnet_init()==0);queues[1].used[1]++;CHECK(vnet_send(packet,60)==VNET_EPROTO);
    fixture();CHECK(vnet_init()==0);queues[0].consumed=65535;queues[0].entries[7]=(struct net_used){0,72};queues[0].entries[0]=(struct net_used){1,72};
    CHECK(used_ok(&queues[0],2,0));CHECK(!used_ok(&queues[0],9,0));queues[0].entries[0].id=99;CHECK(!used_ok(&queues[0],2,0));
    CHECK(vnet_stop()==0);
    printf("virtio-net host: %u checks PASS (real driver, PCI/DMA model, reset quarantine)\n",checks);
    return 0;
}
