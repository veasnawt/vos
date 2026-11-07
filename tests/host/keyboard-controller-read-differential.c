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
/* R2 bounded controller reads: frozen accepted C oracle versus the Rust-owned
 * read_keyboard()/read_aux(), directly and through every lifecycle caller. */
_Static_assert(sizeof(struct keyboard)==88,"keyboard ABI");
extern int read_keyboard_reference(struct keyboard *,uint8_t *);
extern int read_aux_reference(struct keyboard *,uint8_t *);
extern int keyboard_init_reference(struct keyboard *,const struct keyboard_io *);
extern char keyboard_poll_reference(struct keyboard *);
extern int keyboard_suspend_reference(struct keyboard *);
extern int keyboard_resume_reference(struct keyboard *);
extern int keyboard_pointer_start_reference(struct keyboard *);
extern void keyboard_pointer_stop_reference(struct keyboard *);
extern int keyboard_pointer_poll_reference(struct keyboard *,struct pointer_packet *);
static void die(const char *s);
static uint64_t th,tn,reads,cases,scenarios;static int bench;static struct keyboard *active;
#define MAXREADS 2000000ULL
static void tr(uint32_t e){if(!bench){th=(th^e)*UINT64_C(0x100000001b3);tn++;}}
/* ---------------- helper-level scripted controller ---------------- */
struct cell{uint8_t status,data;};
static struct cell cells[128];static unsigned ncells,at;static struct cell fill;static uint64_t swap_at,delay_init,delay_left;
static uint8_t scripted(uint16_t port,int other);
static uint8_t s_in(uint16_t p){return scripted(p,0);}
static uint8_t s_alt(uint16_t p){return scripted(p,1);}
static void s_out(uint16_t p,uint8_t v){(void)p;(void)v;die("unexpected controller write in a read helper");}
static const struct keyboard_io s_io={s_in,s_out},s_alt_io={s_alt,s_out};
/* A status read consumes its cell when OBF is clear (no data follows);
 * otherwise the following data read consumes it. */
static uint8_t scripted(uint16_t port,int other){
 if(port!=0x64 && port!=0x60)die("unexpected read port");
 if(++reads>MAXREADS)die("read bound");
 struct cell c=at<ncells?cells[at]:fill;uint8_t v;
 if(port==0x64 && delay_left){delay_left--;v=0;}
 else if(port==0x64){v=c.status;if(!(v&1))at++;}else{v=c.data;at++;}
 tr(((uint32_t)other<<24)|((uint32_t)port<<8)|v);
 if(swap_at && reads==swap_at)active->io=&s_alt_io;
 return v;
}
static void die(const char *s){fprintf(stderr,"FAIL %s at=%u reads=%llu cases=%llu\n",s,at,(unsigned long long)reads,(unsigned long long)cases);exit(1);}
static uint64_t rng=UINT64_C(0x72326374726c7264);
static uint32_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)rng;}
static struct keyboard object(void){struct keyboard k;memset(&k,0x5a,sizeof k);k.io=&s_io;k.ready=1;k.pointer_ready=0;return k;}
static void run_helper(int aux,int rust,struct keyboard *k,uint8_t *v,int *r,uint64_t *h,uint64_t *n,unsigned *a){
 active=k;at=0;reads=0;delay_left=delay_init;th=UINT64_C(0xcbf29ce484222325);tn=0;
 *r=rust?(aux?vos_rust_keyboard_read_aux(k,v):vos_rust_keyboard_read_keyboard(k,v)):(aux?read_aux_reference(k,v):read_keyboard_reference(k,v));
 *h=th;*n=tn;*a=at;
}
static int compare_helper(int aux){
 struct keyboard a=object(),b=a;uint8_t va=0xa5,vb=0xa5;int ra,rb;uint64_t ha,hb,na,nb;unsigned aa,ab;
 run_helper(aux,0,&a,&va,&ra,&ha,&na,&aa);run_helper(aux,1,&b,&vb,&rb,&hb,&nb,&ab);
 if(ra!=rb||va!=vb||ha!=hb||na!=nb||aa!=ab||memcmp(&a,&b,sizeof a))die("C/Rust helper equivalence");
 if(!ra && va!=0xa5)die("output written on failure");
 cases++;
 return ra && va==0x5a;
}
/* ---------------- lifecycle-level i8042 emulator ---------------- */
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
static void workload(unsigned w){
 memset(cells,0,sizeof cells);ncells=0;fill=(struct cell){0,0};swap_at=0;
 if(w==0){cells[0]=(struct cell){1,0xfa};ncells=1;}
 if(w==1){cells[0]=(struct cell){0x21,0xfa};ncells=1;}
 if(w==2){for(unsigned i=0;i<10;i++)cells[i]=(struct cell){0,0};cells[10]=(struct cell){1,0xfa};ncells=11;}
 if(w==3){cells[0]=(struct cell){0x21,8};cells[1]=(struct cell){1,0xfa};ncells=2;}
 if(w==4){cells[0]=(struct cell){0x41,0};ncells=1;}
}
static double timing(unsigned w,int rust){
 unsigned iters=w==5?20:20000;int aux=w==1;workload(w);struct keyboard k=object();uint64_t a=ns();
 for(unsigned i=0;i<iters;i++){uint8_t v=0;active=&k;at=0;reads=0;int r=rust?(aux?vos_rust_keyboard_read_aux(&k,&v):vos_rust_keyboard_read_keyboard(&k,&v)):(aux?read_aux_reference(&k,&v):read_keyboard_reference(&k,&v));sink+=(uint64_t)r+v+reads;}
 return (double)(ns()-a)/iters;
}
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"--bench")){
  bench=1;puts("workload,sample,C_ns,Rust_ns");
  for(unsigned w=0;w<6;w++){(void)timing(w,0);(void)timing(w,1);
   for(unsigned s=0;s<9;s++){double a,b;if(s&1){b=timing(w,1);a=timing(w,0);}else{a=timing(w,0);b=timing(w,1);}printf("%u,%u,%.4f,%.4f\n",w,s,a,b);}}
  return 0;
 }
 for(int aux=0;aux<2;aux++){
  struct cell term={(uint8_t)(aux?0x21:1),0x5a};
  /* 1. Every first (status,data) cell, then a terminating byte of the requested channel. */
  for(unsigned s=0;s<256;s++)for(unsigned d=0;d<256;d++){cells[0]=(struct cell){(uint8_t)s,(uint8_t)d};ncells=1;fill=term;swap_at=0;compare_helper(aux);}
  /* 2. Exact timeout: OBF never set (100,000 status reads); other-channel flood (200,000 reads). */
  ncells=0;fill=(struct cell){0,0};compare_helper(aux);
  { struct keyboard k=object();uint8_t v=0xa5;int r;uint64_t h,n;unsigned a;run_helper(aux,1,&k,&v,&r,&h,&n,&a);if(r||v!=0xa5||reads!=100000)die("exact 100,000-status-read timeout"); }
  fill=(struct cell){(uint8_t)(aux?1:0x21),0x77};compare_helper(aux);
  { struct keyboard k=object();uint8_t v=0xa5;int r;uint64_t h,n;unsigned a;run_helper(aux,1,&k,&v,&r,&h,&n,&a);if(r||v!=0xa5||reads!=200000)die("other-channel flood consumed for exactly 100,000 iterations"); }
  /* 3. Delayed success: the byte appears after N OBF-clear status reads (limit is 100,000 status reads). */
  const unsigned delays[7]={0,1,2,1000,99998,99999,100000};
  for(unsigned i=0;i<7;i++){cells[0]=term;ncells=1;fill=term;swap_at=0;delay_init=delays[i];
   if(compare_helper(aux)!=(delays[i]<=99999))die("wait-limit boundary");}
  delay_init=0;
  /* 4. Deterministic random interleavings (OBF-clear, other channel, ERR, requested). */
  for(unsigned i=0;i<40000;i++){unsigned m=1+next()%64;for(unsigned j=0;j<m;j++)cells[j]=(struct cell){(uint8_t)next(),(uint8_t)next()};ncells=m;fill=term;swap_at=0;compare_helper(aux);}
  /* 5. Callback substitutes io mid-wait: every later read must use the new callback. */
  for(unsigned w=1;w<=8;w++)for(unsigned i=0;i<16;i++){unsigned m=4+next()%8;for(unsigned j=0;j<m;j++)cells[j]=(struct cell){(uint8_t)(next()&0x21),(uint8_t)next()};ncells=m;fill=term;swap_at=w;compare_helper(aux);}
  swap_at=0;
  /* 6. Boundary objects: refused without any read. */
  ncells=0;fill=term;reads=0;uint8_t v=0xa5;struct keyboard k=object();active=&k;
  if((aux?vos_rust_keyboard_read_aux(NULL,&v):vos_rust_keyboard_read_keyboard(NULL,&v))!=0 || reads)die("NULL keyboard accepted");
  if((aux?vos_rust_keyboard_read_aux(&k,NULL):vos_rust_keyboard_read_keyboard(&k,NULL))!=0 || reads)die("NULL output accepted");
  if((aux?vos_rust_keyboard_read_aux((void*)((char*)&k+1),&v):vos_rust_keyboard_read_keyboard((void*)((char*)&k+1),&v))!=0 || reads)die("misaligned keyboard accepted");
  if((aux?vos_rust_keyboard_read_aux((void*)(UINTPTR_MAX-7),&v):vos_rust_keyboard_read_keyboard((void*)(UINTPTR_MAX-7),&v))!=0 || reads || v!=0xa5)die("wrapping keyboard accepted");
  long page=sysconf(_SC_PAGESIZE);unsigned char *mp=mmap(0,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(mp==MAP_FAILED || mprotect(mp+page,(size_t)page,PROT_READ|PROT_WRITE))die("guard pages");
  struct keyboard *edge=(void*)(mp+2*page-sizeof *edge);*edge=object();active=edge;at=0;reads=0;ncells=0;fill=term;
  if((aux?vos_rust_keyboard_read_aux(edge,&v):vos_rust_keyboard_read_keyboard(edge,&v))!=1 || v!=0x5a || reads!=2)die("guard-page edge object");
  if(munmap(mp,(size_t)page*3))die("unmap");
 }
 /* 7. Lifecycle callers through an i8042 emulator, read AND write traces, fault masks 0..31. */
 for(unsigned i=0;i<3000;i++)compare_lifecycle(0x9e3779b9u*(i+1),i%32);
 /* Coverage: per channel 65536 + 2 + 7 + 40000 + 128 = 105673; x2 = 211346 helper comparisons. */
 if(cases!=211346 || scenarios!=3000)die("comparison coverage count");
 printf("PASS %llu controller read comparisons and %llu lifecycle scenarios; seed=72326374726c7264; exhaustive first cells, exact 100000 wait limit, other-channel flood, delayed success, io swap, boundary objects, read+write traces; no helper controller writes\n",
        (unsigned long long)cases,(unsigned long long)scenarios);
 return 0;
}
