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
/* R2-L controller lifecycle: frozen accepted C oracle (ed3b496) versus the
 * Rust-owned keyboard_init/suspend/resume/pointer_start/pointer_stop. Every
 * controller read and write, every callback-visible change of the 88-byte object,
 * every return value and the final object are compared. */
_Static_assert(sizeof(struct keyboard)==88,"keyboard ABI");
/* Exact helper-free coverage: sections 0-5 (see main). */
#define LIFECYCLE_CASES 25133
extern int keyboard_init_reference(struct keyboard *,const struct keyboard_io *);
extern char keyboard_poll_reference(struct keyboard *);
extern int keyboard_suspend_reference(struct keyboard *);
extern int keyboard_resume_reference(struct keyboard *);
extern int keyboard_pointer_start_reference(struct keyboard *);
extern void keyboard_pointer_stop_reference(struct keyboard *);
extern int keyboard_pointer_poll_reference(struct keyboard *,struct pointer_packet *);
static void die(const char *s);
static uint64_t th,tn,reads,cases,scenarios,max_resp,max_writes;static int bench;
static struct keyboard *active;static unsigned char last[88];
#define MAXREADS 4000000ULL
static void tr(uint32_t e){if(!bench){th=(th^e)*UINT64_C(0x100000001b3);tn++;}}
/* Callback-visible object snapshot: folded into the trace whenever it changed. */
static void snap(void){
 if(bench||!active||!memcmp(active,last,88))return;
 memcpy(last,active,88);tr(0x7fffffffu);for(unsigned i=0;i<88;i++)tr(0x40000000u|last[i]);
}
/* ---------------- programmable i8042 emulator ---------------- */
struct ent{uint8_t b,aux,err;};
static struct {struct ent q[64];unsigned head,count;uint8_t mode,last;int expect_mode,to_aux;uint32_t seed;unsigned faults,resp,writes;} em;
enum kind{K_NONE,K_VALUE,K_FLIP,K_ERR,K_DROP};
static struct {enum kind kind;unsigned at;uint8_t value;unsigned extra_at,extra_n,busy_w,busy_left,pre_n;uint64_t swap_after;} plan;
static const struct keyboard_io *swap_to;
static uint32_t er(void){em.seed^=em.seed<<13;em.seed^=em.seed>>17;em.seed^=em.seed<<5;return em.seed;}
static void push(uint8_t b,int aux,int err){if(em.count<64){unsigned i=(em.head+em.count)%64;em.q[i]=(struct ent){b,(uint8_t)aux,(uint8_t)err};em.count++;}}
static void respond(uint8_t b,int aux){
 unsigned i=em.resp++;int err=0;
 if(i==plan.at){
  if(plan.kind==K_VALUE)b=plan.value;else if(plan.kind==K_FLIP)aux^=1;else if(plan.kind==K_ERR)err=1;
  if(plan.kind!=K_DROP)push(b,aux,err);
 }else push(b,aux,0);
 if(i==plan.extra_at)for(unsigned n=0;n<plan.extra_n;n++)push(0x1e,0,0);
}
static uint8_t e_read(uint16_t port,uint32_t tag){
 uint8_t v;
 if(++reads>MAXREADS)die("emulator read bound");
 snap();
 if(port==0x64){
  v=0x10;int stall=(em.faults&1) && er()%8==0;
  if(em.count && !stall){v|=1;if(em.q[em.head].aux)v|=0x20;if(em.q[em.head].err||((em.faults&2) && er()%97==0))v|=0x80;}
  if(em.writes==plan.busy_w && plan.busy_left){plan.busy_left--;v|=0x02;}
  if((em.faults&8) && er()%61==0)v|=0x02;
 }else if(port==0x60){if(em.count){v=em.q[em.head].b;em.head=(em.head+1)%64;em.count--;em.last=v;}else v=em.last;}
 else die("unexpected read port");
 tr(tag|((uint32_t)port<<8)|v);
 if(plan.swap_after && reads==plan.swap_after && active)active->io=swap_to;
 return v;
}
static void e_write(uint16_t port,uint8_t v,uint32_t tag){
 snap();tr(0x80000000u|tag|((uint32_t)port<<8)|v);em.writes++;
 if(port==0x64){
  if(v==0x20)respond(em.mode,0);else if(v==0x60)em.expect_mode=1;else if(v==0xd4)em.to_aux=1;
  else if(v!=0xad && v!=0xa7 && v!=0xa8 && v!=0xae)die("unauthorized controller command");
 }else if(port==0x60){
  if(em.expect_mode){em.mode=v;em.expect_mode=0;}
  else if(em.to_aux){em.to_aux=0;if((em.faults&4) && er()%5==0)respond(0xfe,1);else{respond(0xfa,1);if(v==0xff){respond(0xaa,1);respond(0x00,1);}}}
  else if((em.faults&4) && er()%5==0)respond(0xfe,0);else respond(0xfa,0);
 }else die("unexpected write port");
 if((em.faults&16) && er()%7==0)push((uint8_t)er(),(int)(er()&1),0);
}
static uint8_t e_in(uint16_t p){return e_read(p,0);}
static void e_out(uint16_t p,uint8_t v){e_write(p,v,0);}
static uint8_t a_in(uint16_t p){return e_read(p,1u<<24);}
static void a_out(uint16_t p,uint8_t v){e_write(p,v,1u<<24);}
static const struct keyboard_io e_io={e_in,e_out},a_io={a_in,a_out},no_in={NULL,e_out},no_out={e_in,NULL};
static void die(const char *s){fprintf(stderr,"FAIL %s reads=%llu cases=%llu scenarios=%llu\n",s,(unsigned long long)reads,(unsigned long long)cases,(unsigned long long)scenarios);exit(1);}
/* ---------------- operation sequences ---------------- */
struct fns{int(*init)(struct keyboard*,const struct keyboard_io*);char(*poll)(struct keyboard*);int(*start)(struct keyboard*);
 void(*stop)(struct keyboard*);int(*ppoll)(struct keyboard*,struct pointer_packet*);int(*suspend)(struct keyboard*);int(*resume)(struct keyboard*);};
static const struct fns REF={keyboard_init_reference,keyboard_poll_reference,keyboard_pointer_start_reference,keyboard_pointer_stop_reference,
 keyboard_pointer_poll_reference,keyboard_suspend_reference,keyboard_resume_reference};
static const struct fns CAND={keyboard_init,keyboard_poll,keyboard_pointer_start,keyboard_pointer_stop,keyboard_pointer_poll,keyboard_suspend,keyboard_resume};
enum op{INIT,POLL,SUSPEND,RESUME,START,PPOLL,STOP,KEY,MOUSE};
struct outcome{int r[64];struct keyboard k;struct pointer_packet p[64];uint64_t h,n,reads;unsigned resp,writes,count;};
struct setup{const struct keyboard_io *io;int preset;uint32_t seed;unsigned faults;};
static void preset(struct keyboard *k,int p){
 memset(k,0x5a,sizeof *k);k->io=&e_io;
 if(p==1)k->ready=0;
 if(p==2)k->pointer_ready=0;
 if(p==3)k->io=NULL;
 if(p==4){k->ready=0;k->pointer_ready=0x5a5a5a5a;}
 if(p>=1){k->key_head=0;k->key_count=0;}
}
static void run(const struct fns *f,const uint8_t *ops,unsigned n,const struct setup *s,struct outcome *o){
 memset(&em,0,sizeof em);em.seed=s->seed|1;em.faults=s->faults;em.mode=0x47;memset(o,0,sizeof *o);
 for(unsigned i=0;i<plan.pre_n;i++)push(0x1e,0,0);
 preset(&o->k,s->preset);active=&o->k;memcpy(last,&o->k,88);
 th=UINT64_C(0xcbf29ce484222325);tn=0;reads=0;
 for(unsigned i=0;i<n;i++){
  switch(ops[i]){
  case INIT:o->r[i]=f->init(&o->k,s->io);break;
  case POLL:o->r[i]=f->poll(&o->k);break;
  case SUSPEND:o->r[i]=f->suspend(&o->k);break;
  case RESUME:o->r[i]=f->resume(&o->k);break;
  case START:o->r[i]=f->start(&o->k);break;
  case PPOLL:memset(&o->p[i],0x5a,sizeof o->p[i]);o->r[i]=f->ppoll(&o->k,&o->p[i]);break;
  case STOP:f->stop(&o->k);break;
  case KEY:push(0x1e,0,0);push(0x9e,0,0);break;
  case MOUSE:push(0x08,1,0);push(0x01,1,0);push(0xff,1,0);break;
  }
  snap();
 }
 o->h=th;o->n=tn;o->reads=reads;o->resp=em.resp;o->writes=em.writes;o->count=em.count;active=NULL;
}
static struct outcome A,B;
static unsigned saved_busy;
static void compare(const uint8_t *ops,unsigned n,const struct setup *s){
 saved_busy=plan.busy_left;
 run(&REF,ops,n,s,&A);plan.busy_left=saved_busy;run(&CAND,ops,n,s,&B);
 if(memcmp(A.r,B.r,sizeof A.r)||memcmp(&A.k,&B.k,sizeof A.k)||memcmp(A.p,B.p,sizeof A.p)||A.h!=B.h||A.n!=B.n||A.reads!=B.reads||A.resp!=B.resp||A.writes!=B.writes||A.count!=B.count)
  die("C/Rust lifecycle trace equivalence");
 if(B.resp>max_resp)max_resp=B.resp;
 if(B.writes>max_writes)max_writes=B.writes;
 cases++;
}
static void clear_plan(void){memset(&plan,0,sizeof plan);plan.at=plan.extra_at=plan.busy_w=~0u;}
static const uint8_t S_INIT[]={INIT,KEY,POLL},S_SUSPEND[]={INIT,KEY,SUSPEND,POLL},S_RESUME[]={INIT,SUSPEND,RESUME,KEY,POLL},
 S_START[]={INIT,START,MOUSE,PPOLL,KEY,POLL},S_STOP[]={INIT,START,MOUSE,PPOLL,STOP,KEY,POLL},
 S_CYCLE[]={INIT,SUSPEND,RESUME,SUSPEND,RESUME,KEY,POLL,START,STOP,SUSPEND,RESUME,START,MOUSE,PPOLL,STOP,START,STOP,KEY,POLL};
static const struct {const uint8_t *ops;unsigned n;} SEQ[6]={{S_INIT,3},{S_SUSPEND,4},{S_RESUME,5},{S_START,6},{S_STOP,7},{S_CYCLE,19}};
/* ---------------- benchmark ---------------- */
static uint64_t ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))die("clock");return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static volatile uint64_t sink;
static double timing(unsigned w,int cand){
 const struct fns *f=cand?&CAND:&REF;unsigned iters=4000;struct keyboard k;struct setup s={&e_io,0,1,0};clear_plan();
 uint64_t total=0;
 for(unsigned i=0;i<iters;i++){
  memset(&em,0,sizeof em);em.seed=1;em.mode=0x47;reads=0;
  if(w>=1 && w<=3){(void)f->init(&k,&e_io);if(w==3)(void)f->start(&k);}
  if(w==4){memset(&k,0,sizeof k);k.io=&e_io;k.ready=1;}
  uint64_t a=ns();int r=0;
  switch(w){
  case 0:r=f->init(&k,s.io);break;
  case 1:r=f->suspend(&k);r+=f->resume(&k);break;
  case 2:r=f->start(&k);break;
  case 3:f->stop(&k);break;
  case 4:r=f->resume(&k);break;           /* refused: already ready */
  default:r=f->init(&k,NULL);break;       /* refused: no io */
  }
  total+=ns()-a;sink+=(uint64_t)r+reads;
 }
 return (double)total/iters;
}
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"--bench")){
  bench=1;puts("workload,sample,C_ns,Rust_ns");
  for(unsigned w=0;w<6;w++){(void)timing(w,0);(void)timing(w,1);
   for(unsigned s=0;s<9;s++){double a,b;if(s&1){b=timing(w,1);a=timing(w,0);}else{a=timing(w,0);b=timing(w,1);}printf("%u,%u,%.4f,%.4f\n",w,s,a,b);}}
  return 0;
 }
 struct setup base={&e_io,0,1,0};
 /* 0. Fault-free sequences must succeed end to end. */
 clear_plan();compare(S_STOP,7,&base);if(!(B.r[0]==1&&B.r[1]==1&&B.r[3]==1))die("fault-free start/stop sequence");
 clear_plan();compare(S_CYCLE,19,&base);
 for(unsigned i=0;i<19;i++)if(S_CYCLE[i]<=START && S_CYCLE[i]!=POLL && B.r[i]!=1)die("fault-free repeated cycle");
 /* 1. Every response byte value, channel flip, ERR and drop at every response index. */
 for(unsigned q=0;q<6;q++){
  clear_plan();compare(SEQ[q].ops,SEQ[q].n,&base);unsigned R=B.resp;
  for(unsigned at=0;at<R;at++){
   for(unsigned v=0;v<256;v++){clear_plan();plan.kind=K_VALUE;plan.at=at;plan.value=(uint8_t)v;compare(SEQ[q].ops,SEQ[q].n,&base);}
   for(unsigned k=K_FLIP;k<=K_DROP;k++){clear_plan();plan.kind=(enum kind)k;plan.at=at;compare(SEQ[q].ops,SEQ[q].n,&base);}
  }
 }
 /* 2. Write-wait boundary at every write: 99,999 busy polls succeed, 100,000 time out. */
 for(unsigned q=0;q<5;q++){
  clear_plan();compare(SEQ[q].ops,SEQ[q].n,&base);unsigned W=B.writes;
  for(unsigned w=0;w<W;w++)for(unsigned b=99999;b<=100000;b++){clear_plan();plan.busy_w=w;plan.busy_left=b;compare(SEQ[q].ops,SEQ[q].n,&base);}
 }
 /* 3. Drain-limit edges: 31/32/33 bytes queued before init; 31/32/33 extra bytes after every response. */
 for(unsigned q=0;q<6;q++)for(unsigned n=31;n<=33;n++){
  clear_plan();plan.pre_n=n;compare(SEQ[q].ops,SEQ[q].n,&base);
  if(q==0 && B.r[0]!=(n<=32))die("init drain limit edge");
 }
 for(unsigned q=1;q<5;q++){
  clear_plan();compare(SEQ[q].ops,SEQ[q].n,&base);unsigned R=B.resp;
  for(unsigned at=0;at<R;at++)for(unsigned n=31;n<=33;n++){clear_plan();plan.extra_at=at;plan.extra_n=n;compare(SEQ[q].ops,SEQ[q].n,&base);}
 }
 /* 4. Callback substitutes the object's io table after N accesses (init keeps using the
  *    caller's table for its validity check and drain; helpers use the object's). */
 swap_to=&a_io;
 for(unsigned q=0;q<6;q++)for(unsigned s=1;s<=48;s++){clear_plan();plan.swap_after=s;plan.pre_n=s%3;compare(SEQ[q].ops,SEQ[q].n,&base);}
 /* 5. Edge states and incomplete io: every lifecycle op from preset objects. */
 { static const uint8_t one[5][1]={{SUSPEND},{RESUME},{START},{STOP},{POLL}};
   for(int p=0;p<=4;p++)for(unsigned o=0;o<5;o++){
    if(p==3 && o>=3)continue;        /* NULL io with pointer_ready/poll: undefined in the accepted C */
    if(p==0 && o==4)continue;        /* poll needs a valid queue; covered by presets 1-4 */
    struct setup s={&e_io,p,7,0};clear_plan();compare(one[o],1,&s);
   }
   static const uint8_t init_only[2]={INIT,SUSPEND};
   const struct keyboard_io *bad[3]={NULL,&no_in,&no_out};
   for(unsigned i=0;i<3;i++){struct setup s={bad[i],0,7,0};clear_plan();compare(init_only,1,&s);
    if(B.r[0]!=0||B.k.io!=bad[i]||B.reads||B.writes)die("incomplete io must be rejected before any access");}
 }
 /* 6. Random repeated operation sequences under fault masks 0..31. */
 for(unsigned i=0;i<3000;i++){
  uint8_t ops[24];uint32_t r=0x9e3779b9u*(i+1);ops[0]=INIT;
  for(unsigned j=1;j<24;j++){r^=r<<13;r^=r>>17;r^=r<<5;ops[j]=(uint8_t)(1+r%8);}
  struct setup s={&e_io,0,r|1,i%32};clear_plan();compare(ops,24,&s);scenarios++;
 }
 /* 7. Rust-only refusals of invalid objects (the accepted C has no defined behaviour): no access. */
 { reads=0;em.writes=0;struct keyboard k;memset(&k,0x5a,sizeof k);active=NULL;
   void *bad[3]={NULL,(char*)&k+1,(void*)(UINTPTR_MAX-7)};
   for(unsigned i=0;i<3;i++){
    if(vos_rust_keyboard_init(bad[i],&e_io)||vos_rust_keyboard_suspend(bad[i])||vos_rust_keyboard_resume(bad[i])||vos_rust_keyboard_pointer_start(bad[i]))die("invalid object accepted");
    vos_rust_keyboard_pointer_stop(bad[i]);
   }
   unsigned char pat[88];memset(pat,0x5a,88);
   if(reads||em.writes||memcmp(&k,pat,88))die("invalid object refusal touched hardware or memory");
   long page=sysconf(_SC_PAGESIZE);unsigned char *mp=mmap(0,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
   if(mp==MAP_FAILED || mprotect(mp+page,(size_t)page,PROT_READ|PROT_WRITE))die("guard pages");
   struct keyboard *edge=(void*)(mp+2*page-sizeof *edge);memset(&em,0,sizeof em);em.seed=1;em.mode=0x47;clear_plan();active=NULL;
   if(vos_rust_keyboard_init(edge,&e_io)!=1||vos_rust_keyboard_pointer_start(edge)!=1)die("guard-page edge object");
   vos_rust_keyboard_pointer_stop(edge);
   if(vos_rust_keyboard_suspend(edge)!=1||vos_rust_keyboard_resume(edge)!=1)die("guard-page edge suspend/resume");
   if(munmap(mp,(size_t)page*3))die("unmap");
 }
 if(cases!=LIFECYCLE_CASES || scenarios!=3000)die("comparison coverage count");
 printf("PASS %llu lifecycle comparisons and %llu random fault scenarios; max %llu responses/%llu writes per sequence; every response value/flip/ERR/drop, 99999/100000 write waits, drain edges 31/32/33, io substitution, edge states, read+write+object traces\n",
        (unsigned long long)cases,(unsigned long long)scenarios,(unsigned long long)max_resp,(unsigned long long)max_writes);
 return 0;
}
