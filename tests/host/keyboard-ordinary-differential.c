#define _GNU_SOURCE
#include "keyboard.h"
#include "rust_kernel.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
/* R2 ordinary keyboard_poll: frozen accepted C oracle versus the C dispatcher plus
 * the Rust-owned ordinary loop, with exact port-read traces and full object state. */
_Static_assert(sizeof(struct keyboard)==88,"keyboard ABI");
_Static_assert(offsetof(struct keyboard,decoder)==8 && offsetof(struct keyboard,ready)==16 &&
               offsetof(struct keyboard,pointer_ready)==20 && offsetof(struct keyboard,keys)==52,"layout");
extern char keyboard_poll_reference(struct keyboard *);
static struct keyboard *active;
struct cell {uint8_t status,byte;};
static struct cell script[96];
static unsigned at,reads,profile,trigger;static uint32_t trace[256];static int bench;
static uint64_t cases;
static void die(const char *s){fprintf(stderr,"FAIL %s at=%u reads=%u profile=%u trigger=%u\n",s,at,reads,profile,trigger);exit(1);}
static uint8_t input(uint16_t port);static uint8_t alternate(uint16_t port);
static void forbidden(uint16_t p,uint8_t b){(void)p;(void)b;die("unexpected controller write");}
static const struct keyboard_io io={input,forbidden},alt={alternate,forbidden};
static uint8_t read_port(uint16_t port,int other){
 if(port!=0x64 && port!=0x60)die("unexpected read port");
 if(at>=96 || reads>=256)die("read bound");
 uint8_t value=port==0x64?script[at].status:script[at++].byte;
 if(!bench)trace[reads]=((uint32_t)other<<24)|((uint32_t)port<<8)|value;
 reads++;
 /* Legal synchronous callback-visible changes; no reference escapes the call. */
 if(profile && reads==trigger){
  if(profile&1)active->decoder.shift^=1;
  if(profile&2)active->ready=0;            /* ready is read once, before the loop */
  if(profile&4)active->io=&alt;            /* next read must reload io->in */
  if(profile&8)active->pointer_ready=1;    /* mode was already selected */
  if(profile&16)memset(&active->decoder,0x5a,sizeof active->decoder);
  if(profile&32){active->key_count=7;active->keys[3]='q';active->pointer_lost=UINT32_MAX;}
 }
 if(profile&64 && port==0x60)active->decoder.extended=1;
 return value;
}
static uint8_t input(uint16_t p){return read_port(p,0);}
static uint8_t alternate(uint16_t p){return read_port(p,1);}
static uint64_t rng=UINT64_C(0x72326f7264696e31);
static uint32_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)rng;}
static struct keyboard base(void){struct keyboard k;memset(&k,0,sizeof k);k.io=&io;k.ready=1;return k;}
static struct keyboard seeded(void){
 struct keyboard k;memset(&k,0xa5,sizeof k);k.io=&io;k.ready=1;k.pointer_ready=0;k.pointer_pending=(int)(next()&1);
 k.pointer_lost=next();k.key_head=next()%32;k.key_count=next()%33;
 uint8_t *d=(void*)&k.decoder;for(unsigned i=0;i<7;i++)d[i]=(uint8_t)(next()&1);
 d=(void*)&k.pointer;for(unsigned i=0;i<5;i++)d[i]=(uint8_t)next();
 for(unsigned i=0;i<32;i++)k.keys[i]=(char)next();
 return k;
}
static void reset(struct keyboard *k){active=k;at=reads=0;memset(trace,0,sizeof trace);}
/* which: 0 public dispatcher, 1 direct Rust ordinary entry (only with pointer_ready==0). */
static void compare(struct keyboard initial,int which){
 if(which && initial.pointer_ready)die("direct entry requires ordinary mode");
 struct keyboard a=initial,b=initial;
 reset(&a);char ra=keyboard_poll_reference(&a);unsigned na=at,nr=reads;uint32_t ta[256];memcpy(ta,trace,sizeof ta);
 reset(&b);char rb=which?vos_rust_keyboard_ordinary_poll(&b):keyboard_poll(&b);
 if(memcmp(&a,&b,sizeof a)||ra!=rb||na!=at||nr!=reads||memcmp(ta,trace,sizeof ta))die("C/Rust state/trace equivalence");
 cases++;
}
static uint64_t ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))die("clock");return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static volatile uint64_t sink;
static void workload(unsigned n,struct keyboard *k){
 *k=base();memset(script,0,sizeof script);profile=0;
 if(n==0)k->ready=0;                                                        /* unready */
 if(n==2)script[0]=(struct cell){1,0x1e};                                   /* one key */
 if(n==3){script[0]=(struct cell){0x21,8};script[1]=(struct cell){1,0x1e};} /* aux then key */
 if(n==4){script[0]=(struct cell){0x41,7};script[1]=(struct cell){1,0x1e};} /* error reset then key */
 if(n==5)for(unsigned j=0;j<33;j++)script[j]=(struct cell){1,0x9e};        /* full bound, releases */
 if(n==6){script[0]=(struct cell){1,0x2a};script[1]=(struct cell){1,0x1e};} /* shifted key */
 if(n==7)k->pointer_ready=1;                                                /* routed control */
}
static double timing(unsigned n,int rust){
 struct keyboard start,k;workload(n,&start);uint64_t a=ns();
 for(unsigned i=0;i<20000;i++){k=start;active=&k;at=reads=0;char c=rust?keyboard_poll(&k):keyboard_poll_reference(&k);sink+=(unsigned char)c+k.decoder.shift+at+reads;}
 return (double)(ns()-a)/20000;
}
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"--bench")){
  bench=1;puts("workload,sample,C_ns,Rust_ns");
  for(unsigned n=0;n<8;n++){(void)timing(n,0);(void)timing(n,1);
   for(unsigned s=0;s<9;s++){double a,b;if(s&1){b=timing(n,1);a=timing(n,0);}else{a=timing(n,0);b=timing(n,1);}printf("%u,%u,%.4f,%.4f\n",n,s,a,b);}}
  return 0;
 }
 struct keyboard k;
 /* 1. Every first (status,data) cell; OBF-clear terminator follows. */
 for(unsigned s=0;s<256;s++)for(unsigned b=0;b<256;b++){k=base();memset(script,0,sizeof script);script[0]=(struct cell){(uint8_t)s,(uint8_t)b};profile=0;compare(k,0);compare(k,1);}
 /* 2. Every decoder flag state (7 bytes, 0/1) times every scan code. */
 for(unsigned m=0;m<128;m++)for(unsigned sc=0;sc<256;sc++){k=base();uint8_t *d=(void*)&k.decoder;for(unsigned i=0;i<7;i++){d[i]=(uint8_t)((m>>i)&1);}
  memset(script,0,sizeof script);script[0]=(struct cell){1,(uint8_t)sc};profile=0;compare(k,0);}
 /* 3. Every two-byte sequence (prefixes, modifiers, releases, resets). */
 for(unsigned x=0;x<256;x++)for(unsigned y=0;y<256;y++){k=base();memset(script,0,sizeof script);script[0]=(struct cell){1,(uint8_t)x};script[1]=(struct cell){1,(uint8_t)y};profile=0;compare(k,0);}
 /* 4. ready value classes. */
 const int rv[5]={0,1,-1,2,INT_MIN};
 for(unsigned i=0;i<5;i++){k=base();k.ready=rv[i];memset(script,0,sizeof script);script[0]=(struct cell){1,0x1e};profile=0;compare(k,0);compare(k,1);}
 /* 5. Exact bound: 32 non-producing pairs then a producing 33rd byte. */
 const struct cell fill[4]={{1,0x9e},{0x21,0x1e},{0x41,0x1e},{1,0}};
 for(unsigned f=0;f<4;f++){k=base();for(unsigned j=0;j<96;j++)script[j]=fill[f];script[32]=(struct cell){1,0x1e};profile=0;compare(k,0);compare(k,1);
  reset(&k);if(vos_rust_keyboard_ordinary_poll(&k)!=0 || reads!=64 || at!=32)die("exact 32-iteration/64-read bound");}
 /* 6. Callback-visible mutations at the first, second and third status reads. */
 for(unsigned p=0;p<128;p++)for(unsigned t=1;t<=5;t+=2){profile=p;trigger=t;memset(script,0,sizeof script);
  for(unsigned i=0;i<64;i++){script[i]=(struct cell){(uint8_t)((i&1)?0x21:1),(uint8_t)((i%3==0)?0x2a:(i%3==1)?0x9e:0x1e)};}k=seeded();compare(k,0);}
 /* 7. Deterministic random objects/scripts/profiles; ordinary and routed control. */
 for(unsigned i=0;i<40000;i++){for(unsigned j=0;j<96;j++){script[j]=(struct cell){(uint8_t)next(),(uint8_t)next()};}
  k=seeded();if(i%5==0)k.pointer_ready=1;if(i%11==0)k.ready=(int)next();profile=i%128;trigger=1+2*(i%3);
  compare(k,0);if(!k.pointer_ready)compare(k,1);}
 /* 8. Boundary objects: NULL/high/misaligned refused without reads; guard-page edge. */
 profile=0;memset(script,0,sizeof script);script[0]=(struct cell){1,0x1e};reads=0;
 if(vos_rust_keyboard_ordinary_poll(NULL)!=0 || vos_rust_keyboard_ordinary_poll((void*)(UINTPTR_MAX-7))!=0)die("invalid address accepted");
 k=base();if(vos_rust_keyboard_ordinary_poll((void*)((char*)&k+1))!=0 || reads!=0)die("misaligned address read");
 long page=sysconf(_SC_PAGESIZE);unsigned char *mp=mmap(0,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(mp==MAP_FAILED || mprotect(mp+page,(size_t)page,PROT_READ|PROT_WRITE))die("guard pages");
 struct keyboard *edge=(void*)(mp+2*page-sizeof *edge);*edge=base();active=edge;at=reads=0;
 if(vos_rust_keyboard_ordinary_poll(edge)!='a' || reads!=2)die("guard-page edge object");
 if(munmap(mp,(size_t)page*3))die("unmap");
 /* Coverage: 131072 + 32768 + 65536 + 10 + 8 + 384 + 72000 = 301778 comparisons. */
 if(cases!=301778)die("comparison coverage count");
 printf("PASS %llu ordinary keyboard_poll comparisons; seed=72326f7264696e31; exhaustive cells/decoder states/byte pairs, exact 32-iteration bound, callback mutations, exact read traces, boundary objects; no controller writes\n",(unsigned long long)cases);
 return 0;
}
