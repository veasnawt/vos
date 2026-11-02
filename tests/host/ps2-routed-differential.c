#define _GNU_SOURCE
#include "keyboard.h"
#include "rust_kernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stddef.h>
_Static_assert(sizeof(struct keyboard)==88,"keyboard ABI");
_Static_assert(offsetof(struct keyboard,decoder)==8 && offsetof(struct keyboard,pointer)==28 && offsetof(struct keyboard,packet)==34,"decoder ABI");
_Static_assert(offsetof(struct keyboard,key_head)==44 && offsetof(struct keyboard,key_count)==48 && offsetof(struct keyboard,keys)==52,"queue ABI");
extern void routed_drain_reference(struct keyboard *);
extern char keyboard_poll_reference(struct keyboard *);
extern int keyboard_pointer_poll_reference(struct keyboard *,struct pointer_packet *);
extern unsigned keyboard_pointer_lost_reference(struct keyboard *);
static struct keyboard *active;
struct cell {uint8_t status,byte;};static struct cell script[96];
static unsigned at,reads,profile;static uint32_t trace[256];static int bench;
static void die(const char *s){fprintf(stderr,"FAIL %s at=%u reads=%u profile=%u\n",s,at,reads,profile);exit(1);}
static uint8_t input(uint16_t port);static uint8_t alternate(uint16_t port);
static void forbidden(uint16_t p,uint8_t b){(void)p;(void)b;die("unexpected controller write");}
static const struct keyboard_io io={input,forbidden},alt={alternate,forbidden};
static uint8_t read_port(uint16_t port,int other){
 if(port!=0x64 && port!=0x60)die("unexpected read port");
 if(at>=96 || reads>=256)die("read bound");
 uint8_t value=port==0x64?script[at].status:script[at++].byte;
 if(!bench)trace[reads]=((uint32_t)other<<24)|((uint32_t)port<<8)|value;
 reads++;
 /* Legal synchronous callback-visible changes, with no escaping reference. */
 if(profile && port==0x64 && reads==1){
  if(profile&1){active->key_head=31;active->key_count=2;}
  if(profile&2)active->pointer_pending=1;
  if(profile&4)active->decoder.shift=1;
  if(profile&8)active->io=&alt;
 }
 if(profile&16 && port==0x60){active->key_head=(active->key_head+3)%32;active->pointer_lost=UINT32_MAX;}
 return value;
}
static uint8_t input(uint16_t p){return read_port(p,0);}static uint8_t alternate(uint16_t p){return read_port(p,1);}
static uint64_t rng=UINT64_C(0x7232647261696e31),cases;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)rng;}
static struct keyboard seed(void){struct keyboard k;memset(&k,0xa5,sizeof k);k.io=&io;k.ready=1;k.pointer_ready=1;k.pointer_pending=0;k.pointer_lost=next();k.key_head=next()%32;k.key_count=next()%33;uint8_t *d=(void*)&k.decoder;for(unsigned i=0;i<7;i++)d[i]=(uint8_t)next();d=(void*)&k.pointer;for(unsigned i=0;i<5;i++)d[i]=(uint8_t)next();return k;}
static void reset(struct keyboard *k){active=k;at=reads=0;memset(trace,0,sizeof trace);}
static void compare(struct keyboard initial,unsigned mode){
 struct keyboard a=initial,b=initial;struct pointer_packet pa,pb;memset(&pa,0xa5,sizeof pa);memset(&pb,0xa5,sizeof pb);
 reset(&a);int ra=0,rb=0;
 if(mode==0)routed_drain_reference(&a);else if(mode==1)ra=keyboard_poll_reference(&a);else ra=keyboard_pointer_poll_reference(&a,&pa);
 unsigned na=at,nr=reads;uint32_t ta[256];memcpy(ta,trace,sizeof ta);
 reset(&b);if(mode==0)vos_rust_keyboard_routed_drain(&b);else if(mode==1)rb=keyboard_poll(&b);else rb=keyboard_pointer_poll(&b,&pb);
 if(memcmp(&a,&b,sizeof a)||ra!=rb||memcmp(&pa,&pb,sizeof pa)||na!=at||nr!=reads||memcmp(ta,trace,sizeof ta))die("C/Rust state/trace equivalence");
 if(mode==2){unsigned la=keyboard_pointer_lost_reference(&a),lb=keyboard_pointer_lost(&b);if(la!=lb||a.pointer_lost!=b.pointer_lost)die("pointer_lost equivalence");}
 cases++;
}
static uint64_t ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))die("clock");return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static volatile uint64_t sink;
static void workload(unsigned n,struct keyboard *k){
 memset(k,0,sizeof *k);k->io=&io;k->ready=k->pointer_ready=1;memset(script,0,sizeof script);profile=0;
 if(n==1)script[0]=(struct cell){1,0x1e};
 if(n==2){script[0]=(struct cell){0x21,8};script[1]=(struct cell){0x21,3};script[2]=(struct cell){0x21,2};}
 if(n==3)script[0]=(struct cell){0x21,8};
 if(n==4){script[0]=(struct cell){1,0x1e};script[1]=(struct cell){0x21,8};script[2]=(struct cell){1,0x30};script[3]=(struct cell){0x21,3};script[4]=(struct cell){0x21,2};}
 if(n==5)k->key_count=32;
 if(n==6)for(unsigned j=0;j<32;j++)script[j]=(struct cell){0x61,0xff};
 if(n==7)for(unsigned j=0;j<32;j++)script[j]=(struct cell){1,0x1e};
 if(n==8){k->pointer_pending=1;k->packet=(struct pointer_packet){10,-20,1};}
 if(n==9){k->pointer.used=1;k->pointer.idle=15;}
 if(n==10){k->pointer_lost=12345;}
}
static double timing(unsigned n,int rust){
 struct keyboard start,k;struct pointer_packet p;workload(n,&start);uint64_t a=ns();
 for(unsigned i=0;i<20000;i++){
  k=start;active=&k;at=reads=0;
  if(n<8){
   if(rust)vos_rust_keyboard_routed_drain(&k);else routed_drain_reference(&k);
  } else if(n==8 || n==9){
   int r;
   if(rust)r=keyboard_pointer_poll(&k,&p);else r=keyboard_pointer_poll_reference(&k,&p);
   sink+=r+p.dx;
  } else {
   unsigned l;
   if(rust)l=keyboard_pointer_lost(&k);else l=keyboard_pointer_lost_reference(&k);
   sink+=l;
  }
  sink+=k.key_count+k.pointer_lost+k.pointer_pending+at+reads;
 }
 return (double)(ns()-a)/20000;
}
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"--bench")){bench=1;puts("workload,sample,C_ns,Rust_ns");for(unsigned n=0;n<11;n++){(void)timing(n,0);(void)timing(n,1);for(unsigned s=0;s<9;s++){double a,b;if(s&1){b=timing(n,1);a=timing(n,0);}else{a=timing(n,0);b=timing(n,1);}printf("%u,%u,%.4f,%.4f\n",n,s,a,b);}}return 0;}
 struct keyboard k=seed();k.key_count=0;memset(&k.decoder,0,sizeof k.decoder);memset(&k.pointer,0,sizeof k.pointer);
 for(unsigned s=0;s<256;s++)for(unsigned b=0;b<256;b++){memset(script,0,sizeof script);script[0]=(struct cell){s,b};profile=0;compare(k,0);}
 for(unsigned p=0;p<32;p++)for(unsigned mode=0;mode<3;mode++){profile=p;memset(script,0,sizeof script);for(unsigned i=0;i<64;i++)script[i]=(struct cell){(i&1)?0x21:1,(i%3==0)?8:0x1e};k=seed();k.key_count=0;memset(&k.pointer,0,sizeof k.pointer);compare(k,mode);}
 for(unsigned i=0;i<40000;i++){for(unsigned j=0;j<96;j++)script[j]=(struct cell){(uint8_t)next(),(uint8_t)next()};k=seed();if(i%7==0)k.pointer_pending=(int)next();if(i%11==0)k.ready=0;if(i%13==0)k.pointer_ready=0;profile=i%32;for(unsigned mode=0;mode<3;mode++)compare(k,mode);}
 /* Defined unsigned wrapping is exercised only through direct drain, never an
    invalid C dequeue with an out-of-range head. */
 k=seed();k.key_head=UINT32_MAX;k.key_count=UINT32_MAX;k.pointer_pending=0;profile=0;memset(script,0,sizeof script);script[0]=(struct cell){1,0x1e};memset(&k.decoder,0,sizeof k.decoder);compare(k,0);
 for(unsigned i=0;i<4;i++){k=seed();k.key_count=0;k.pointer_lost=UINT32_MAX-i;k.pointer_pending=0;profile=0;for(unsigned j=0;j<96;j++)script[j]=(struct cell){0x61,0};compare(k,0);}
 long page=sysconf(_SC_PAGESIZE);unsigned char *m=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(m==MAP_FAILED || mprotect(m+page,page,PROT_READ|PROT_WRITE))die("guard pages");
 struct keyboard *edge=(void*)(m+2*page-sizeof *edge);*edge=k;edge->key_count=0;memset(script,0,sizeof script);active=edge;profile=0;at=reads=0;vos_rust_keyboard_routed_drain(edge);if(munmap(m,page*3))die("unmap");
 vos_rust_keyboard_routed_drain(NULL);
 printf("PASS %llu routed driver comparisons; seed=7232647261696e31; callback mutations, exact read trace, boundary object; no controller writes\n",(unsigned long long)cases);return 0;
}
