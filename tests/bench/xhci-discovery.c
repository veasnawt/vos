/* USB.1 review: same exported operations, no tracing/checks inside callbacks.
 * Synthetic ordinary-RAM snapshots, NOT PCI/MMIO or boot latency. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "xhci.h"

int reference_xhci_class_matches(uint32_t);
enum xhci_status reference_xhci_decode_caps(const uint32_t *,size_t,struct xhci_info *);
enum xhci_status reference_xhci_probe(struct pci_addr,const struct xhci_probe_ops *,struct xhci_info *);
__attribute__((noreturn)) void panic(const char *why){fprintf(stderr,"unexpected panic: %s\n",why);abort();}
struct input { uint32_t cfg[6], caps[7]; };
static struct input corpus[8];
static volatile uint64_t sink;
static uint32_t config(void *ctx,struct pci_addr a,unsigned offset){(void)a;return ((struct input *)ctx)->cfg[offset/4];}
static int check(void *ctx,uint64_t base,uint64_t bytes){(void)ctx;(void)base;(void)bytes;return 0;}
static int map(void *ctx,uint64_t base,size_t bytes,const volatile uint32_t **out,size_t *available){
    (void)base;*out=((struct input *)ctx)->caps;*available=bytes;return 0;
}
static uint64_t clock_ns(clockid_t clock){struct timespec t;if(clock_gettime(clock,&t))abort();return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void initialize(void){
    for(unsigned n=0;n<8;n++){
        struct input *i=&corpus[n];i->cfg[0]=0x000d1b36;i->cfg[1]=2;i->cfg[2]=0x0c033001;i->cfg[4]=0xf0000004;
        i->caps[0]=0x1000040;i->caps[1]=(8u<<24)|(16<<8)|64;i->caps[4]=1;i->caps[5]=0x3000;i->caps[6]=0x2000;
    }
    corpus[1].caps[0]=0x1100040;corpus[2].caps[0]=0x1200024;
    corpus[3].cfg[4]=0xf0000000;corpus[4].cfg[0]=0x12348086;
    corpus[5].caps[6]=0x2010;corpus[6].cfg[0]=UINT32_MAX;corpus[7].cfg[2]=0x0c032001;
}
static void equivalent(void){
    for(unsigned n=0;n<8;n++){
        struct xhci_info a={0},b={0};struct xhci_probe_ops o={&corpus[n],36,config,check,map};
        if(xhci_probe((struct pci_addr){0,1,0},&o,&a)!=reference_xhci_probe((struct pci_addr){0,1,0},&o,&b)
           ||a.base!=b.base||a.layout_end!=b.layout_end||strcmp(a.reason,b.reason))abort();
        if(xhci_decode_caps(corpus[n].caps,28,&a)!=reference_xhci_decode_caps(corpus[n].caps,28,&b)
           ||a.layout_end!=b.layout_end||strcmp(a.reason,b.reason))abort();
    }
}
typedef int (*class_fn)(uint32_t);
typedef enum xhci_status (*decode_fn)(const uint32_t *,size_t,struct xhci_info *);
typedef enum xhci_status (*probe_fn)(struct pci_addr,const struct xhci_probe_ops *,struct xhci_info *);
static uint64_t work(const char *operation,int rust,unsigned count,int varied){
    struct xhci_info info={0};struct xhci_probe_ops ops={&corpus[0],36,config,check,map};uint64_t sum=0;
    if(!strcmp(operation,"class")){
        class_fn fn=rust?xhci_class_matches:reference_xhci_class_matches;
        for(unsigned n=0;n<count;n++)sum+=(unsigned)fn(corpus[varied?n&7:0].cfg[2]);
    }else if(!strcmp(operation,"decode")){
        decode_fn fn=rust?xhci_decode_caps:reference_xhci_decode_caps;
        for(unsigned n=0;n<count;n++){sum+=fn(corpus[varied?n&7:0].caps,28,&info);sum+=info.layout_end;}
    }else{
        probe_fn fn=rust?xhci_probe:reference_xhci_probe;
        for(unsigned n=0;n<count;n++){ops.context=&corpus[varied?n&7:0];sum+=fn((struct pci_addr){0,1,0},&ops,&info);sum+=info.base+info.layout_end;}
    }
    sink^=sum;return sum;
}
static void sample(const char *op,int rust,unsigned n,int varied,unsigned index){
    uint64_t wall=clock_ns(CLOCK_MONOTONIC_RAW),cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);
    uint64_t checksum=work(op,rust,n,varied);cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;wall=clock_ns(CLOCK_MONOTONIC_RAW)-wall;
    printf("{\"operation\":\"%s\",\"corpus\":\"%s\",\"implementation\":\"%s\",\"pair\":%u,\"iterations\":%u,\"wall_ns\":%llu,\"cpu_ns\":%llu,\"checksum\":%llu}\n",op,varied?"mixed":"positive",rust?"Rust":"C",index,n,(unsigned long long)wall,(unsigned long long)cpu,(unsigned long long)checksum);
}
int main(void){
    initialize();equivalent();
    for(unsigned varied=0;varied<2;varied++)for(unsigned op=0;op<3;op++){
        const char *name=(const char *[]) {"class","decode","probe"}[op];
        for(unsigned warm=0;warm<3;warm++){work(name,0,50000,varied);work(name,1,50000,varied);}
        for(unsigned pair=0;pair<9;pair++){
            int first=pair&1;sample(name,first,300000,varied,pair);sample(name,!first,300000,varied,pair);
        }
    }
    return 0;
}
