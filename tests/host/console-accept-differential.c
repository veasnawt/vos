/* Actual production ABI, frozen accepted C and callback-observable order. */
#define _GNU_SOURCE
#include "input_console.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
_Static_assert(sizeof(struct input_console_line)==144 && _Alignof(struct input_console_line)==4,"layout");
_Static_assert(offsetof(struct input_console_line,bytes)==4 && offsetof(struct input_console_line,ready)==136 && offsetof(struct input_console_line,overflow)==140,"offsets");
void input_console_accept_reference(struct input_console_line*,char);
struct event { unsigned kind; struct input_console_line state; };
static struct input_console_line *active;
static struct event trace[4];static unsigned used;static int edit,benchmark;
static volatile unsigned sink;
static void record(unsigned kind){if(benchmark){sink+=kind;return;}assert(used<4);trace[used].kind=kind;trace[used++].state=*active;}
void serial_putc(char c){record((unsigned char)c);}
void serial_puts(const char *s){while(*s)serial_putc(*s++);}
void console_erase_previous(void){record(8);if(edit)active->length=(unsigned)edit;}
static uint64_t seed=UINT64_C(0xc1a55e7a20261102),count;
static uint64_t rnd(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static void compare(struct input_console_line *state,unsigned byte,int mutation){
 struct input_console_line a=*state,b=*state;struct event expected[4];
 memset(trace,0,sizeof trace);used=0;edit=mutation;active=&a;input_console_accept_reference(&a,(char)byte);unsigned n=used;memcpy(expected,trace,sizeof trace);
 memset(trace,0,sizeof trace);used=0;active=&b;input_console_accept(&b,(char)byte);
 if(memcmp(&a,&b,sizeof a)||n!=used||memcmp(expected,trace,sizeof trace)){fprintf(stderr,"mismatch case=%llu byte=%u len=%u ready=%d overflow=%d edit=%d\n",(unsigned long long)count,byte,state->length,state->ready,state->overflow,mutation);abort();}
 *state=b;count++;
}
static void guards(void){
 input_console_accept(NULL,'x');input_console_accept((void*)(UINTPTR_MAX-100),'x');
 long page=sysconf(_SC_PAGESIZE);assert(page>144);unsigned char *p=mmap(NULL,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);assert(!mprotect(p+page,(size_t)page,PROT_READ|PROT_WRITE));
 for(unsigned edge=0;edge<2;edge++){
  struct input_console_line *s=(void*)(p+(edge?2*page-144:page));memset(s,0,144);s->length=127;benchmark=1;input_console_accept(s,'z');assert(s->length==128 && !s->bytes[128]);input_console_accept(s,'z');assert(s->overflow);input_console_accept(s,8);assert(s->length==127);benchmark=0;
 }
 assert(!munmap(p,(size_t)page*3));struct input_console_line s={.length=UINT32_MAX};active=&s;input_console_accept(&s,'x');assert(s.length==UINT32_MAX);used=0;input_console_accept(&s,8);assert(s.length==UINT32_MAX && used==1);
 puts("ABI/guard PASS: 144B align4 offsets0/4/136/140, null/wrap, guard pages, malformed length bounded");
}
static uint64_t ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static struct input_console_line states[256],original;
static void bench(void){
 void (*volatile fn[2])(struct input_console_line*,char)={input_console_accept_reference,input_console_accept};
 const char *names[]={"ready","ignored","append","backspace","newline","overflow"};char chars[]={ 'x',0,'x',8,10,'x'};benchmark=1;
 puts("workload,implementation,sample,operations,elapsed_ns,ns_per_call,operations_per_second");
 for(unsigned w=0;w<6;w++)for(unsigned sample=0;sample<9;sample++)for(unsigned turn=0;turn<2;turn++){
  unsigned impl=turn^(sample&1);uint64_t elapsed=0;memset(&original,0,sizeof original);original.length=w==5?128:16;original.ready=w==0;memset(original.bytes,'a',original.length);
  for(unsigned batch=0;batch<5010;batch++){
   for(unsigned i=0;i<256;i++)states[i]=original;
   uint64_t start=ns();for(unsigned i=0;i<256;i++)fn[impl](&states[i],chars[w]);uint64_t delta=ns()-start;if(batch>=10)elapsed+=delta;
  }
  unsigned calls=5000*256;printf("%s,%s,%u,%u,%llu,%.4f,%.1f\n",names[w],impl?"Rust":"C",sample,calls,(unsigned long long)elapsed,(double)elapsed/calls,(double)calls*1e9/elapsed);
 }
}
int main(int argc,char **argv){
 if(argc==2&&!strcmp(argv[1],"--bench")){bench();return 0;}
 guards();int flags[]={0,1,-7};
 for(unsigned len=0;len<=128;len++)for(unsigned r=0;r<3;r++)for(unsigned o=0;o<3;o++)for(unsigned byte=0;byte<256;byte++){
  struct input_console_line s;memset(&s,0x5a,sizeof s);s.length=len;s.ready=flags[r];s.overflow=flags[o];compare(&s,byte,0);
 }
 for(unsigned len=1;len<=128;len++)for(unsigned next=1;next<=129;next++){
  struct input_console_line s={.length=len};compare(&s,8,(int)next);
 }
 for(unsigned round=0;round<2000;round++){
  struct input_console_line s={0};for(unsigned i=0;i<256;i++){compare(&s,(unsigned)(rnd()&255),0);if(s.ready)memset(&s,0,sizeof s);}
 }
 printf("differential PASS: %llu cases, exhaustive bytes/lengths/flags, post-erase callback reload, seed=c1a55e7a20261102\n",(unsigned long long)count);
 return 0;
}
