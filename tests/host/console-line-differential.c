/* Actual production ABI vs frozen accepted C. Host only; no device access. */
#define _GNU_SOURCE
#include "input_console.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
_Static_assert(sizeof(struct input_console_line)==144,"size");
_Static_assert(_Alignof(struct input_console_line)==4,"align");
_Static_assert(offsetof(struct input_console_line,length)==0,"length");
_Static_assert(offsetof(struct input_console_line,bytes)==4,"bytes");
_Static_assert(offsetof(struct input_console_line,ready)==136,"ready");
_Static_assert(offsetof(struct input_console_line,overflow)==140,"overflow");
int input_console_take_line_reference(struct input_console_line*,char*,unsigned);
static uint64_t comparisons,seed=UINT64_C(0xa76c209ecf315b84);
static unsigned char *a,*b;
static uint64_t random64(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static void compare(unsigned len,int ready,int overflow,unsigned cap,int dest){
 for(unsigned i=0;i<512;i++)a[i]=(unsigned char)(i*17+len*3);
 struct input_console_line *x=(void*)(a+160);x->length=len;x->ready=ready;x->overflow=overflow;
 memcpy(b,a,512);struct input_console_line *y=(void*)(b+160);
 int c=input_console_take_line_reference(x,dest<0?NULL:(char*)a+dest,cap);
 int r=input_console_take_line(y,dest<0?NULL:(char*)b+dest,cap);
 if(c!=r||memcmp(a,b,512)){fprintf(stderr,"mismatch comparison=%llu len=%u ready=%d overflow=%d cap=%u dest=%d C=%d Rust=%d\n",(unsigned long long)comparisons,len,ready,overflow,cap,dest,c,r);abort();}
 comparisons++;
}
static void guards(void){
 assert(input_console_take_line(NULL,NULL,0)==INPUT_CONSOLE_NOSPACE);
 assert(input_console_take_line((void*)(UINTPTR_MAX-100),NULL,0)==INPUT_CONSOLE_NOSPACE);
 long page=sysconf(_SC_PAGESIZE);assert(page>144);
 unsigned char *p=mmap(NULL,(size_t)page*5,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
 assert(!mprotect(p+page,(size_t)page,PROT_READ|PROT_WRITE));assert(!mprotect(p+3*page,(size_t)page,PROT_READ|PROT_WRITE));
 for(unsigned edge=0;edge<2;edge++){
  struct input_console_line *s=(void*)(p+(edge?2*page-144:page));memset(s,0,144);s->ready=1;s->length=128;memset(s->bytes,'x',128);s->bytes[128]=0;
  char *out=(void*)(p+4*page-129);assert(input_console_take_line(s,out,129)==128);assert(out[0]=='x'&&out[128]==0&&!s->ready);
 }
 struct input_console_line s={.ready=1,.length=129};struct input_console_line old=s;
 assert(input_console_take_line(&s,(void*)1,UINT32_MAX)==INPUT_CONSOLE_NOSPACE&&!memcmp(&s,&old,144));
 s.length=1;old=s;assert(input_console_take_line(&s,(void*)(UINTPTR_MAX-1),2)==INPUT_CONSOLE_NOSPACE&&!memcmp(&s,&old,144));
 assert(!munmap(p,(size_t)page*5));puts("ABI/guard PASS: 144B align4 offsets0/4/136/140, null/wrap, both editor edges, exact output edge, oversized refusal");
}
static uint64_t ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static volatile unsigned sink;
static struct input_console_line states[256],original;
static char output[256][129];
static void bench(void){
 int (*volatile functions[2])(struct input_console_line*,char*,unsigned)={input_console_take_line_reference,input_console_take_line};
 const char *names[]={"pending","overflow","nospace","empty","short16","max128"};
 puts("workload,implementation,sample,operations,elapsed_ns,ns_per_call,operations_per_second");
 for(unsigned work=0;work<6;work++)for(unsigned sample=0;sample<7;sample++)for(unsigned turn=0;turn<2;turn++){
  unsigned impl=turn^(sample&1),sum=0;uint64_t elapsed=0;unsigned cap=129;
  memset(&original,0,sizeof original);original.length=work==5?128:work==3?0:16;original.ready=work!=0;original.overflow=work==1;
  memset(original.bytes,'a',original.length);original.bytes[original.length]=0;if(work==2)cap=original.length;
  for(unsigned batch=0;batch<5010;batch++){
   for(unsigned i=0;i<256;i++)states[i]=original;
   uint64_t start=ns();for(unsigned i=0;i<256;i++)sum+=(unsigned)functions[impl](&states[i],output[i],cap);uint64_t dt=ns()-start;
   if(batch>=10)elapsed+=dt;
  }
  sink=sum;unsigned calls=5000*256;printf("%s,%s,%u,%u,%llu,%.4f,%.1f\n",names[work],impl?"Rust":"C",sample,calls,(unsigned long long)elapsed,(double)elapsed/calls,(double)calls*1e9/elapsed);
 }
}
int main(int argc,char **argv){
 if(argc==2&&!strcmp(argv[1],"--bench")){bench();return 0;}
 a=malloc(512);b=malloc(512);assert(a&&b);guards();
 int flags[]={0,1,-7};for(unsigned len=0;len<=128;len++)for(unsigned cap=0;cap<=130;cap++)for(unsigned r=0;r<3;r++)for(unsigned o=0;o<3;o++)compare(len,flags[r],flags[o],cap,0);
 for(unsigned len=0;len<=128;len++)for(int dest=0;dest<=383;dest++)compare(len,1,0,129,dest);
 for(unsigned len=0;len<=128;len++){compare(len,1,0,UINT32_MAX,0);compare(len,1,0,UINT32_MAX,-1);compare(len,0,1,0,-1);}
 for(unsigned i=0;i<20000;i++){uint64_t v=random64();unsigned len=v%129;compare(len,(int)(v>>32),(int)(v&1),(v>>8)%132,(int)((v>>16)%384));}
 /* Out-of-range lengths are still defined when precedence prevents copying. */
 compare(UINT32_MAX,0,0,UINT32_MAX,-1);compare(UINT32_MAX,1,1,0,0);compare(UINT32_MAX,1,0,0,0);
 printf("differential PASS: %llu cases including exhaustive valid lengths/capacities, noncanonical flags, forward overlaps and seed=a76c209ecf315b84\n",(unsigned long long)comparisons);
 free(a);free(b);return 0;
}
