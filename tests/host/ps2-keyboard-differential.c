/* Actual Rust ABI against the frozen accepted C. Hosted only. */
#define _GNU_SOURCE
#include "keyboard.h"
#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
_Static_assert(sizeof(struct keyboard_decoder)==7,"seven bytes");
_Static_assert(_Alignof(struct keyboard_decoder)==1,"byte alignment");
#define OFF(n,v) _Static_assert(offsetof(struct keyboard_decoder,n)==v,"offset")
OFF(shift,0);OFF(ctrl,1);OFF(alt,2);OFF(caps,3);OFF(caps_down,4);OFF(extended,5);OFF(pause_left,6);
char keyboard_decode_reference(struct keyboard_decoder *,uint8_t);
static uint64_t comparisons,seed=UINT64_C(0x72b15e7deca0de01);
static uint64_t random64(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static struct keyboard_decoder state(unsigned n){
 struct keyboard_decoder d={0};
 d.shift=n%4;n/=4;d.ctrl=n%4;n/=4;d.alt=n%4;n/=4;
 d.caps=n%2;n/=2;d.caps_down=n%2;n/=2;d.extended=n%2;n/=2;d.pause_left=n;
 return d;
}
static unsigned index_state(struct keyboard_decoder d){
 assert(d.shift<4&&d.ctrl<4&&d.alt<4&&d.caps<2&&d.caps_down<2&&d.extended<2&&d.pause_left<6);
 return d.shift+4*(d.ctrl+4*(d.alt+4*(d.caps+2*(d.caps_down+2*(d.extended+2*d.pause_left)))));
}
static void compare(struct keyboard_decoder initial,uint8_t scan){
 struct keyboard_decoder c=initial,r=initial;
 char a=keyboard_decode_reference(&c,scan),b=keyboard_decode(&r,scan);
 if(a!=b||memcmp(&c,&r,7)){
  fprintf(stderr,"mismatch at comparison %"PRIu64" scan=%02x\n",comparisons,scan);abort();
 }
 comparisons++;
}
static void guards(void){
 assert(keyboard_decode(NULL,0x1e)==0);
 assert(keyboard_decode((struct keyboard_decoder *)(UINTPTR_MAX-2),0x1e)==0);
 long page=sysconf(_SC_PAGESIZE);assert(page>7);
 unsigned char *p=mmap(NULL,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
 assert(!mprotect(p+page,(size_t)page,PROT_READ|PROT_WRITE));
 struct keyboard_decoder *d=(void*)(p+page);memset(d,0,7);assert(keyboard_decode(d,0x1e)=='a');
 d=(void*)(p+2*page-7);memset(d,0,7);assert(keyboard_decode(d,0x1e)=='a');
 d=(void*)(p+page+1);memset(d,0,7);assert(keyboard_decode(d,0x2a)==0&&d->shift==1);
 assert(!munmap(p,(size_t)page*3));
 puts("ABI PASS: size=7 align=1 offsets=0..6, null/wrap, both guard-page boundaries and unaligned byte state");
}
static uint64_t clock_ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static volatile unsigned sink;
static void bench(void){
 char (*volatile functions[2])(struct keyboard_decoder*,uint8_t)={keyboard_decode_reference,keyboard_decode};
 for(unsigned round=0;round<7;round++)for(unsigned impl=0;impl<2;impl++){
  struct keyboard_decoder d={0};unsigned sum=0;uint64_t start=clock_ns();
  const uint8_t scans[]={0x2a,0x1e,0x9e,0xaa,0x3a,0xba,0x02,0x82};
  for(unsigned i=0;i<6000000;i++)sum+=(unsigned char)functions[impl](&d,scans[i&7]);
  uint64_t elapsed=clock_ns()-start;sink=sum;
  printf("%s,%u,6000000,%"PRIu64",%.4f\n",impl?"Rust":"C",round,elapsed,(double)elapsed/6000000);
 }
}
int main(int argc,char **argv){
 if(argc==2&&!strcmp(argv[1],"--bench")){bench();return 0;}
 guards();
 for(unsigned n=0;n<3072;n++)for(unsigned b=0;b<256;b++)compare(state(n),(uint8_t)b);
 static unsigned queue[3072];static unsigned char seen[3072];unsigned head=0,tail=1;seen[0]=1;
 while(head<tail){unsigned n=queue[head++];for(unsigned b=0;b<256;b++){
  struct keyboard_decoder d=state(n);compare(d,(uint8_t)b);keyboard_decode_reference(&d,(uint8_t)b);
  unsigned j=index_state(d);if(!seen[j]){assert(tail<3072);queue[tail++]=j;seen[j]=1;}
 }}
 for(unsigned field=0;field<7;field++)for(unsigned v=0;v<256;v++)for(unsigned b=0;b<256;b++){
  struct keyboard_decoder d={0};((unsigned char*)&d)[field]=(uint8_t)v;compare(d,(uint8_t)b);
 }
 for(unsigned i=0;i<1000000;i++){
  uint64_t bits=random64();struct keyboard_decoder d;memcpy(&d,&bits,7);
  if(i&1)d.pause_left=0;
  compare(d,(uint8_t)(bits>>56));
 }
 struct keyboard_decoder c={0},r={0};
 for(unsigned i=0;i<1000000;i++){
  uint8_t b=(uint8_t)random64();char a=keyboard_decode_reference(&c,b),x=keyboard_decode(&r,b);
  assert(a==x&&!memcmp(&c,&r,7));comparisons++;
 }
 printf("differential PASS: %"PRIu64" state/byte comparisons; 3072 encoded states; %u reachable states; seed=72b15e7deca0de01; hostile field bytes and 1000000-step stream\n",comparisons,tail);
 return 0;
}
