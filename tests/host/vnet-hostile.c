/* R3-N3-S: deterministic hostile-device regression for the virtio-net RX used
 * ring. The device model of tests/host/vnet-differential.c runs the driver
 * selected by VNET_DRIVER (the frozen vulnerable f3683869 driver, or the
 * corrected production driver). For one vnet_receive() call the RX ring page is
 * made inaccessible; every driver access to it faults, the handler opens the
 * page and single-steps exactly that instruction (x86 trap flag), then closes
 * it again. This counts the driver's reads of the head used entry, and right
 * after the first read of its `len` (the end of validation) the hostile device
 * rewrites the entry's id and/or len, as a device can at any time through DMA.
 * Each case runs in a forked child; a memory violation is reported, not fatal. */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#define main vnet_differential_main
#include "vnet-differential.c"
#undef main

static uintptr_t page, fault_addr;
static volatile struct net_used *head;
static int armed, fired, id_reads, len_reads, mutate_id, mutate_len;
static uint32_t hostile_id, hostile_len;
static int dup_extra, bad_header, zero_count;   /* fixture variants: extra completions reuse the head id; nonzero header flags */
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;uintptr_t a=(uintptr_t)si->si_addr;
    if (armed && a>=page && a<page+4096) {
        mprotect((void *)page,4096,PROT_READ|PROT_WRITE);fault_addr=a;
        ((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EFL]|=0x100;   /* single-step this one access */
        return;
    }
    static const char m[]="HOSTILE: WILD MEMORY ACCESS\n";(void)!write(2,m,sizeof m-1);_exit(42);
}
static void on_trap(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;(void)si;((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EFL]&=~0x100;
    if (fault_addr==(uintptr_t)&head->id) id_reads++;
    if (fault_addr==(uintptr_t)&head->len) {
        len_reads++;
        if (len_reads==1 && !fired) {fired=1;if (mutate_id) head->id=hostile_id;if (mutate_len) head->len=hostile_len;}
    }
    fault_addr=0;mprotect((void *)page,4096,PROT_NONE);
}
static void handlers(void)
{
    struct sigaction s;memset(&s,0,sizeof s);s.sa_flags=SA_SIGINFO|SA_NODEFER;
    s.sa_sigaction=on_segv;sigaction(SIGSEGV,&s,0);s.sa_sigaction=on_trap;sigaction(SIGTRAP,&s,0);
}
/* One RX completion of a valid packet in descriptor `id`, then the hostile rewrite. */
static void rx_case(const char *name,uint16_t consumed,unsigned id,uint32_t len,unsigned extra,int mi,uint32_t hid,int ml,uint32_t hlen,uint32_t capacity)
{
    fflush(stdout);
    pid_t pid=fork();
    if (!pid) {
        T=fopen("/dev/null","w");fixture();if (vnet_init()) _exit(5);
        struct net_queue *q=&queues[0];
        q->consumed=consumed;q->used[1]=consumed;q->published=(uint16_t)(consumed+VNET_QUEUE);   /* ring positioned at the wrap */
        for (unsigned d=0;d<VNET_QUEUE;d++) {uint8_t *p=host_alias(net.pool[RX_BASE+d]);memset(p,0,PAGE_SIZE);memset(p+VNET_HEADER,0x40+d,VNET_FRAME_MAX);}
        if (!zero_count) {q->entries[q->used[1]%VNET_QUEUE]=(struct net_used){id,len};q->used[1]++;}
        for (unsigned k=0;k<extra;k++){q->entries[q->used[1]%VNET_QUEUE]=(struct net_used){dup_extra?id:(id+1+k)%VNET_QUEUE,72};q->used[1]++;}
        if (bad_header && id<VNET_QUEUE) ((uint8_t *)host_alias(net.pool[RX_BASE+id]))[0]=1;
        head=&q->entries[consumed%VNET_QUEUE];page=(uintptr_t)host_alias(net.pool[0]);
        mutate_id=mi;hostile_id=hid;mutate_len=ml;hostile_len=hlen;handlers();
        uint32_t n=0xabcdef;memset(output,0xa5,sizeof output);
        armed=1;mprotect((void *)page,4096,PROT_NONE);
        int rc=vnet_receive(output,capacity,&n);
        mprotect((void *)page,4096,PROT_READ|PROT_WRITE);armed=0;
        unsigned h=2166136261u;for (size_t i=0;i<sizeof output;i++) h=(h^output[i])*16777619u;
        printf("case %s rc=%d n=%u out=%08x first=%02x id_reads=%d len_reads=%d fired=%d state=%d consumed=%u published=%u\n",
               name,rc,n,h,output[0],id_reads,len_reads,fired,net.state,queues[0].consumed,queues[0].published);
        fflush(stdout);_exit(0);
    }
    int st=0;waitpid(pid,&st,0);
    if (WIFEXITED(st) && WEXITSTATUS(st)==0) return;
    printf("case %s VIOLATION %s %d\n",name,WIFSIGNALED(st)?"signal":"exit",WIFSIGNALED(st)?WTERMSIG(st):WEXITSTATUS(st));
}
/* TX: the driver never reads TX used values after validation; a rewrite must not matter. */
static void tx_case(const char *name,uint32_t hid,uint32_t hlen)
{
    fflush(stdout);
    pid_t pid=fork();
    if (!pid) {
        T=fopen("/dev/null","w");fixture();if (vnet_init()) _exit(5);
        int rc=vnet_send(packet,60);struct net_queue *q=&queues[1];
        q->entries[(uint16_t)(q->consumed-1)%VNET_QUEUE]=(struct net_used){hid,hlen};   /* rewrite the consumed completion */
        int rc2=vnet_send(packet,64);
        printf("case %s rc=%d rc2=%d state=%d txc=%u\n",name,rc,rc2,net.state,queues[1].consumed);fflush(stdout);_exit(0);
    }
    int st=0;waitpid(pid,&st,0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st)==0)) printf("case %s VIOLATION %d\n",name,st);
}
int main(void)
{
    static const uint16_t cons[3]={0,5,65535};
    static const uint32_t bad_ids[6]={8,9,10,0x10000,0x7fffffffu,0xffffffffu};
    static const uint32_t bad_lens[7]={0,11,25,1527,5000,0x7fffffffu,0xffffffffu};
    char name[96];
    for (unsigned c=0;c<3;c++)for(unsigned id=0;id<VNET_QUEUE;id++){
        snprintf(name,sizeof name,"c%u-id%u-honest",cons[c],id);rx_case(name,cons[c],id,72,0,0,0,0,0,1514);
        for (unsigned b=0;b<6;b++){snprintf(name,sizeof name,"c%u-id%u-to-id%u",cons[c],id,bad_ids[b]);rx_case(name,cons[c],id,72,0,1,bad_ids[b],0,0,1514);}
        snprintf(name,sizeof name,"c%u-id%u-to-other",cons[c],id);rx_case(name,cons[c],id,72,0,1,(id+3)%VNET_QUEUE,0,0,1514);
        for (unsigned l=0;l<7;l++){snprintf(name,sizeof name,"c%u-id%u-len-to-%u",cons[c],id,bad_lens[l]);rx_case(name,cons[c],id,72,0,0,0,1,bad_lens[l],1514);}
        snprintf(name,sizeof name,"c%u-id%u-len-to-40",cons[c],id);rx_case(name,cons[c],id,72,0,0,0,1,40,1514);
        snprintf(name,sizeof name,"c%u-id%u-both",cons[c],id);rx_case(name,cons[c],id,72,0,1,0xffffffffu,1,0xffffffffu,1514);
    }
    /* multiple completions, short capacity (EMSGSIZE leaves the packet queued), and paths that never use the head values */
    for (unsigned id=0;id<VNET_QUEUE;id++){
        snprintf(name,sizeof name,"multi3-id%u-to-id9",id);rx_case(name,7,id,100,2,1,9,0,0,1514);
        snprintf(name,sizeof name,"cap8-id%u-to-id9",id);rx_case(name,0,id,72,0,1,9,0,0,8);
        snprintf(name,sizeof name,"cap60-id%u-len-to-1000",id);rx_case(name,0,id,72,0,0,0,1,1000,60);
    }
    rx_case("invalid-initial-id9",0,9,72,0,1,0,0,0,1514);
    rx_case("invalid-initial-len",0,1,25,0,0,0,1,72,1514);
    dup_extra=1;rx_case("duplicate-ids",0,2,72,1,0,0,0,0,1514);dup_extra=0;
    bad_header=1;rx_case("bad-header",0,4,72,0,0,0,0,0,1514);bad_header=0;
    zero_count=1;rx_case("zero-count",0,0,72,0,1,9,1,0,1514);zero_count=0;
    tx_case("tx-rewrite-id9",9,0);tx_case("tx-rewrite-len",0,72);tx_case("tx-honest",0,0);
    printf("HOSTILE DONE\n");
    return 0;
}
