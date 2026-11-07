#define _GNU_SOURCE
#include "keyboard.h"
#include "rust_kernel.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
/* R2-W bounded controller writes: frozen accepted C oracle (74abed0) versus the
 * Rust-owned write_port()/command()/aux_command(), directly with exact read+write
 * traces and through every lifecycle caller. */
_Static_assert(sizeof(struct keyboard)==88,"keyboard ABI");
extern int write_port_reference(struct keyboard *,uint16_t,uint8_t);
extern int command_reference(struct keyboard *,uint8_t);
extern int aux_command_reference(struct keyboard *,uint8_t);
extern int keyboard_init_reference(struct keyboard *,const struct keyboard_io *);
extern char keyboard_poll_reference(struct keyboard *);
extern int keyboard_suspend_reference(struct keyboard *);
extern int keyboard_resume_reference(struct keyboard *);
extern int keyboard_pointer_start_reference(struct keyboard *);
extern void keyboard_pointer_stop_reference(struct keyboard *);
extern int keyboard_pointer_poll_reference(struct keyboard *,struct pointer_packet *);
static void die(const char *s);
static uint64_t th,tn,reads,writes,cases,scenarios;static int bench;static struct keyboard *active;
static uint16_t last_port;static uint8_t last_value;
#define MAXREADS 2000000ULL
static void tr(uint32_t e){if(!bench){th=(th^e)*UINT64_C(0x100000001b3);tn++;}}
/* ---------------- helper-level scripted controller ----------------
 * Every read (either port) consumes the next script byte; after the script, status
 * reads return fill_status and data reads fill_data. Writes are traced, never consumed. */
#define SCRIPT_MAX 200064u
static uint8_t script[SCRIPT_MAX];static unsigned nscript,pos;static uint8_t fill_status,fill_data;
static uint8_t s_read(uint16_t port,uint32_t tag){
 if(port!=0x64 && port!=0x60)die("unexpected read port");
 if(++reads>MAXREADS)die("read bound");
 uint8_t v=pos<nscript?script[pos++]:(port==0x64?fill_status:fill_data);
 tr(tag|((uint32_t)port<<8)|v);
 return v;
}
static void s_write(uint16_t port,uint8_t v,uint32_t tag){
 if(port!=0x64 && port!=0x60)die("unexpected write port");
 writes++;last_port=port;last_value=v;tr(0x80000000u|tag|((uint32_t)port<<8)|v);
}
static uint8_t s_in(uint16_t p){return s_read(p,0);}
static void s_out(uint16_t p,uint8_t v){s_write(p,v,0);}
static uint8_t a_in(uint16_t p){return s_read(p,1u<<24);}
static void a_out(uint16_t p,uint8_t v){s_write(p,v,1u<<24);}
static const struct keyboard_io s_io={s_in,s_out},a_io={a_in,a_out},null_out_io={s_in,NULL};
/* io substitution: after swap_after reads through w_io, the live object's io becomes a_io.
 * Reads and writes carry the table's tag, so a stale table is visible in the trace. */
static uint64_t swap_after;
static uint8_t w_in(uint16_t p){uint8_t v=s_read(p,2u<<24);if(reads==swap_after)active->io=&a_io;return v;}
static void w_out(uint16_t p,uint8_t v){s_write(p,v,2u<<24);}
static const struct keyboard_io w_io={w_in,w_out};
static void die(const char *s){fprintf(stderr,"FAIL %s pos=%u reads=%llu writes=%llu cases=%llu\n",s,pos,(unsigned long long)reads,(unsigned long long)writes,(unsigned long long)cases);exit(1);}
static uint64_t rng=UINT64_C(0x7232772d77726974);
static uint32_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)rng;}
static struct keyboard object(void){struct keyboard k;memset(&k,0x5a,sizeof k);k.io=&s_io;k.ready=1;k.pointer_ready=0;return k;}
enum helper{WRITE_DATA,WRITE_STATUS,COMMAND,AUX_COMMAND};
static int call(enum helper h,int rust,struct keyboard *k,uint8_t value){
 switch(h){
 case WRITE_DATA:return rust?vos_rust_keyboard_write_port(k,0x60,value):write_port_reference(k,0x60,value);
 case WRITE_STATUS:return rust?vos_rust_keyboard_write_port(k,0x64,value):write_port_reference(k,0x64,value);
 case COMMAND:return rust?vos_rust_keyboard_command(k,value):command_reference(k,value);
 default:return rust?vos_rust_keyboard_aux_command(k,value):aux_command_reference(k,value);
 }
}
struct run{int r;uint64_t h,n,reads,writes;unsigned pos;uint16_t port;uint8_t value;struct keyboard k;};
static void run_helper(enum helper h,int rust,uint8_t value,struct run *o){
 o->k=object();if(swap_after)o->k.io=&w_io;active=&o->k;pos=0;reads=0;writes=0;last_port=0;last_value=0;th=UINT64_C(0xcbf29ce484222325);tn=0;
 o->r=call(h,rust,&o->k,value);
 o->h=th;o->n=tn;o->reads=reads;o->writes=writes;o->pos=pos;o->port=last_port;o->value=last_value;
}
static struct run ra,rb;
/* Returns the candidate result after proving it equal to the frozen C oracle. */
static int compare_helper(enum helper h,uint8_t value){
 run_helper(h,0,value,&ra);run_helper(h,1,value,&rb);
 if(ra.r!=rb.r||ra.h!=rb.h||ra.n!=rb.n||ra.reads!=rb.reads||ra.writes!=rb.writes||ra.pos!=rb.pos||memcmp(&ra.k,&rb.k,sizeof ra.k))die("C/Rust helper trace equivalence");
 if(h<=WRITE_STATUS && rb.writes!=(uint64_t)(rb.r?1:0))die("write_port must write exactly once on success and never on failure");
 if(h<=WRITE_STATUS && rb.r && (rb.port!=(h==WRITE_DATA?0x60:0x64)||rb.value!=value))die("write_port port/value");
 cases++;
 return rb.r;
}
static void set_script(const uint8_t *b,unsigned n,uint8_t fs,uint8_t fd){if(n>SCRIPT_MAX)die("script size");memcpy(script,b,n);nscript=n;fill_status=fs;fill_data=fd;}
/* N busy (IBF) status reads inserted at offset 'at' of a prefix. */
static void busy_script(const uint8_t *pre,unsigned at,unsigned busy,const uint8_t *post,unsigned npost,uint8_t fs,uint8_t fd){
 unsigned n=0;
 for(unsigned i=0;i<at;i++)script[n++]=pre[i];
 for(unsigned i=0;i<busy;i++)script[n++]=0x02;
 for(unsigned i=0;i<npost;i++)script[n++]=post[i];
 nscript=n;fill_status=fs;fill_data=fd;
}
/* ---------------- lifecycle-level i8042 emulator (accepted R2 harness) ---------------- */
static struct {uint8_t q[64],qa[64];unsigned head,count;uint8_t mode,last;int expect_mode,to_aux;uint32_t seed;unsigned faults;} em;
static uint32_t er(void){em.seed^=em.seed<<13;em.seed^=em.seed>>17;em.seed^=em.seed<<5;return em.seed;}
static void push(uint8_t b,int aux){if(em.count<64){unsigned i=(em.head+em.count)%64;em.q[i]=b;em.qa[i]=(uint8_t)aux;em.count++;}}
static uint8_t e_in(uint16_t port){
 uint8_t v;
 if(++reads>MAXREADS)die("lifecycle read bound");
 if(port==0x64){
  v=0x10;int stall=(em.faults&1) && er()%8==0;
  if(em.count && !stall){v|=1;if(em.qa[em.head])v|=0x20;if((em.faults&2) && er()%97==0)v|=0x80;}
  if((em.faults&8) && er()%61==0)v|=0x02;
 }else if(port==0x60){if(em.count){v=em.q[em.head];em.head=(em.head+1)%64;em.count--;em.last=v;}else v=em.last;}
 else die("unexpected port");
 tr(((uint32_t)port<<8)|v);return v;
}
static void e_out(uint16_t port,uint8_t v){
 tr(0x80000000u|((uint32_t)port<<8)|v);
 if(port==0x64){if(v==0x20)push(em.mode,0);else if(v==0x60)em.expect_mode=1;else if(v==0xd4)em.to_aux=1;}
 else if(port==0x60){
  if(em.expect_mode){em.mode=v;em.expect_mode=0;}
  else if(em.to_aux){em.to_aux=0;if((em.faults&4) && er()%5==0)push(0xfe,1);else{push(0xfa,1);if(v==0xff){push(0xaa,1);push(0x00,1);}}}
  else if((em.faults&4) && er()%5==0)push(0xfe,0);else push(0xfa,0);
 }else die("unexpected write port");
 if((em.faults&16) && er()%7==0)push((uint8_t)er(),(int)(er()&1));
}
static const struct keyboard_io e_io={e_in,e_out};
struct fns{int(*init)(struct keyboard*,const struct keyboard_io*);char(*poll)(struct keyboard*);int(*start)(struct keyboard*);
 void(*stop)(struct keyboard*);int(*ppoll)(struct keyboard*,struct pointer_packet*);int(*suspend)(struct keyboard*);int(*resume)(struct keyboard*);};
static const struct fns REF={keyboard_init_reference,keyboard_poll_reference,keyboard_pointer_start_reference,keyboard_pointer_stop_reference,
 keyboard_pointer_poll_reference,keyboard_suspend_reference,keyboard_resume_reference};
static const struct fns CAND={keyboard_init,keyboard_poll,keyboard_pointer_start,keyboard_pointer_stop,keyboard_pointer_poll,keyboard_suspend,keyboard_resume};
struct outcome{int r[8];char c[4];struct pointer_packet p;struct keyboard k;uint64_t h,n;};
static void scenario(const struct fns *f,uint32_t seed,unsigned faults,struct outcome *o){
 memset(&em,0,sizeof em);em.seed=seed|1;em.faults=faults;em.mode=0x47;memset(o,0,sizeof *o);memset(&o->k,0,sizeof o->k);
 th=UINT64_C(0xcbf29ce484222325);tn=0;reads=0;
 o->r[0]=f->init(&o->k,&e_io);
 push(0x1e,0);o->c[0]=f->poll(&o->k);push(0x9e,0);push(0x30,0);o->c[1]=f->poll(&o->k);
 o->r[1]=f->start(&o->k);
 push(0x08,1);push(0x01,1);push(0xff,1);push(0x20,0);
 memset(&o->p,0x5a,sizeof o->p);o->r[2]=f->ppoll(&o->k,&o->p);o->c[2]=f->poll(&o->k);
 f->stop(&o->k);o->r[3]=f->suspend(&o->k);o->r[4]=f->resume(&o->k);push(0x1e,0);o->c[3]=f->poll(&o->k);
 o->h=th;o->n=tn;
}
static void compare_lifecycle(uint32_t seed,unsigned faults){
 struct outcome a,b;scenario(&REF,seed,faults,&a);scenario(&CAND,seed,faults,&b);
 if(memcmp(a.r,b.r,sizeof a.r)||memcmp(a.c,b.c,sizeof a.c)||memcmp(&a.p,&b.p,sizeof a.p)||memcmp(&a.k,&b.k,sizeof a.k)||a.h!=b.h||a.n!=b.n)die("C/Rust lifecycle trace equivalence");
 if(!faults && !(b.r[0]==1 && b.r[1]==1 && b.r[2]==1 && b.c[0]=='a' && b.c[1]=='b' && b.r[3]==1 && b.r[4]==1 && b.c[3]=='a'))die("fault-free lifecycle did not succeed");
 scenarios++;
}
/* ---------------- benchmark ---------------- */
static uint64_t ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))die("clock");return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static volatile uint64_t sink;
static enum helper workload(unsigned w){
 static const uint8_t clear[1]={0x00},cmd_ack[3]={0x00,0x01,0xfa},aux_ack[4]={0x00,0x00,0x21,0xfa},cmd_nak[3]={0x00,0x01,0xfe};
 if(w==0){set_script(clear,1,0x00,0x00);return WRITE_DATA;}
 if(w==1){busy_script(clear,0,10,clear,1,0x00,0x00);return WRITE_STATUS;}
 if(w==2){set_script(cmd_ack,3,0x01,0xfa);return COMMAND;}
 if(w==3){set_script(aux_ack,4,0x21,0xfa);return AUX_COMMAND;}
 if(w==4){set_script(cmd_nak,3,0x01,0xfe);return COMMAND;}
 set_script(clear,0,0x02,0x00);return WRITE_DATA;
}
static double timing(unsigned w,int rust){
 unsigned iters=w==5?20:20000;enum helper h=workload(w);struct keyboard k=object();uint64_t a=ns();
 for(unsigned i=0;i<iters;i++){active=&k;pos=0;reads=0;writes=0;int r=call(h,rust,&k,0xf4);sink+=(uint64_t)r+reads+writes;}
 return (double)(ns()-a)/iters;
}
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"--bench")){
  bench=1;puts("workload,sample,C_ns,Rust_ns");
  for(unsigned w=0;w<6;w++){(void)timing(w,0);(void)timing(w,1);
   for(unsigned s=0;s<9;s++){double a,b;if(s&1){b=timing(w,1);a=timing(w,0);}else{a=timing(w,0);b=timing(w,1);}printf("%u,%u,%.4f,%.4f\n",w,s,a,b);}}
  return 0;
 }
 uint8_t b[8];
 /* 1. Exhaustive: write_port status x value per port; command write-status x read-status
  *    and read-status x ACK; aux_command both write statuses and read-status x ACK. */
 for(unsigned s=0;s<256;s++)for(unsigned v=0;v<256;v++){
  b[0]=(uint8_t)s;set_script(b,1,0x00,0x00);compare_helper(WRITE_DATA,(uint8_t)v);
  set_script(b,1,0x00,0x00);compare_helper(WRITE_STATUS,(uint8_t)v);
  b[0]=(uint8_t)s;b[1]=(uint8_t)v;set_script(b,2,0x01,0xfa);compare_helper(COMMAND,0xf4);
  b[0]=0x00;b[1]=(uint8_t)s;b[2]=(uint8_t)v;set_script(b,3,0x01,0xfa);
  /* With OBF set the outcome is fixed: ERR fails; aux is consumed then fill ACKs; keyboard needs 0xFA. */
  int rc=compare_helper(COMMAND,0xf5);
  if((s&1) && rc!=(!(s&0xc0) && ((s&0x20) || v==0xfa)))die("command status x ACK semantics");
  b[0]=(uint8_t)s;b[1]=(uint8_t)v;set_script(b,2,0x21,0xfa);compare_helper(AUX_COMMAND,0xf4);
  b[0]=0x00;b[1]=0x00;b[2]=(uint8_t)s;b[3]=(uint8_t)v;set_script(b,4,0x21,0xfa);
  int ra2=compare_helper(AUX_COMMAND,0xf6);
  if((s&1) && ra2!=(!(s&0xc0) && (!(s&0x20) || v==0xfa)))die("aux status x ACK semantics");
 }
 /* 2. Wait-limit boundaries: N busy status reads before IBF clears; success iff N <= 99,999. */
 static const unsigned delays[7]={0,1,2,1000,99998,99999,100000};const uint8_t z[1]={0x00};
 for(unsigned i=0;i<7;i++){unsigned n=delays[i];int want=n<=99999;
  busy_script(z,0,n,z,1,0x00,0x00);if(compare_helper(WRITE_DATA,0x42)!=want || rb.reads!=(n<100000?n+1:100000))die("write_port wait boundary");
  busy_script(z,0,n,z,1,0x00,0x00);if(compare_helper(WRITE_STATUS,0xae)!=want)die("write_port status wait boundary");
  busy_script(z,0,n,z,1,0x01,0xfa);if(compare_helper(COMMAND,0xf4)!=want)die("command write wait boundary");
  busy_script(z,0,n,z,1,0x21,0xfa);if(compare_helper(AUX_COMMAND,0xf4)!=want || rb.writes!=(want?2u:0u))die("aux prefix wait boundary");
  busy_script(z,1,n,z,1,0x21,0xfa);if(compare_helper(AUX_COMMAND,0xf4)!=want || rb.writes!=(want?2u:1u))die("aux data wait boundary");
 }
 /* 3. Failure injection at every command step, with exact expected writes. */
 { struct {enum helper h;uint8_t s[6];unsigned n;uint8_t fs,fd;int want;unsigned w;uint64_t rd;} f[11]={
   {COMMAND,{0},0,0x02,0x00,0,0,100000},                       /* write never possible */
   {COMMAND,{0x00},1,0x00,0x00,0,1,100001},                    /* ACK read times out */
   {COMMAND,{0x00,0x41,0xfa},3,0x01,0xfa,0,1,3},               /* ERR before channel */
   {COMMAND,{0x00,0x01,0xfe},3,0x01,0xfa,0,1,3},               /* NAK */
   {COMMAND,{0x00,0x21,0xfa,0x01,0xfa},5,0x01,0xfe,1,1,5},     /* aux byte consumed, then ACK */
   {AUX_COMMAND,{0},0,0x02,0x00,0,0,100000},                   /* prefix write never possible */
   {AUX_COMMAND,{0x00},1,0x02,0x00,0,1,100001},                /* data write never possible */
   {AUX_COMMAND,{0x00,0x00},2,0x00,0x00,0,2,100002},           /* aux ACK times out */
   {AUX_COMMAND,{0x00,0x00,0x61,0xfa},4,0x21,0xfa,0,2,4},      /* ERR */
   {AUX_COMMAND,{0x00,0x00,0x21,0xfe},4,0x21,0xfa,0,2,4},      /* NAK */
   {AUX_COMMAND,{0x00,0x00,0x01,0xfa,0x21,0xfa},6,0x21,0xfe,1,2,6}};/* keyboard byte consumed, then ACK */
   for(unsigned i=0;i<11;i++){set_script(f[i].s,f[i].n,f[i].fs,f[i].fd);
    if(compare_helper(f[i].h,0xf4)!=f[i].want || rb.writes!=f[i].w || rb.reads!=f[i].rd)die("failure-injection step semantics");}
 }
 /* 4. Deterministic random streams biased to controller-significant bytes. */
 static const uint8_t bias[10]={0x00,0x01,0x02,0x03,0x21,0x41,0x81,0xfa,0xfe,0x10};
 for(unsigned h=0;h<4;h++)for(unsigned i=0;i<40000;i++){unsigned n=1+next()%48;
  for(unsigned j=0;j<n;j++){uint32_t r=next();script[j]=(r&3)?bias[(r>>2)%10]:(uint8_t)(r>>8);}
  nscript=n;fill_status=(uint8_t)((next()&1)?0x01:0x21);fill_data=(uint8_t)((next()&3)?0xfa:next());compare_helper((enum helper)h,(uint8_t)next());}
 /* 5. Callback substitutes io mid-operation: every later read AND write must use the new
  *    callback table (io reloaded per access), exactly as the accepted C does. */
 for(unsigned h=0;h<4;h++)for(unsigned w=1;w<=8;w++)for(unsigned i=0;i<16;i++){
  unsigned n=3+next()%6;for(unsigned j=0;j<n;j++)script[j]=bias[next()%10];nscript=n;fill_status=(next()&1)?0x01:0x21;fill_data=0xfa;
  swap_after=w;compare_helper((enum helper)h,(uint8_t)(0x40+i));
 }
 swap_after=0;
 /* 6. Rust-only refusals (C oracle has no defined behaviour here): no access at all. */
 { struct keyboard k=object();active=&k;nscript=0;fill_status=0x00;fill_data=0xfa;
  static const uint16_t bad[8]={0x00,0x5f,0x61,0x63,0x65,0x160,0x164,0xffff};
  for(unsigned i=0;i<8;i++){reads=writes=0;if(vos_rust_keyboard_write_port(&k,bad[i],0xfe)!=0 || reads || writes)die("unauthorized port accepted");}
  reads=writes=0;
  if(vos_rust_keyboard_write_port(NULL,0x60,1)||vos_rust_keyboard_command(NULL,1)||vos_rust_keyboard_aux_command(NULL,1)||reads||writes)die("NULL keyboard accepted");
  void *mis=(char*)&k+1,*wrap=(void*)(UINTPTR_MAX-7);
  if(vos_rust_keyboard_write_port(mis,0x60,1)||vos_rust_keyboard_command(mis,1)||vos_rust_keyboard_aux_command(mis,1)||reads||writes)die("misaligned keyboard accepted");
  if(vos_rust_keyboard_write_port(wrap,0x60,1)||vos_rust_keyboard_command(wrap,1)||vos_rust_keyboard_aux_command(wrap,1)||reads||writes)die("wrapping keyboard accepted");
  k.io=&null_out_io;pos=0;
  if(vos_rust_keyboard_write_port(&k,0x60,1)||reads!=1||writes)die("null out callback called");
  reads=0;if(vos_rust_keyboard_aux_command(&k,1)||reads!=1||writes)die("null out callback in aux_command");
  long page=sysconf(_SC_PAGESIZE);unsigned char *mp=mmap(0,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(mp==MAP_FAILED || mprotect(mp+page,(size_t)page,PROT_READ|PROT_WRITE))die("guard pages");
  struct keyboard *edge=(void*)(mp+2*page-sizeof *edge);*edge=object();active=edge;pos=0;reads=writes=0;nscript=0;fill_status=0x01;fill_data=0xfa;
  if(vos_rust_keyboard_command(edge,0xf4)!=1 || reads!=3 || writes!=1)die("guard-page edge object");
  if(munmap(mp,(size_t)page*3))die("unmap");
 }
 /* 7. Lifecycle callers through an i8042 emulator, read AND write traces, fault masks 0..31. */
 for(unsigned i=0;i<3000;i++)compare_lifecycle(0x9e3779b9u*(i+1),i%32);
 /* Coverage: 6 x 65536 exhaustive + 35 boundaries + 11 failure steps + 160000 random + 512 io swaps. */
 if(cases!=553774 || scenarios!=3000)die("comparison coverage count");
 printf("PASS %llu controller write comparisons and %llu lifecycle scenarios; seed=7232772d77726974; exhaustive status x value/ACK, exact 100000 wait limit, failure at every step, io reload, refused ports/objects, read+write traces\n",
        (unsigned long long)cases,(unsigned long long)scenarios);
 return 0;
}
