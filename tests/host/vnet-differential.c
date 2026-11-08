/* R3-N3: transcript differential for the virtio-net validation decisions.
 * This file includes a driver source (VNET_DRIVER: the frozen accepted C at
 * tests/reference/virtio-net.c, or the production arch/x86_64/virtio_net.c
 * that calls the Rust decisions) against one bounded PCI/DMA device model, and
 * writes a transcript of every decision and every driver outcome. The two
 * builds must produce byte-identical transcripts. Only the physical alias and
 * the poll limits are replaced for host execution, as in tests/host/virtio-net.c. */
#include <stdio.h>
#include <stdlib.h>
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
#include VNET_DRIVER

static FILE *T;static unsigned long long records;
static void rec(const char *k,long long v){fprintf(T,"%s %lld\n",k,v);records++;}
static uint64_t rng=UINT64_C(0x52334e3356534e45);
static uint64_t next64(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
/* ---------------- device model (as tests/host/virtio-net.c, with fault knobs) ---------------- */
static uint8_t cfg[256],common[56],device[8],memory[VNET_POOL_FRAMES][4096] __attribute__((aligned(4096)));
static uint32_t window,window_size,dfsel,gfsel,qsel;
static uint64_t offered,accepted;
static uint16_t command_word,qsize[2],qenable[2],numq;
static unsigned allocated,freed,generation_reads;
static int found=1,hang,stall,budget=VNET_POOL_FRAMES,map_flags=VMM_WRITE,unstable,tx_bad_id,tx_bad_len,tx_burst,qnotify_value=-1;
static struct thread caller;
static void *host_alias(uint64_t pa) {if(!(pa>=0x100000 && pa<0x100000+sizeof(memory))){fprintf(stderr,"bad alias\n");exit(3);}return (uint8_t *)memory+pa-0x100000;}
static uint32_t get(const void *p,unsigned n) {uint32_t v=0;memcpy(&v,p,n);return v;}
static void put(void *p,uint32_t v,unsigned n) {memcpy(p,&v,n);}
static uint32_t reg_read(void)
{
    if (window>=0x3000) return get(device+window-0x3000,window_size);
    if (window==DF) return (uint32_t)(offered>>(32*dfsel));
    if (window==GF) return (uint32_t)(accepted>>(32*gfsel));
    if (window==QSIZE) return qsize[qsel];
    if (window==QENABLE) return qenable[qsel];
    if (window==QNOTIFY) return qnotify_value<0?qsel:(uint32_t)qnotify_value;
    if (window==NUMQ) return numq;
    if (window==GENERATION) return unstable?(uint8_t)(generation_reads++):common[GENERATION];
    if (window+window_size>sizeof(common)) {fprintf(stderr,"window\n");exit(3);}
    return get(common+window,window_size);
}
static void reg_write(uint32_t v)
{
    if (window>=0x1000) {
        if (v==1 && !stall) {
            struct net_queue *q=&queues[1];
            for (int k=0;k<(tx_burst?2:1);k++){unsigned at=q->used[1]%VNET_QUEUE;q->entries[at]=(struct net_used){(uint32_t)tx_bad_id,(uint32_t)tx_bad_len};q->used[1]++;}
        }
        return;
    }
    if (window==DFSELECT) dfsel=v;
    else if (window==GFSELECT) gfsel=v;
    else if (window==GF) {accepted&=~(0xffffffffULL<<(32*gfsel));accepted|=(uint64_t)v<<(32*gfsel);}
    else if (window==QSELECT) {qsel=v&1;}
    else if (window==QSIZE) qsize[qsel]=(uint16_t)v;
    else if (window==QENABLE) qenable[qsel]=(uint16_t)v;
    else if (window==STATUS && v==0) {if (!hang) {common[STATUS]=0;qenable[0]=qenable[1]=0;accepted=0;}}
    else if (window+window_size<=sizeof(common)) put(common+window,v,window_size);
}
uint8_t pci_read8(struct pci_addr a,unsigned o) {(void)a;return cfg[o];}
uint16_t pci_read16(struct pci_addr a,unsigned o) {(void)a;return o==PCI_COMMAND?command_word:(uint16_t)get(cfg+o,2);}
uint32_t pci_read32(struct pci_addr a,unsigned o) {(void)a;return o==0x94?reg_read():get(cfg+o,4);}
void pci_write8(struct pci_addr a,unsigned o,uint8_t v) {(void)a;if(o==0x94)reg_write(v);else cfg[o]=v;}
void pci_write16(struct pci_addr a,unsigned o,uint16_t v) {(void)a;if(o==PCI_COMMAND)command_word=v;else if(o==0x94)reg_write(v);else put(cfg+o,v,2);}
void pci_write32(struct pci_addr a,unsigned o,uint32_t v) {(void)a;if(o==0x8c)window=v;else if(o==0x90)window_size=v;else if(o==0x94)reg_write(v);else put(cfg+o,v,4);}
int pci_scan_bus0(int (*f)(struct pci_addr,void *),void *c,struct pci_addr *a) {*a=(struct pci_addr){0,1,0};return found && f(*a,c);}
int pmm_alloc_frame(uint64_t *p) {if ((int)allocated>=budget) return PMM_ENOMEM;*p=0x100000+4096*allocated;memset(memory[allocated++],0,4096);return PMM_OK;}
int pmm_free_frame(uint64_t p) {(void)p;freed++;return PMM_OK;}
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
    if (net.held) {hang=0;(void)vnet_stop();}
    memset(&net,0,sizeof(net));memset(queues,0,sizeof(queues));memset(cfg,0,sizeof(cfg));memset(common,0,sizeof(common));memset(memory,0,sizeof(memory));
    command_word=0;allocated=freed=generation_reads=0;found=1;hang=stall=unstable=tx_bad_id=tx_bad_len=tx_burst=0;qnotify_value=-1;
    budget=VNET_POOL_FRAMES;map_flags=VMM_WRITE;accepted=0;offered=FEATURES;numq=2;
    qsize[0]=qsize[1]=8;qenable[0]=qenable[1]=0;put(cfg,0x10411af4,4);cfg[PCI_STATUS]=PCI_STATUS_CAP_LIST;cfg[PCI_CAP_PTR]=0x40;
    unsigned ptr[]={0x40,0x50,0x64,0x74,0x84};unsigned types[]={1,2,3,4,5};unsigned off[]={0,0x1000,0x2000,0x3000,0};unsigned len[]={56,4096,1,8,0};
    for (unsigned n=0;n<5;n++) {unsigned p=ptr[n];cfg[p]=9;cfg[p+1]=n==4?0:ptr[n+1];cfg[p+2]=(n==1 || n==4)?20:16;cfg[p+3]=types[n];put(cfg+p+8,off[n],4);put(cfg+p+12,len[n],4);}
    put(cfg+0x60,4,4);
    memcpy(device,"\x52\x54\x00\x12\x34\x56",6);
}
/* Whole observable state after a driver call. */
static void state(const char *what,int rc)
{
    fprintf(T,"%s rc=%d st=%d held=%d q=%d why=%s off=%llx acc=%llx tx=%llu rx=%llu last=%u cmd=%04x alloc=%u freed=%u rxp=%u rxc=%u txp=%u txc=%u mac=%02x%02x%02x%02x%02x%02x hdr=",
            what,rc,net.state,net.held,net.quarantined,net.why?net.why:"-",(unsigned long long)net.offered,(unsigned long long)net.accepted,
            (unsigned long long)net.transmitted,(unsigned long long)net.received,net.last_rx_length,command_word,allocated,freed,
            queues[0].published,queues[0].consumed,queues[1].published,queues[1].consumed,net.mac[0],net.mac[1],net.mac[2],net.mac[3],net.mac[4],net.mac[5]);
    for (int i=0;i<VNET_HEADER;i++) fprintf(T,"%02x",net.last_rx_header[i]);
    const uint8_t *m=(const uint8_t *)memory;unsigned h=2166136261u;for (size_t i=0;i<sizeof(memory);i++) h=(h^m[i])*16777619u;
    fprintf(T," mem=%08x\n",h);records++;
}
/* ---------------- stage 1: pure decisions, exhaustive where the domain allows ---------------- */
static void decisions(void)
{
    static const unsigned bits[10]={5,32,33,0,1,31,34,63,6,40};
    for (unsigned m=0;m<1024;m++){uint64_t v=0;for(int b=0;b<10;b++)if(m>>b&1)v|=1ULL<<bits[b];rec("feat",choose_features(v));}
    for (int i=0;i<20000;i++){uint64_t v=next64();if(i&1)v|=FEATURES;if(i&2)v&=~(1ULL<<33);rec("featr",choose_features(v));}
    for (unsigned s=0;s<=65536;s++) rec("qsize",queue_size_ok(s));
    static const unsigned big[]={65537,1u<<17,1u<<20,1u<<31,0xffffffffu,0x80000000u+8,32769,32767,16384,24576};
    for (unsigned i=0;i<sizeof big/sizeof *big;i++) rec("qbig",queue_size_ok(big[i]));
    uint8_t h[VNET_HEADER];
    for (unsigned a=0;a<256;a++)for(unsigned b=0;b<256;b++){for(int i=0;i<VNET_HEADER;i++)h[i]=(uint8_t)next64();h[0]=(uint8_t)a;h[1]=(uint8_t)b;rec("hdr",header_ok(h));}
}
/* ---------------- stage 2: used-ring validation with fault injection at every entry ---------------- */
static struct net_used ring[VNET_QUEUE];
static int used_case(uint16_t consumed,unsigned count,int tx)
{
    struct net_queue q;memset(&q,0,sizeof q);q.entries=ring;q.consumed=consumed;return used_ok(&q,count,tx);
}
static void used(void)
{
    static const uint16_t cons[6]={0,5,7,8,65535,65530};
    static const uint32_t badid[5]={8,9,31,32,0xffffffffu};
    static const uint32_t lens[12]={0,1,25,26,27,800,1525,1526,1527,4096,0x80000000u,0xffffffffu};
    for (int tx=0;tx<2;tx++)for(unsigned ci=0;ci<6;ci++){
        /* count bound, every count 0..10 and far beyond */
        for (unsigned count=0;count<=12;count++){for(unsigned i=0;i<VNET_QUEUE;i++)ring[i]=(struct net_used){(uint32_t)(i),tx?0:72};rec("ucount",used_case(cons[ci],count,tx));}
        rec("ucount",used_case(cons[ci],65535,tx));rec("ucount",used_case(cons[ci],0xffffffffu,tx));
        unsigned max=tx?1:VNET_QUEUE;
        for (unsigned count=1;count<=max;count++)for(unsigned pos=0;pos<count;pos++){
            /* valid permutation, then one fault at `pos` */
            for (unsigned i=0;i<VNET_QUEUE;i++)ring[i]=(struct net_used){0,tx?0:72};
            for (unsigned n=0;n<count;n++)ring[(uint16_t)(cons[ci]+n)%VNET_QUEUE]=(struct net_used){(n*3+ci)%max,tx?0:26+n};
            for (unsigned n=0;n<count;n++){unsigned at=(uint16_t)(cons[ci]+n)%VNET_QUEUE;ring[at].id=n;}
            rec("uvalid",used_case(cons[ci],count,tx));
            unsigned at=(uint16_t)(cons[ci]+pos)%VNET_QUEUE;struct net_used keep=ring[at];
            for (unsigned b=0;b<5;b++){ring[at].id=badid[b];rec("ubadid",used_case(cons[ci],count,tx));}ring[at]=keep;
            if (pos){ring[at].id=ring[(uint16_t)(cons[ci]+pos-1)%VNET_QUEUE].id;rec("udup",used_case(cons[ci],count,tx));ring[at]=keep;
                     ring[at].id=ring[(uint16_t)(cons[ci])%VNET_QUEUE].id;rec("udup0",used_case(cons[ci],count,tx));ring[at]=keep;}
            for (unsigned l=0;l<12;l++){ring[at].len=lens[l];rec("ulen",used_case(cons[ci],count,tx));}ring[at]=keep;
        }
    }
    for (int i=0;i<60000;i++){int tx=(i%7)==0;unsigned count=(unsigned)(next64()%(tx?3:11));uint16_t c=(uint16_t)next64();
        for(unsigned k=0;k<VNET_QUEUE;k++){uint64_t r=next64();ring[k]=(struct net_used){(uint32_t)(r%(i%3?9:12)),(uint32_t)((r>>16)%(i%5?1530:70000))};if(i%4==0)ring[k].len=26+(uint32_t)(r>>40)%1500;}
        rec("urand",used_case(c,count,tx));}
}
/* ---------------- stage 3: whole-driver scenarios with device fault injection ---------------- */
static uint8_t packet[VNET_FRAME_MAX+64],output[VNET_FRAME_MAX+64];
static void receive_packet(unsigned id,uint32_t bytes,uint8_t h0,uint8_t h1)
{
    uint8_t *p=host_alias(net.pool[RX_BASE+(id%VNET_QUEUE)]);memset(p,0,PAGE_SIZE);p[0]=h0;p[1]=h1;p[10]=1;memset(p+VNET_HEADER,0x42,1514);
    struct net_queue *q=&queues[0];q->entries[q->used[1]%VNET_QUEUE]=(struct net_used){id,bytes};q->used[1]++;
}
static void scenarios(void)
{
    static const uint64_t feats[8]={FEATURES,0,1ULL<<5,1ULL<<32,FEATURES|(1ULL<<33),FEATURES|0xffffULL,~0ULL,FEATURES|(1ULL<<35)};
    for (int f=0;f<8;f++){fixture();offered=feats[f];state("feat-init",vnet_init());}
    static const uint16_t nq[5]={0,1,2,3,0xffff};
    for (int n=0;n<5;n++){fixture();numq=nq[n];state("numq-init",vnet_init());}
    static const uint16_t qs[8]={0,4,8,9,16,32768,65535,7};
    for (int a=0;a<8;a++)for(int b=0;b<8;b+=3){fixture();qsize[0]=qs[a];qsize[1]=qs[b];state("qsize-init",vnet_init());}
    fixture();qenable[1]=1;state("qenable-init",vnet_init());
    /* notify offset x multiplier against the 4096-byte window */
    static const int qn[11]={0,1,1023,1024,1025,0xffff,-1,4094,4095,4096,4097};static const uint32_t mul[4]={0,1,4,0x10000};
    for (int a=0;a<11;a++)for(int b=0;b<4;b++){fixture();qnotify_value=qn[a];put(cfg+0x60,mul[b],4);state("notify-init",vnet_init());}
    static const char *macs[6]={"\x00\x00\x00\x00\x00\x00","\x01\x54\x00\x12\x34\x56","\x52\x54\x00\x12\x34\x56","\x00\x00\x00\x00\x00\x01","\xfe\xff\xff\xff\xff\xff","\x03\x00\x00\x00\x00\x00"};
    for (int m=0;m<6;m++)for(int u=0;u<2;u++){fixture();memcpy(device,macs[m],6);unstable=u;state("mac-init",vnet_init());}
    fixture();found=0;state("absent-init",vnet_init());
    fixture();hang=1;common[STATUS]=15;state("hang-init",vnet_init());
    for (int bud=0;bud<=VNET_POOL_FRAMES;bud+=4){fixture();budget=bud;state("budget-init",vnet_init());}
    memset(packet,0x55,sizeof packet);
    /* send: lengths, aliasing the DMA pool, user/readonly mappings, TX completion faults */
    static const uint32_t sl[10]={0,1,13,14,15,59,60,61,1514,1515};
    for (int i=0;i<10;i++){fixture();vnet_init();state("send",vnet_send(packet,sl[i]));}
    fixture();vnet_init();state("send-null",vnet_send(NULL,60));
    fixture();vnet_init();state("send-dma",vnet_send(host_alias(net.pool[TX_BUFFER]),60));
    fixture();vnet_init();map_flags=VMM_USER|VMM_WRITE;state("send-user",vnet_send(packet,60));
    static const int tid[3]={0,1,8};static const int tlen[3]={0,1,72};
    for (int a=0;a<3;a++)for(int b=0;b<3;b++){fixture();vnet_init();tx_bad_id=tid[a];tx_bad_len=tlen[b];state("send-txfault",vnet_send(packet,60));}
    fixture();vnet_init();tx_burst=1;state("send-burst",vnet_send(packet,60));
    fixture();vnet_init();stall=1;state("send-stall",vnet_send(packet,60));
    fixture();vnet_init();queues[1].used[1]++;state("send-unsolicited",vnet_send(packet,60));
    /* receive: capacities, output overlap, every used/header/descriptor fault */
    static const uint32_t cap[9]={0,1,8,59,60,61,1514,1515,0xffffffffu};
    for (int i=0;i<9;i++){fixture();vnet_init();receive_packet(0,72,0,0);uint32_t n=0xabcdef;state("recv-cap",vnet_receive(output,cap[i],&n));fprintf(T,"n=%u out0=%02x\n",n,output[0]);}
    static const int ov[6]={-8,-4,-3,0,59,60};
    for (int i=0;i<6;i++){fixture();vnet_init();receive_packet(0,72,0,0);state("recv-overlap",vnet_receive(output+8,60,(uint32_t *)(void *)(output+8+ov[i])));}
    static const uint32_t rl[9]={0,25,26,27,72,1525,1526,1527,0xffffffffu};
    for (int i=0;i<9;i++){fixture();vnet_init();receive_packet(0,rl[i],0,0);uint32_t n=0;state("recv-len",vnet_receive(output,1514,&n));}
    static const uint32_t rid[6]={0,1,7,8,9,0xffffffffu};
    for (int i=0;i<6;i++){fixture();vnet_init();receive_packet(rid[i],72,0,0);uint32_t n=0;state("recv-id",vnet_receive(output,1514,&n));}
    static const uint8_t hb[5][2]={{0,0},{1,0},{0,1},{0x80,0},{0xff,0xff}};
    for (int i=0;i<5;i++){fixture();vnet_init();receive_packet(2,100,hb[i][0],hb[i][1]);uint32_t n=0;state("recv-hdr",vnet_receive(output,1514,&n));}
    for (int i=0;i<4;i++){fixture();vnet_init();receive_packet(3,100,0,0);
        if(i==0)queues[0].desc[3].len--; else if(i==1)queues[0].desc[3].flags=0; else if(i==2)queues[0].desc[3].next=1; else queues[0].desc[3].addr+=8;
        uint32_t n=0;state("recv-desc",vnet_receive(output,1514,&n));}
    for (unsigned k=0;k<=10;k++){fixture();vnet_init();for(unsigned j=0;j<k;j++)receive_packet(j%VNET_QUEUE,72,0,0);uint32_t n=0;state("recv-count",vnet_receive(output,1514,&n));}
    fixture();vnet_init();receive_packet(4,72,0,0);receive_packet(4,72,0,0);{uint32_t n=0;state("recv-dup",vnet_receive(output,1514,&n));}
    /* consumed-index wrap over a long stream, interleaving sends */
    fixture();vnet_init();
    for (unsigned i=0;i<70000;i++){unsigned id=queues[0].consumed%VNET_QUEUE;receive_packet(id,26+(i%1489),0,0);uint32_t n=0;int rc=vnet_receive(output,1514,&n);
        if(rc||i%9973==0){state("wrap",rc);}
        if(rc)break;
        if(i%4096==0){state("wrap-send",vnet_send(packet,60+(i%1400)));}}
    state("wrap-end",vnet_stop());
    fixture();vnet_init();hang=1;receive_packet(0,72,1,0);{uint32_t n=0;state("quarantine",vnet_receive(output,1514,&n));}hang=0;state("quarantine-stop",vnet_stop());
}
/* Paired hosted timing (separate binaries, same model): RX validate+copy+republish, TX send+completion. */
#include <time.h>
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
static int bench(void)
{
    T=fopen("/dev/null","w");fixture();if(vnet_init())return 1;
    for (int s=0;s<9;s++){uint64_t a=ns();
        for (unsigned i=0;i<20000;i++){unsigned id=queues[0].consumed%VNET_QUEUE;struct net_queue *q=&queues[0];
            q->entries[q->used[1]%VNET_QUEUE]=(struct net_used){id,26+64};q->used[1]++;uint32_t n;if(vnet_receive(output,1514,&n))return 1;}
        uint64_t b=ns();for (unsigned i=0;i<20000;i++)if(vnet_send(packet,64))return 1;uint64_t c=ns();
        printf("%d,%.4f,%.4f\n",s,(double)(b-a)/20000,(double)(c-b)/20000);}
    return 0;
}
int main(int argc,char **argv)
{
    if (argc>1 && !strcmp(argv[1],"--bench")) {puts("sample,rx_ns,tx_ns");return bench();}
    if (argc<2) {fprintf(stderr,"usage: %s transcript\n",argv[0]);return 2;}
    T=fopen(argv[1],"w");if(!T)return 2;
    decisions();used();scenarios();
    fprintf(T,"END\n");fclose(T);
    printf("TRANSCRIPT %llu records\n",records);
    return 0;
}
