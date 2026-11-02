/* Frozen accepted reader vs production Rust, sharing accepted editor/extractor. */
#define _GNU_SOURCE
#include "input_console.h"
#include "keyboard.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
_Static_assert(offsetof(struct keyboard,ready)==16 && sizeof(struct keyboard)==88,"keyboard ABI");
_Static_assert(sizeof(struct input_console_line)==144,"editor ABI");
int input_console_read_line_for_reference(struct keyboard*,char*,unsigned,const char*,int(*)(void*),void*);
static unsigned at,n,checks,sleeps,cancel_at,used;static unsigned char bytes[1024];static uint32_t trace[2048];static int benchmark;
static void event(unsigned e){if(!benchmark){assert(used<2048);trace[used++]=e;}}
void serial_putc(char c){event(0x10000u+(unsigned char)c);}
void serial_puts(const char *s){while(*s)serial_putc(*s++);}
void console_erase_previous(void){event(0x20000);}
char keyboard_poll(struct keyboard *k){assert(k&&k->ready&&at<n);event(0x30000+bytes[at]);return (char)bytes[at++];}
void thread_sleep(uint64_t ticks){assert(ticks==1&&sleeps<2048);sleeps++;event(0x40000);}
static int cancelled(void *ctx){assert(ctx==&checks);checks++;event(0x50000);return cancel_at&&checks>=cancel_at?-9:0;}
static uint64_t count,seed=UINT64_C(0xc0a2d5e720261102);
static uint64_t rnd(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static void reset(void){at=checks=sleeps=used=0;memset(trace,0,sizeof trace);}
static void compare(int ready,unsigned cap,int present,unsigned cancel,int hook){
 struct keyboard k={.ready=ready};char a[160],b[160];memset(a,0x5a,sizeof a);memcpy(b,a,sizeof a);cancel_at=cancel;reset();
 int c=input_console_read_line_for_reference(ready==-1?NULL:&k,present?a:NULL,cap,"P> ",hook?cancelled:NULL,&checks);
 unsigned ca=at,cc=checks,cs=sleeps,cu=used;uint32_t expected[2048];memcpy(expected,trace,sizeof trace);reset();
 int r=input_console_read_line_for(ready==-1?NULL:&k,present?b:NULL,cap,"P> ",hook?cancelled:NULL,&checks);
 if(c!=r||memcmp(a,b,sizeof a)||ca!=at||cc!=checks||cs!=sleeps||cu!=used||memcmp(expected,trace,sizeof trace)){
  fprintf(stderr,"mismatch case=%llu ready=%d cap=%u present=%d cancel=%u hook=%d C=%d Rust=%d\n",(unsigned long long)count,ready,cap,present,cancel,hook,c,r);__builtin_abort();
 }
 count++;
}
static uint64_t ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return(uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static volatile int sink;
static void bench(void){
 const char *names[]={"unavailable","nospace","empty","short16","maximum","overflow-retry","initial-cancel","mid-cancel","idle16"};
 int (*volatile fn[2])(struct keyboard*,char*,unsigned,const char*,int(*)(void*),void*)={input_console_read_line_for_reference,input_console_read_line_for};
 benchmark=1;puts("workload,implementation,sample,operations,elapsed_ns,ns_per_call,operations_per_second");
 for(unsigned w=0;w<9;w++)for(unsigned sample=0;sample<9;sample++)for(unsigned turn=0;turn<2;turn++){
  unsigned impl=turn^(sample&1);struct keyboard k={.ready=w!=0};char out[160];unsigned cap=w==1?128:129;int hook=w==6||w==7;
  n=w==3?17:w==4?129:w==5?132:w==8?17:1;memset(bytes,w==8?0:'a',n);bytes[n-1]=10;if(w==5){bytes[129]=10;bytes[130]='x';bytes[131]=10;}cancel_at=w==6?1:w==7?2:0;if(w==7){n=8;memset(bytes,0,n);}
  uint64_t elapsed=0;for(unsigned batch=0;batch<2010;batch++){
   /* Common script counters reset inside timing; both paths own a fresh local editor. */
   uint64_t start=ns();for(unsigned i=0;i<64;i++){at=checks=sleeps=used=0;sink=fn[impl](&k,out,cap,"P> ",hook?cancelled:NULL,&checks);}uint64_t delta=ns()-start;if(batch>=10)elapsed+=delta;
  }
  unsigned calls=2000*64;printf("%s,%s,%u,%u,%llu,%.4f,%.1f\n",names[w],impl?"Rust":"C",sample,calls,(unsigned long long)elapsed,(double)elapsed/calls,(double)calls*1e9/elapsed);
 }
}
static void guard(void){
 assert(input_console_read_line_for(NULL,NULL,0,NULL,NULL,NULL)==-4);
 assert(input_console_read_line_for((void*)(UINTPTR_MAX-10),NULL,0,NULL,NULL,NULL)==-4);
 struct keyboard k={.ready=1};assert(input_console_read_line_for(&k,NULL,129,NULL,NULL,NULL)==-2);
 long page=sysconf(_SC_PAGESIZE);unsigned char *p=mmap(NULL,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);assert(!mprotect(p+page,(size_t)page,PROT_READ|PROT_WRITE));
 n=129;memset(bytes,'x',128);bytes[128]=10;reset();char *out=(char*)p+2*page-129;assert(input_console_read_line_for(&k,out,129,"",NULL,NULL)==128);assert(!out[128]);assert(!munmap(p,(size_t)page*3));
 puts("ABI/guard PASS: keyboard88/ready16, editor144, null/wrap, exact129-byte output edge");
}
int main(int argc,char **argv){
 if(argc==2&&!strcmp(argv[1],"--bench")){bench();return 0;}
 guard();
 for(unsigned length=0;length<=129;length++){
  n=length+3;memset(bytes,'x',length);bytes[length]=10;bytes[length+1]='y';bytes[length+2]=10;
  for(unsigned cap=0;cap<=131;cap++)for(int ready=-1;ready<=2;ready++)for(int present=0;present<2;present++){
   compare(ready,cap,present,0,0);compare(ready,cap,present,1,1);compare(ready,cap,present,2,1);
  }
 }
 for(unsigned c=0;c<256;c++){n=3;bytes[0]=(unsigned char)c;bytes[1]=10;bytes[2]=10;compare(1,129,1,0,0);}
 for(unsigned round=0;round<5000;round++){
  n=303;for(unsigned i=0;i<300;i++)bytes[i]=(unsigned char)rnd();bytes[300]=10;bytes[301]='z';bytes[302]=10;
  compare(1,129,1,0,0);compare(1,129,1,(unsigned)(rnd()%32+1),1);
 }
 printf("differential PASS: %llu cases; exhaustive lengths/capacity/readiness/null/cancellation, all input bytes, random streams seed=c0a2d5e720261102\n",(unsigned long long)count);
 return 0;
}
