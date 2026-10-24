#include "virtio_net.h"
#include "virtio_blk.h" /* Existing bounded PCI capability parser/window checks. */
#include "pmm.h"
#include "vmm.h"
#include "thread.h"
#include "irq.h"
#include "lib.h"

#define DFSELECT 0
#define DF 4
#define GFSELECT 8
#define GF 12
#define MSIX 16
#define NUMQ 18
#define STATUS 20
#define GENERATION 21
#define QSELECT 22
#define QSIZE 24
#define QMSIX 26
#define QENABLE 28
#define QNOTIFY 30
#define QDESC 32
#define QAVAIL 40
#define QUSED 48
#define FEATURES ((1ULL<<32) | (1ULL<<5)) /* VERSION_1 and MAC only. */
#define RX_BASE 2
#define TX_BUFFER (2 + VNET_QUEUE)
#define AVAIL_OFF (16 * VNET_QUEUE)
#define USED_OFF 1024
struct net_desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct net_used { uint32_t id, len; };
struct net_queue {
    volatile struct net_desc *desc;
    volatile uint16_t *avail, *used;
    volatile struct net_used *entries;
    uint16_t published, consumed;
    uint32_t notify;
};
_Static_assert(sizeof(struct net_desc)==16 && sizeof(struct net_used)==8,"virtqueue layout");
_Static_assert(AVAIL_OFF+6+2*VNET_QUEUE<=USED_OFF && USED_OFF+6+8*VNET_QUEUE<=PAGE_SIZE,"ring bounds");
_Static_assert(VNET_HEADER+VNET_FRAME_MAX<=PAGE_SIZE,"packet bounds");
static struct vnet_info net;
static struct vpci_caps caps;
static struct net_queue queues[2];
static struct thread *owner;
static int access_error, present, sending;
static void fence(void) { __asm__ volatile("mfence" ::: "memory"); }

static uint32_t read_reg(const struct vpci_region *r,uint32_t off,unsigned size)
{
    if ((uint64_t)off+size>r->length || !vblk_window_ok(&caps,r->bar,r->offset+off,size)) {access_error=1;return 0;}
    pci_write8(net.pci,caps.cfg_cap+4,r->bar);
    pci_write32(net.pci,caps.cfg_cap+8,r->offset+off);
    pci_write32(net.pci,caps.cfg_cap+12,size);
    uint32_t v=pci_read32(net.pci,caps.cfg_cap+16);
    return size==4?v:v&((1u<<(size*8))-1);
}
static void write_reg(const struct vpci_region *r,uint32_t off,unsigned size,uint32_t v)
{
    if ((uint64_t)off+size>r->length || !vblk_window_ok(&caps,r->bar,r->offset+off,size)) {access_error=1;return;}
    pci_write8(net.pci,caps.cfg_cap+4,r->bar);
    pci_write32(net.pci,caps.cfg_cap+8,r->offset+off);
    pci_write32(net.pci,caps.cfg_cap+12,size);
    if (size==1) pci_write8(net.pci,caps.cfg_cap+16,(uint8_t)v);
    else if (size==2) pci_write16(net.pci,caps.cfg_cap+16,(uint16_t)v);
    else pci_write32(net.pci,caps.cfg_cap+16,v);
}
static void mastering(int on)
{
    uint16_t c=pci_read16(net.pci,PCI_COMMAND);
    c|=PCI_COMMAND_MEMORY|PCI_COMMAND_IO|PCI_COMMAND_INTX_OFF;
    c&=~PCI_COMMAND_MASTER;
    pci_write16(net.pci,PCI_COMMAND,c|(on?PCI_COMMAND_MASTER:0));
}
static int reset(void)
{
    write_reg(&caps.common,STATUS,1,0);
    for (unsigned i=0;i<VNET_RESET_POLLS;i++)
        if (!read_reg(&caps.common,STATUS,1) && !access_error) return 1;
    return 0;
}
static void release(void)
{
    for (int i=0;i<net.held;i++) pmm_free_frame(net.pool[i]);
    net.held=0;net.quarantined=0;
    memset(queues,0,sizeof(queues));
}
static int stop_as(int state,const char *why,int error)
{
    net.state=state;net.why=why; /* Unavailable before reset, including across errors. */
    int confirmed=reset();
    mastering(0);
    if (confirmed) release();
    else {net.quarantined=net.held!=0;net.state=VNET_BROKEN;}
    return confirmed?error:VNET_EBROKEN;
}
static int broken(const char *why,int error) {return stop_as(VNET_BROKEN,why,error);}
static int context(void) {return !irq_in_handler() && owner==thread_current();}
static int ready(void)
{
    if (!context()) return VNET_EBUSY;
    if (net.state!=VNET_READY) return net.state==VNET_BROKEN?VNET_EBROKEN:VNET_ENODEV;
    if (read_reg(&caps.common,STATUS,1)!=15 || access_error) return broken("device status changed",VNET_EPROTO);
    return VNET_OK;
}
/* Kernel mappings only; never alias any DMA pool page. */
static int buffer_ok(const void *buffer,uint32_t length,int output)
{
    uint64_t va=(uint64_t)buffer,end=va+length;
    if (!buffer || !length || va<KERNEL_VMA || end<=va) return 0;
    for (uint64_t p=va&~(PAGE_SIZE-1);p<end;p+=PAGE_SIZE) {
        struct vmm_mapping m;
        if (vmm_lookup(vmm_kernel_root(),p,&m)!=VMM_OK || (m.flags&VMM_USER) || (output && !(m.flags&VMM_WRITE))) return 0;
        for (int i=0;i<net.held;i++) if ((m.pa&~(PAGE_SIZE-1))==net.pool[i]) return 0;
        if (p>UINT64_MAX-PAGE_SIZE) return 0;
    }
    return 1;
}
static int choose_features(uint64_t offered)
{
    return (offered&FEATURES)==FEATURES && !(offered&(1ULL<<33)); /* No ACCESS_PLATFORM/IOMMU. */
}
static int queue_size_ok(unsigned size) {return size>=VNET_QUEUE && size<=32768 && !(size&(size-1));}
static int used_ok(const struct net_queue *q,unsigned count,int tx)
{
    if (count>(tx?1:VNET_QUEUE)) return 0;
    unsigned seen=0;
    for (unsigned n=0;n<count;n++) {
        unsigned at=(uint16_t)(q->consumed+n)%VNET_QUEUE;
        uint32_t id=q->entries[at].id,len=q->entries[at].len;
        if (id>=(tx?1:VNET_QUEUE) || (seen&(1u<<id))) return 0;
        if (tx ? len!=0 : (len<VNET_HEADER+14 || len>VNET_HEADER+VNET_FRAME_MAX)) return 0;
        seen|=1u<<id;
    }
    return 1;
}
/* MRG_RXBUF is never negotiated. Exactly one used descriptor is the packet;
 * num_buffers cannot authorize additional buffers. QEMU 10.2.1's socket RX
 * path leaves that field zero although virtio 1.2 asks devices to set it to 1.
 * Ignore it in this mode; validate actual used IDs/lengths and offloads instead. */
static int header_ok(const uint8_t *p) {return p[0]==0 && p[1]==0;}
static void publish(struct net_queue *q,unsigned id)
{
    q->avail[2+q->published%VNET_QUEUE]=(uint16_t)id;
    fence();q->published++;q->avail[1]=q->published;fence();
}
static void notify(unsigned q) {write_reg(&caps.notify,queues[q].notify,2,q);}
static uint8_t cap8(void *a,unsigned o) {return pci_read8(*(struct pci_addr *)a,o);}
static uint32_t cap32(void *a,unsigned o) {return pci_read32(*(struct pci_addr *)a,o);}
static int match(struct pci_addr a,void *unused)
{
    (void)unused;uint16_t id=pci_read16(a,PCI_DEVICE_ID);
    return pci_read16(a,PCI_VENDOR_ID)==0x1af4 && (id==0x1041 || (id==0x1000 && pci_read16(a,PCI_SUBSYSTEM_ID)==1));
}
static int setup_queue(unsigned i)
{
    write_reg(&caps.common,QSELECT,2,i);
    if (!queue_size_ok(read_reg(&caps.common,QSIZE,2)) || read_reg(&caps.common,QENABLE,2)) return 0;
    write_reg(&caps.common,QSIZE,2,VNET_QUEUE);
    if (read_reg(&caps.common,QSIZE,2)!=VNET_QUEUE) return 0;
    struct net_queue *q=&queues[i];uint64_t pa=net.pool[i];uint8_t *p=phys_to_virt(pa);
    q->desc=(void *)p;q->avail=(void *)(p+AVAIL_OFF);q->used=(void *)(p+USED_OFF);q->entries=(void *)(p+USED_OFF+4);
    q->avail[0]=1; /* No interrupt notifications; PCI INTx is also disabled. */
    const unsigned regs[]={QDESC,QAVAIL,QUSED};const unsigned offsets[]={0,AVAIL_OFF,USED_OFF};
    for (unsigned n=0;n<3;n++) {
        uint64_t a=pa+offsets[n];write_reg(&caps.common,regs[n],4,(uint32_t)a);write_reg(&caps.common,regs[n]+4,4,(uint32_t)(a>>32));
    }
    uint64_t offset=(uint64_t)read_reg(&caps.common,QNOTIFY,2)*caps.notify_mult;
    if (offset>caps.notify.length || caps.notify.length-offset<2) return 0;
    q->notify=(uint32_t)offset;
    write_reg(&caps.common,QMSIX,2,0xffff);
    write_reg(&caps.common,QENABLE,2,1);
    return read_reg(&caps.common,QENABLE,2)==1 && !access_error;
}
int vnet_init(void)
{
    if (irq_in_handler() || net.held || (net.state!=VNET_ABSENT && net.state!=VNET_STOPPED)) return VNET_EBUSY;
    memset(&net,0,sizeof(net));memset(&caps,0,sizeof(caps));memset(queues,0,sizeof(queues));
    owner=thread_current();access_error=0;present=0;sending=0;
    if (!pci_scan_bus0(match,0,&net.pci)) return VNET_ENODEV;
    present=1;net.state=VNET_FAILED;
    const char *why=vpci_parse_caps(cap8,cap32,&net.pci,&caps);
    if (why) {net.why=why;return VNET_EUNSUPPORTED;} /* No DMA or command changes yet. */
    mastering(0);
    if (!reset()) {net.state=VNET_BROKEN;net.why="initial reset unconfirmed";return VNET_EBROKEN;}
    write_reg(&caps.common,STATUS,1,1);write_reg(&caps.common,STATUS,1,3);
    write_reg(&caps.common,DFSELECT,4,0);net.offered=read_reg(&caps.common,DF,4);
    write_reg(&caps.common,DFSELECT,4,1);net.offered|=(uint64_t)read_reg(&caps.common,DF,4)<<32;
    if (!choose_features(net.offered)) return stop_as(VNET_FAILED,"unsupported features",VNET_EUNSUPPORTED);
    net.accepted=FEATURES;
    for (unsigned i=0;i<2;i++) {
        write_reg(&caps.common,GFSELECT,4,i);write_reg(&caps.common,GF,4,(uint32_t)(FEATURES>>(32*i)));
        if (read_reg(&caps.common,GF,4)!=(uint32_t)(FEATURES>>(32*i))) return stop_as(VNET_FAILED,"feature readback",VNET_EPROTO);
    }
    write_reg(&caps.common,STATUS,1,11);
    if (read_reg(&caps.common,STATUS,1)!=11 || access_error) return stop_as(VNET_FAILED,"FEATURES_OK refused",VNET_EUNSUPPORTED);
    if (read_reg(&caps.common,NUMQ,2)<2) return stop_as(VNET_FAILED,"missing RX/TX queues",VNET_EUNSUPPORTED);
    int stable=0;
    for (unsigned n=0;n<8;n++) {
        uint8_t generation=read_reg(&caps.common,GENERATION,1);
        for (unsigned i=0;i<6;i++) net.mac[i]=(uint8_t)read_reg(&caps.device,i,1);
        if (generation==read_reg(&caps.common,GENERATION,1)) {stable=1;break;}
    }
    uint8_t any=0;for (unsigned i=0;i<6;i++) any|=net.mac[i];
    if (!stable || !any || (net.mac[0]&1)) return stop_as(VNET_FAILED,"invalid/unstable MAC",VNET_EPROTO);
    for (int i=0;i<VNET_POOL_FRAMES;i++) {
        if (pmm_alloc_frame(&net.pool[i])!=PMM_OK) return stop_as(VNET_FAILED,"DMA pool exhausted",VNET_ENOMEM);
        net.held++;
    }
    write_reg(&caps.common,MSIX,2,0xffff);
    if (!setup_queue(0) || !setup_queue(1)) return stop_as(VNET_FAILED,"invalid queue configuration",VNET_EPROTO);
    for (unsigned i=0;i<VNET_QUEUE;i++) {
        queues[0].desc[i]=(struct net_desc){net.pool[RX_BASE+i],VNET_HEADER+VNET_FRAME_MAX,2,0};
        publish(&queues[0],i);
    }
    mastering(1); /* All queue addresses and buffers are now initialized. */
    write_reg(&caps.common,STATUS,1,15);
    if (read_reg(&caps.common,STATUS,1)!=15 || access_error) return stop_as(VNET_FAILED,"DRIVER_OK refused",VNET_EPROTO);
    net.state=VNET_READY;notify(0);
    if (access_error) return broken("notification access",VNET_EPROTO);
    return VNET_OK;
}
int vnet_send(const void *frame,uint32_t length)
{
    int err=ready();if (err) return err;
    if (sending) return VNET_EBUSY;
    if (length<14 || length>VNET_FRAME_MAX || !buffer_ok(frame,length,0)) return VNET_EINVAL;
    struct net_queue *q=&queues[1];fence();
    if (q->used[1]!=q->consumed) return broken("unsolicited TX completion",VNET_EPROTO);
    sending=1;uint8_t *p=phys_to_virt(net.pool[TX_BUFFER]);memset(p,0,PAGE_SIZE);
    memcpy(p+VNET_HEADER,frame,length);uint32_t wire=length<60?60:length;
    q->desc[0]=(struct net_desc){net.pool[TX_BUFFER],VNET_HEADER+wire,0,0};publish(q,0);notify(1);
    for (unsigned n=0;n<VNET_POLL_LIMIT;n++) {
        uint16_t count=(uint16_t)(q->used[1]-q->consumed);fence();
        if (access_error || !used_ok(q,count,1)) {sending=0;return broken("invalid TX completion",VNET_EPROTO);}
        if (count) {q->consumed++;net.transmitted++;sending=0;return VNET_OK;}
        if (!(n%4096)) {err=ready();if (err) {sending=0;return err;}thread_yield();}
        __asm__ volatile("pause");
    }
    sending=0;return broken("TX deadline",VNET_ETIMEOUT);
}
int vnet_receive(void *frame,uint32_t capacity,uint32_t *length)
{
    int err=ready();if (err) return err;
    uint64_t a=(uint64_t)frame,b=(uint64_t)length;
    if (!capacity || capacity>VNET_FRAME_MAX || !buffer_ok(frame,capacity,1) || !buffer_ok(length,sizeof(*length),1) ||
        (a<b+sizeof(*length) && b<a+capacity)) return VNET_EINVAL;
    struct net_queue *q=&queues[0];uint16_t count=(uint16_t)(q->used[1]-q->consumed);fence();
    if (!used_ok(q,count,0)) return broken("invalid RX completion",VNET_EPROTO);
    if (!count) return VNET_EAGAIN;
    uint32_t id=q->entries[q->consumed%VNET_QUEUE].id,n=q->entries[q->consumed%VNET_QUEUE].len-VNET_HEADER;
    uint8_t *p=phys_to_virt(net.pool[RX_BASE+id]);
    memcpy(net.last_rx_header,p,VNET_HEADER);net.last_rx_length=n;
    if (!header_ok(p) || q->desc[id].addr!=net.pool[RX_BASE+id] || q->desc[id].len!=VNET_HEADER+VNET_FRAME_MAX || q->desc[id].flags!=2 || q->desc[id].next)
        return broken("invalid RX header/descriptor",VNET_EPROTO);
    if (capacity<n) return VNET_EMSGSIZE;
    memcpy(frame,p+VNET_HEADER,n);*length=n;q->consumed++;net.received++;
    publish(q,id);notify(0);
    if (access_error) return broken("RX notification access",VNET_EPROTO);
    return VNET_OK;
}
int vnet_stop(void)
{
    if (!context() || sending) return VNET_EBUSY;
    if (!present) return VNET_ENODEV;
    return stop_as(VNET_STOPPED,"stopped",VNET_OK);
}
const struct vnet_info *vnet_info(void) {return &net;}
