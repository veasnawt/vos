/* Production bare-metal Rust ABI against the unchanged C reference. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pointer.h"
int pointer_decode_reference(struct pointer_decoder *,uint8_t,struct pointer_packet *);
int pointer_idle_reference(struct pointer_decoder *);
_Static_assert(sizeof(struct pointer_decoder)==5 && _Alignof(struct pointer_decoder)==1,"decoder ABI");
_Static_assert(offsetof(struct pointer_decoder,used)==3 && offsetof(struct pointer_decoder,idle)==4,"state ABI");
_Static_assert(sizeof(struct pointer_packet)==6 && _Alignof(struct pointer_packet)==2,"packet ABI");
_Static_assert(offsetof(struct pointer_packet,dx)==0 && offsetof(struct pointer_packet,dy)==2 && offsetof(struct pointer_packet,buttons)==4,"output ABI");
static uint64_t calls;
static void feed(struct pointer_decoder *a,struct pointer_decoder *b,uint8_t v,
                 struct pointer_packet *x,struct pointer_packet *y) {
    int r=pointer_decode(a,v,x),s=pointer_decode_reference(b,v,y);
    assert(r==s && !memcmp(a,b,5) && !memcmp(x,y,6)); calls++;
    if(r==1) {
        assert(x->dx==(int)a->bytes[1]-((a->bytes[0]&16)?256:0));
        assert(x->dy==(int)a->bytes[2]-((a->bytes[0]&32)?256:0));
        assert(x->buttons==(a->bytes[0]&1));
    }
}
static void differential(void) {
    for(unsigned h=0;h<256;h++)for(unsigned u=0;u<256;u++)for(unsigned v=0;v<256;v++) {
        struct pointer_decoder a={0},b={0};struct pointer_packet x,y;
        memset(&x,0xa5,6);memset(&y,0xa5,6);
        feed(&a,&b,h,&x,&y);feed(&a,&b,u,&x,&y);feed(&a,&b,v,&x,&y);
    }
    uint32_t seed=0x235781ae;
    struct pointer_decoder a={0},b={0};struct pointer_packet x,y;
    memset(&x,0xa5,6);memset(&y,0xa5,6);
    for(unsigned i=0;i<1000000;i++) {
        seed=seed*1664525u+1013904223u;
        if((seed&7)==0) {
            unsigned n=(seed>>8)%20;
            for(unsigned j=0;j<n;j++){assert(pointer_idle(&a)==pointer_idle_reference(&b));assert(!memcmp(&a,&b,5));calls++;}
        } else feed(&a,&b,seed>>24,&x,&y);
    }
    for(unsigned used=1;used<=2;used++)for(unsigned idle=0;idle<16;idle++) {
        a=(struct pointer_decoder){{8,255,255},used,idle};b=a;
        for(unsigned i=0;i<17;i++){assert(pointer_idle(&a)==pointer_idle_reference(&b));assert(!memcmp(&a,&b,5));calls++;}
    }
    /* Invalid internal states are outside the C reference's safe contract. */
    for(unsigned used=3;used<256;used++) {
        a=(struct pointer_decoder){{8,1,2},used,0};memset(&x,0xa5,6);y=x;
        assert(pointer_decode(&a,8,&x)==-1 && !a.used && !a.idle && !memcmp(&x,&y,6));
        a.used=used;assert(pointer_idle(&a)==1 && !a.used && !a.idle);
    }
    for(unsigned idle=16;idle<256;idle++) {
        a=(struct pointer_decoder){{8,1,2},1,idle};assert(pointer_decode(&a,8,&x)==-1 && !a.used && !a.idle);
    }
    a=(struct pointer_decoder){0};
    assert(pointer_decode(NULL,8,&x)==-1 && pointer_idle(NULL)==0);
    assert(pointer_decode(&a,8,NULL)==-1 && !a.used);
    _Alignas(2) unsigned char arena[16]={0};
    assert(pointer_decode(&a,8,(void *)(arena+1))==-1 && !a.used);
    assert(pointer_decode((void *)arena,8,(void *)arena)==-1);
    assert(pointer_decode((void *)(UINTPTR_MAX-2),8,&x)==-1);
    assert(pointer_decode(&a,8,(void *)(UINTPTR_MAX-1))==-1);
    puts("invalid-state/null/alignment/overlap/overflow checks PASS");
    printf("differential PASS: 16777216 triplets; %llu byte/idle comparisons; C ABI 5/6 bytes\n",(unsigned long long)calls);
}
typedef int (*decode_fn)(struct pointer_decoder *,uint8_t,struct pointer_packet *);
typedef int (*idle_fn)(struct pointer_decoder *);
static volatile uint64_t sink;
static void bench(decode_fn f,idle_fn idle,const char *name,unsigned mode) {
    struct pointer_decoder d={0};struct pointer_packet p={0};
    const unsigned n=6000000;uint64_t sum=0;struct timespec a,b;
    clock_gettime(CLOCK_MONOTONIC,&a);
    for(unsigned i=0;i<n;i++) {
        uint8_t byte=mode==1?0:(i%3==0?0x39:(uint8_t)i);
        sum+=(uint32_t)f(&d,byte,&p);sum+=(uint16_t)p.dx;
        if(mode==2)sum+=(unsigned)idle(&d);
    }
    clock_gettime(CLOCK_MONOTONIC,&b);sink=sum;
    double ns=(b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec);
    printf("bench,%s,%u,%.4f,%llu\n",name,mode,ns/n,(unsigned long long)sum);
}
int main(int argc,char **argv) {
    (void)argv;
    if(argc>1){for(unsigned i=0;i<7;i++)for(unsigned mode=0;mode<3;mode++) {
        bench(pointer_decode_reference,pointer_idle_reference,"C",mode);
        bench(pointer_decode,pointer_idle,"Rust",mode);
    }}else differential();
    return 0;
}
