/* The original C reference is compiled under reference_* names, test-only.
 * Compare all defined fields, reason text AND platform operation sequences.
 * Padding/static-string addresses are not semantic output. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "xhci.h"

enum xhci_status reference_xhci_probe(struct pci_addr, const struct xhci_probe_ops *, struct xhci_info *);
enum xhci_status reference_xhci_decode_caps(const uint32_t *, size_t, struct xhci_info *);
int reference_xhci_class_matches(uint32_t);
__attribute__((noreturn)) void panic(const char *why) { fprintf(stderr,"unexpected Rust panic: %s\n",why); abort(); }
static unsigned cases;
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"FAIL case %u line %d: %s\n",cases,__LINE__,#x);exit(1);} } while(0)
struct input { uint32_t cfg[64], caps[7]; int check_error, map_error, invalid; unsigned bits; };
struct trace { struct input *in; unsigned n; uint64_t values[32]; };
static void event(struct trace *t, uint64_t v) { CHECK(t->n < 32); t->values[t->n++]=v; }
static uint32_t config(void *ctx, struct pci_addr a, unsigned off) {
    struct trace *t=ctx; CHECK(off<256 && !(off&3));
    event(t,((uint64_t)a.bus<<32)|((uint64_t)a.dev<<24)|((uint64_t)a.fn<<16)|off);
    return t->in->cfg[off/4];
}
static int check(void *ctx,uint64_t base,uint64_t bytes) {
    struct trace *t=ctx;event(t,0x10000);event(t,base);event(t,bytes);return t->in->check_error;
}
static int map(void *ctx,uint64_t base,size_t bytes,const volatile uint32_t **out,size_t *available) {
    struct trace *t=ctx;event(t,0x20000);event(t,base);event(t,bytes);
    *out=t->in->invalid==1?NULL:t->in->invalid==2?(const void *)((uintptr_t)t->in->caps+1):t->in->caps;
    *available=t->in->invalid==3?27:28;return t->in->map_error;
}
static struct input good(void) {
    struct input i={.bits=36};i.cfg[0]=0x000d1b36;i.cfg[1]=2;i.cfg[2]=0x0c033001;i.cfg[4]=0xf0000004;
    i.caps[0]=0x1000040;i.caps[1]=8u<<24|16<<8|64;i.caps[4]=1;i.caps[5]=0x3000;i.caps[6]=0x2000;return i;
}
static void equal(const struct xhci_info *a,const struct xhci_info *b) {
#define F(f) CHECK(a->f==b->f)
    F(status);F(vendor);F(device);F(command);F(pci_status);F(version);F(class_revision);F(bar_low);F(bar_high);
    F(hcs1);F(hcs2);F(hcs3);F(hcc1);F(doorbell);F(runtime);F(layout_end);F(base);F(caplength);
    F(slots);F(interrupters);F(ports);F(address_bits);F(mapping_error);
#undef F
    CHECK(a->reason && b->reason && !strcmp(a->reason,b->reason));
}
static void differential(struct input i, unsigned interface) {
    struct input before=i;struct trace t={.in=&i},ref={.in=&i};struct xhci_info a,b;
    struct xhci_probe_ops o={&t,i.bits,config,check,map},r={&ref,i.bits,config,check,map};
    if(interface==1)o.config=r.config=NULL;
    if(interface==2)o.check=r.check=NULL;
    if(interface==3)o.map=r.map=NULL;
    enum xhci_status x=xhci_probe((struct pci_addr){0,3,2},interface==4?NULL:&o,&a);
    enum xhci_status y=reference_xhci_probe((struct pci_addr){0,3,2},interface==4?NULL:&r,&b);
    cases++;CHECK(x==y);equal(&a,&b);CHECK(t.n==ref.n && !memcmp(t.values,ref.values,t.n*sizeof(uint64_t)));
    CHECK(!memcmp(&before,&i,sizeof(i)));
}
static uint32_t random32(uint32_t *seed) { *seed=*seed*1664525u+1013904223u;return *seed; }
static void tests(void) {
    for(unsigned n=0;n<5;n++)differential(good(),n);
    struct input i=good();
    for(unsigned width=0;width<66;width++){i=good();i.bits=width;differential(i,0);}
    for(unsigned n=0;n<4;n++){i=good();i.invalid=(int)n;differential(i,0);}
    for(int n=-8;n<=8;n++){i=good();i.check_error=n;differential(i,0);i=good();i.map_error=n;differential(i,0);}
    uint32_t seed=1;
    for(unsigned n=0;n<20000;n++){
        i=good();
        switch(n%7){
        case 0:for(unsigned k=0;k<7;k++)i.caps[k]=random32(&seed);break;
        case 1:i.caps[(n/7)%7]=random32(&seed);break;
        case 2:i.cfg[(unsigned[]){0,1,2,3,4,5}[n%6]]=random32(&seed);break;
        case 3:i.cfg[4]=random32(&seed)&~0xfffu;i.cfg[5]=random32(&seed)&0x3ffff;break;
        case 4:i.caps[0]=((unsigned[]){0x100,0x110,0x120}[n%3]<<16)|(0x20+(n%56)*4);break;
        case 5:i.caps[5]=0x400+(n%2048)*4;i.caps[6]=0x1000+(n%128)*32;break;
        default:i.cfg[4]=(unsigned[]){0,1,2,3,4,6,0xfffff000,0xfffffff4}[n%8];i.cfg[5]=n%4;break;
        }
        differential(i,0);
        struct xhci_info a={.base=1234,.layout_end=5678,.vendor=42},b=a;
        size_t len=n%33;
        CHECK(xhci_decode_caps(i.caps,len,&a)==reference_xhci_decode_caps(i.caps,len,&b));equal(&a,&b);cases++;
        CHECK(xhci_class_matches(seed)==reference_xhci_class_matches(seed));
    }
    struct xhci_info a={0},b={0};CHECK(xhci_decode_caps(NULL,28,&a)==reference_xhci_decode_caps(NULL,28,&b));equal(&a,&b);cases++;
    printf("xhci differential: %u probe/decode cases PASS; fields, reasons, operation traces, unchanged inputs\n",cases);
}
static uint64_t now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC_RAW,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t cpu_now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
struct timing { uint64_t wall, cpu; };
static volatile uint64_t sink;
static struct timing bench(int rust,unsigned iterations) {
    struct input i=good();struct trace t={.in=&i};struct xhci_probe_ops o={&t,36,config,check,map};
    struct xhci_info info;uint64_t start=now(),cpu_start=cpu_now();
    for(unsigned n=0;n<iterations;n++){t.n=0;enum xhci_status s=rust?xhci_probe((struct pci_addr){0,3,2},&o,&info):reference_xhci_probe((struct pci_addr){0,3,2},&o,&info);sink+=info.base+s+t.n;}
    uint64_t cpu=cpu_now()-cpu_start;return (struct timing){now()-start,cpu};
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"--bench")) {
        unsigned iterations=200000;printf("{\"iterations\":%u,\"samples\":[",iterations);
        for(unsigned n=0;n<7;n++){struct timing c,r;if(n&1){r=bench(1,iterations);c=bench(0,iterations);}else{c=bench(0,iterations);r=bench(1,iterations);}
            printf("%s{\"C_ns\":%llu,\"Rust_ns\":%llu,\"C_cpu_ns\":%llu,\"Rust_cpu_ns\":%llu}",n?",":"",(unsigned long long)c.wall,(unsigned long long)r.wall,(unsigned long long)c.cpu,(unsigned long long)r.cpu);}
        printf("],\"sink\":%llu}\n",(unsigned long long)sink);
    } else tests();
    return 0;
}
