#define _GNU_SOURCE
#include "net_tcp.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* R3-N2: the frozen accepted C TCP (f8731c9) in two configurations versus the
 * Rust-owned TCP core. A = frozen C TCP over the frozen C ARP/IPv4 (879c511),
 * B = frozen C TCP over the production Rust IPv4 (R3-N1), R = production
 * net_tcp.c forwarding to Rust. Every call runs on three identical 8 KiB
 * arenas (context, frame, output, length, data and address slots); the return
 * value and the ENTIRE arena must match, which checks state, output bytes and
 * lengths, NOSPACE/invalid-input immutability and aliasing at once. Contexts
 * are reached only through the public API, so the C oracle never sees a state
 * outside its invariants. */
_Static_assert(sizeof(struct net_tcp)==504 && _Alignof(struct net_tcp)==8,"ABI");
_Static_assert(offsetof(struct net_tcp,next_isn)==288 && offsetof(struct net_tcp,state)==336 && offsetof(struct net_tcp,closure_enabled)==364 &&
               offsetof(struct net_tcp,tx_since)==368 && offsetof(struct net_tcp,rx)==376 && offsetof(struct net_tcp,tx)==440,"layout");
#define TCP_CASES 374029ULL   /* pinned exact comparison count of stages 0-6 */
#define TCP_SEQUENCES 4000u

#define DECL(S) \
 int net_tcp_init##S(struct net_tcp*,const uint8_t*,const uint8_t*,uint16_t,uint32_t); \
 int net_tcp_input##S(struct net_tcp*,const void*,size_t,uint64_t,void*,size_t,size_t*); \
 int net_tcp_expire##S(struct net_tcp*,uint64_t); void net_tcp_close##S(struct net_tcp*); \
 int net_tcp_enable_payload##S(struct net_tcp*); \
 int net_tcp_send##S(struct net_tcp*,const void*,size_t,uint64_t,void*,size_t,size_t*); \
 int net_tcp_receive##S(struct net_tcp*,void*,size_t,size_t*,uint64_t); \
 int net_tcp_ack##S(struct net_tcp*,uint64_t,void*,size_t,size_t*); void net_tcp_abort##S(struct net_tcp*); \
 int net_tcp_enable_retransmission##S(struct net_tcp*); \
 int net_tcp_retransmit##S(struct net_tcp*,uint64_t,void*,size_t,size_t*); \
 int net_tcp_enable_closure##S(struct net_tcp*); \
 int net_tcp_finish##S(struct net_tcp*,uint64_t,void*,size_t,size_t*);
DECL(_rA) DECL(_rB)
typedef struct {
 int (*init)(struct net_tcp*,const uint8_t*,const uint8_t*,uint16_t,uint32_t);
 int (*input)(struct net_tcp*,const void*,size_t,uint64_t,void*,size_t,size_t*);
 int (*expire)(struct net_tcp*,uint64_t); void (*close)(struct net_tcp*); int (*epay)(struct net_tcp*);
 int (*send)(struct net_tcp*,const void*,size_t,uint64_t,void*,size_t,size_t*);
 int (*recv)(struct net_tcp*,void*,size_t,size_t*,uint64_t);
 int (*ack)(struct net_tcp*,uint64_t,void*,size_t,size_t*); void (*abort)(struct net_tcp*);
 int (*eretx)(struct net_tcp*); int (*retx)(struct net_tcp*,uint64_t,void*,size_t,size_t*);
 int (*eclose)(struct net_tcp*); int (*finish)(struct net_tcp*,uint64_t,void*,size_t,size_t*);
} impl;
#define IMPL(S) {net_tcp_init##S,net_tcp_input##S,net_tcp_expire##S,net_tcp_close##S,net_tcp_enable_payload##S,net_tcp_send##S,net_tcp_receive##S, \
                 net_tcp_ack##S,net_tcp_abort##S,net_tcp_enable_retransmission##S,net_tcp_retransmit##S,net_tcp_enable_closure##S,net_tcp_finish##S}
static const impl I[3]={IMPL(_rA),IMPL(_rB),IMPL()};

static unsigned long long cases[8],sequences;static int stage,bench;
static uint64_t rng=UINT64_C(0x52334e3254435021);
static uint32_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)rng;}
/* ---------------- arenas ---------------- */
#define ARENA 8192
enum {CTX=0,FRAME=1024,OUT=2624,LEN=4160,DATA=4224,IN2=4352,NUL=-1};
static _Alignas(64) unsigned char arena[3][ARENA],tmpl[ARENA];
static struct net_tcp *ctx(int s){return (struct net_tcp*)arena[s];}
static void *at(int s,long off){return off<0?NULL:arena[s]+off;}
static void save(void){memcpy(tmpl,arena[0],ARENA);}
static void restore(void){for(int s=0;s<3;s++)memcpy(arena[s],tmpl,ARENA);}
static const char *what="";
static unsigned char seen_state[10],seen_rc[32];   /* coverage: states reached and return codes produced (A) */
static void check(const int r[3]){
 for(int s=1;s<3;s++){
  if(r[s]!=r[0]){fprintf(stderr,"FAIL %s stage %d case %llu: impl %d returned %d, A returned %d\n",what,stage,cases[stage],s,r[s],r[0]);exit(1);}
  if(memcmp(arena[0],arena[s],ARENA)){size_t k=0;while(arena[0][k]==arena[s][k])k++;
   fprintf(stderr,"FAIL %s stage %d case %llu: impl %d arena differs at %zu (A %02x, %d %02x) rc %d\n",what,stage,cases[stage],s,k,arena[0][k],s,arena[s][k],r[0]);exit(1);}
 }
 if(!bench){cases[stage]++;unsigned st=(unsigned)ctx(0)->state;if(st<10)seen_state[st]=1;if(r[0]>=-12&&r[0]<=19)seen_rc[r[0]+12]=1;}
}
/* ---------------- one public call on all three arenas ---------------- */
enum {INIT,INPUT,EXPIRE,CLOSE,EPAY,SEND,RECV,ACKN,ABORT,ERETX,RETX,ECLOSE,FINISH};
typedef struct {int op;uint64_t now;long out,len,data,mac,ip;size_t cap,n;uint16_t port;uint32_t isn;} call_t;
static int call1(int s,const call_t *k){
 const impl *m=&I[s];struct net_tcp *c=ctx(s);
 switch(k->op){
 case INIT:return m->init(c,at(s,k->mac),at(s,k->ip),k->port,k->isn);
 case INPUT:return m->input(c,at(s,FRAME),k->n,k->now,at(s,k->out),k->cap,at(s,k->len));
 case EXPIRE:return m->expire(c,k->now);
 case CLOSE:m->close(c);return 0;
 case EPAY:return m->epay(c);
 case SEND:return m->send(c,at(s,k->data),k->n,k->now,at(s,k->out),k->cap,at(s,k->len));
 case RECV:return m->recv(c,at(s,k->out),k->cap,at(s,k->len),k->now);
 case ACKN:return m->ack(c,k->now,at(s,k->out),k->cap,at(s,k->len));
 case ABORT:m->abort(c);return 0;
 case ERETX:return m->eretx(c);
 case RETX:return m->retx(c,k->now,at(s,k->out),k->cap,at(s,k->len));
 case ECLOSE:return m->eclose(c);
 default:return m->finish(c,k->now,at(s,k->out),k->cap,at(s,k->len));
 }
}
static int run(const call_t *k){int r[3];for(int s=0;s<3;s++)r[s]=call1(s,k);check(r);return r[0];}
static call_t mk(int op,uint64_t now){call_t k={op,now,OUT,LEN,DATA,IN2,IN2+8,1514,0,4001,0};return k;}
static int run1(int op){call_t k=mk(op,0);return run(&k);}
/* ---------------- frames (independent of every implementation) ---------------- */
static const uint8_t G[6]={0x52,0x54,0,0x12,0x34,0x56},P[6]={0x52,0x54,0,0xaa,0xbb,0xcc},GI[4]={10,0,2,15},PI[4]={10,0,2,2};
static void put(uint8_t *p,unsigned x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void put32(uint8_t *p,uint32_t x){p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static uint16_t csum(const uint8_t *p,size_t n,uint32_t s){for(size_t i=0;i<n;i++){s+=(uint32_t)p[i]<<((i&1)?0:8);s=(s&65535)+(s>>16);}return (uint16_t)~s;}
static void fix_ip(uint8_t *f){put(f+24,0);put(f+24,csum(f+14,(size_t)(f[14]&15)*4,0));}
static void fix_tcp(uint8_t *f){size_t ihl=(size_t)(f[14]&15)*4,tot=(size_t)f[16]<<8|f[17];if(tot<ihl||tot>1500)return;size_t n=tot-ihl;uint8_t *t=f+14+ihl;
 uint8_t ph[12];memcpy(ph,f+26,4);memcpy(ph+4,f+30,4);ph[8]=0;ph[9]=f[23];put(ph+10,(unsigned)n);
 put(t+16,0);uint32_t s=0;for(int i=0;i<12;i++){s+=(uint32_t)ph[i]<<((i&1)?0:8);s=(s&65535)+(s>>16);}put(t+16,csum(t,n,s));}
static uint8_t F[1600];static size_t FL;
/* Segment from the peer (P/PI:5001) to the endpoint (G/GI:4001). */
static size_t seg(unsigned flags,uint32_t seq,uint32_t ack,unsigned win,const uint8_t *data,size_t n){
 size_t wire=54+n<60?60:54+n;memset(F,0,sizeof F);memcpy(F,G,6);memcpy(F+6,P,6);put(F+12,0x0800);
 uint8_t *h=F+14;h[0]=0x45;put(h+2,(unsigned)(40+n));put(h+4,0x2233);put(h+6,0x4000);h[8]=64;h[9]=6;memcpy(h+12,PI,4);memcpy(h+16,GI,4);
 uint8_t *t=F+34;put(t,5001);put(t+2,4001);put32(t+4,seq);put32(t+8,ack);t[12]=0x50;t[13]=(uint8_t)flags;put(t+14,win);if(n)memcpy(t+20,data,n);
 fix_ip(F);fix_tcp(F);FL=wire;return wire;
}
static int input(uint64_t now){for(int s=0;s<3;s++)memcpy(arena[s]+FRAME,F,FL);call_t k=mk(INPUT,now);k.n=FL;return run(&k);}
static int input_k(call_t k){for(int s=0;s<3;s++)memcpy(arena[s]+FRAME,F,sizeof F);return run(&k);}
static uint8_t D[128],DB[1500];   /* D fits the arena DATA slot; DB backs maximum-size frames */
/* ---------------- scenario setup through the public API ---------------- */
static uint64_t T;   /* scenario clock */
static void fresh(uint32_t isn,int pay,int retx,int clo){
 for(int s=0;s<3;s++){memset(arena[s],0x5a,ARENA);memcpy(arena[s]+IN2,G,6);memcpy(arena[s]+IN2+8,GI,4);memcpy(arena[s]+DATA,D,sizeof D);}
 call_t k=mk(INIT,0);k.isn=isn;if(run(&k))exit(2);
 if(pay&&run1(EPAY))exit(3);
 if(retx&&run1(ERETX))exit(4);
 if(clo&&run1(ECLOSE))exit(5);
}
static struct net_tcp *C0(void){return ctx(0);}
/* SYN then ACK: returns 1 when ESTABLISHED. */
static int establish(uint32_t peer,unsigned win){
 seg(2,peer,0,1024,0,0);if(input(T)!=NET_TCP_SYNACK)return 0;
 seg(16,peer+1,C0()->snd_next,win,0,0);return input(T+1)==NET_TCP_CONNECTED;
}
static int send_n(size_t n,uint64_t now){call_t k=mk(SEND,now);k.n=n;return run(&k);}
static int peer_data(size_t n,uint64_t now){seg(n?24:16,C0()->rcv_next,C0()->snd_next,1024,D+7,n);return input(now);}
/* ---------------- stage 0: init and profile admission ---------------- */
static void stage0(void){
 stage=0;what="init/enable";
 static const uint8_t macs[4][6]={{0x52,0x54,0,0x12,0x34,0x56},{1,0,0,0,0,1},{0,0,0,0,0,0},{0x52,0x54,0,0xaa,0xbb,0xcc}};
 static const uint8_t ips[5][4]={{10,0,2,15},{0,1,2,3},{127,0,0,1},{224,0,0,1},{10,0,2,2}};
 static const uint32_t isns[5]={0,1,0x7fffffff,0x80000000,0xffffffff};
 for(int m=0;m<4;m++)for(int i=0;i<5;i++)for(int p=0;p<3;p++)for(int q=0;q<5;q++){
  for(int s=0;s<3;s++){memset(arena[s],0x5a,ARENA);memcpy(arena[s]+IN2,macs[m],6);memcpy(arena[s]+IN2+8,ips[i],4);}
  call_t k=mk(INIT,0);k.port=(uint16_t)(p==0?0:p==1?1:4001);k.isn=isns[q];run(&k);
  k.mac=NUL;run(&k);k.mac=IN2;k.ip=NUL;run(&k);}
 /* addresses aliasing the context (the C copies them first) */
 fresh(7,0,0,0);{call_t k=mk(INIT,0);k.mac=CTX;k.ip=CTX+6;run(&k);run(&k);}
 /* enable ordering and state gating */
 for(int order=0;order<27;order++){fresh(9,0,0,0);int ops[3]={EPAY+0,ERETX,ECLOSE};
  for(int j=0,o=order;j<3;j++,o/=3){run1(ops[o%3]);}}
 fresh(9,1,0,0);T=100;seg(2,500,0,1024,0,0);input(T);for(int op=EPAY;op<=FINISH;op++)if(op==EPAY||op==ERETX||op==ECLOSE)run1(op);
 /* closed (port 0) context: every entry */
 fresh(9,1,1,1);run1(CLOSE);
 for(int op=INIT+1;op<=FINISH;op++){call_t k=mk(op,5);k.n=op==SEND?4:60;if(op==INPUT){seg(2,1,0,1,0,0);input_k(k);}else run(&k);}
 run1(CLOSE);run1(ABORT);
}
/* ---------------- stage 1: LISTEN input, header validation, output admission ---------------- */
static void listen_sweeps(int pay,int clo){
 uint32_t isn=0xfffffff0u;fresh(isn,pay,0,clo);T=1000;save();
 for(unsigned fl=0;fl<256;fl++)for(int a=0;a<2;a++){restore();seg(fl,0x1000,a?5:0,512,0,0);input(T);}
 seg(2,0xffffffffu,0,512,0,0);uint8_t base[60];memcpy(base,F,60);
 for(size_t off=0;off<60;off++)for(unsigned b=0;b<8;b++)for(int fixi=0;fixi<2;fixi++)for(int fixt=0;fixt<2;fixt++){
  restore();memcpy(F,base,60);F[off]^=(uint8_t)(1u<<b);if(fixi)fix_ip(F);if(fixt)fix_tcp(F);FL=60;input(T);}
 for(size_t l=0;l<=70;l++){restore();memcpy(F,base,60);memset(F+60,0,10);FL=l;input(T);}
 for(unsigned doff=0;doff<16;doff++)for(size_t n=0;n<=12;n+=4){restore();seg(2,77,0,64,D,n);F[46]=(uint8_t)(doff<<4);fix_tcp(F);input(T);}
 for(unsigned tot=0;tot<=80;tot++){restore();seg(2,77,0,64,D,8);put(F+16,tot);fix_ip(F);input(T);}
 { restore();seg(2,77,0,64,0,0);put(F+52,1);fix_tcp(F);input(T);   /* urgent pointer */
   restore();seg(2,77,0,64,0,0);put(F+34,0);fix_tcp(F);input(T);   /* source port 0 */
   restore();seg(2,77,0,64,0,0);put(F+36,0);fix_tcp(F);input(T);   /* destination port 0 */
   restore();seg(2,77,0,64,0,0);put(F+36,4002);fix_tcp(F);input(T); /* other port */
   restore();seg(2,77,0,64,D,1);input(T);                           /* SYN with data */
   restore();seg(2,77,0,64,D,64);input(T);restore();seg(2,77,0,64,DB,1460);input(T); }
 /* output admission and aliasing on the SYN-ACK path */
 static const long outs[3]={OUT,NUL,FRAME};static const long lens[4]={LEN,NUL,OUT+8,FRAME+8};   /* aligned: a misaligned size_t* is outside the C contract */static const size_t caps[4]={1514,60,59,0};
 for(int o=0;o<3;o++)for(int l=0;l<4;l++)for(int c=0;c<4;c++)for(uint64_t w=0;w<2;w++){restore();seg(2,0x80000000u,0,1024,0,0);
  call_t k=mk(INPUT,w?UINT64_MAX:T);k.n=FL;k.out=outs[o];k.len=lens[l];k.cap=caps[c];input_k(k);}
 /* non-TCP local frames are delegated to the IPv4/ARP input */
 { restore();uint8_t *f=F;memset(f,0,60);memcpy(f,(const uint8_t[6]){255,255,255,255,255,255},6);memcpy(f+6,P,6);put(f+12,0x0806);put(f+14,1);put(f+16,0x0800);f[18]=6;f[19]=4;put(f+20,1);
   memcpy(f+22,P,6);memcpy(f+28,PI,4);memcpy(f+38,GI,4);FL=60;input(T);
   for(unsigned proto=0;proto<256;proto+=(proto<20?1:17)){restore();seg(2,1,0,1,D,16);F[23]=(uint8_t)proto;fix_ip(F);input(T);}
   restore();seg(2,1,0,1,0,0);uint8_t *h=F+14;h[9]=1;F[34]=8;F[35]=0;put(F+36,0);put(F+38,7);put(F+40,9);put(F+36,csum(F+34,26,0));fix_ip(F);input(T); }
 /* expiry with no connection, repeated aborts, and timers at the clock edges */
 for(int t=0;t<4;t++){restore();call_t k=mk(EXPIRE,t==0?0:t==1?UINT64_MAX:t==2?T+200:T+400);run(&k);run1(ABORT);run(&k);}
}
static void stage1(void){stage=1;what="listen";for(int pay=0;pay<2;pay++)for(int clo=0;clo<=pay;clo++)listen_sweeps(pay,clo);}
/* ---------------- stage 2: SYN-RECEIVED: completion, TTL, RST and peer identity ---------------- */
static void stage2(void){
 stage=2;what="syn-received";
 static const uint32_t peers[4]={0,0x7fffffff,0xfffffffe,0x12345678};
 static const int32_t ds[9]={0,1,-1,2,-2,0x7fffffff,(int32_t)0x80000000,0x40000000,64};
 for(int pay=0;pay<2;pay++)for(int clo=0;clo<=pay;clo++)for(int pi=0;pi<4;pi++)for(int ii=0;ii<2;ii++){
  uint32_t isn=ii?0x7ffffffeu:0xffffffffu;fresh(isn,pay,0,clo);T=ii?UINT64_MAX-100:5000;
  seg(2,peers[pi],0,1024,0,0);if(input(T)!=NET_TCP_SYNACK)exit(6);save();
  uint32_t rn=C0()->rcv_next,sn=C0()->snd_next;
  for(int a=0;a<9;a++)for(int b=0;b<9;b++){restore();seg(16,rn+(uint32_t)ds[a],sn+(uint32_t)ds[b],777,0,0);input(T+50);}
  for(unsigned fl=0;fl<256;fl++){restore();seg(fl,rn,sn,1000,0,0);input(T+10);}
  for(uint64_t dt=198;dt<=201;dt++){restore();seg(16,rn,sn,10,0,0);input(T+dt);restore();seg(4,rn,0,0,0,0);input(T+dt);restore();call_t k=mk(EXPIRE,T+dt);run(&k);}
  for(int v=0;v<6;v++){restore();seg(v<3?16:4,rn,v<3?sn:0,64,0,0);if(v%3==0)put(F+34,5002);if(v%3==1){F[29]^=1;}if(v%3==2)F[11]^=2;fix_ip(F);fix_tcp(F);input(T+3);}
  for(int v=0;v<4;v++){restore();seg(v<2?4:20,rn+(uint32_t)(v&1),v<2?0:sn,0,v==3?D:0,v==3?4:0);input(T+3);}
  restore();seg(2,peers[pi]+9,0,64,0,0);input(T+5);   /* second SYN while half-open */
  restore();seg(24,rn,sn,64,D,4);input(T+5);          /* data before establishment */
 }
}
/* ---------------- stage 3: ESTABLISHED payload profile: sequence/ACK serial tests, windows, data ---------------- */
static const int32_t DSEQ[12]={0,1,-1,63,64,65,0x7fffffff,(int32_t)0x80000000,(int32_t)0x80000001,-64,-65,2};
static void payload_sweeps(uint32_t isn,uint32_t peer,uint64_t t0,int retx,int clo){
 fresh(isn,1,retx,clo);T=t0;if(!establish(peer,64))exit(7);
 for(int fill=0;fill<5;fill++)for(int out=0;out<3;out++){
  static const size_t fills[5]={0,1,32,63,64},outs[3]={0,1,64};
  fresh(isn,1,retx,clo);T=t0;establish(peer,64);
  if(fills[fill])peer_data(fills[fill],T+2);
  if(outs[out])send_n(outs[out],T+3);
  save();
  for(int a=0;a<12;a++)for(int b=0;b<12;b++)for(size_t n=0;n<=65;n+=(n<3||n>61)?1:20){
   restore();seg(n?24:16,C0()->rcv_next+(uint32_t)DSEQ[a],C0()->snd_una+(uint32_t)DSEQ[b],(unsigned)(b*997),D+3,n);input(T+4);}
  for(unsigned w=0;w<=65;w+=(w<3||w>60)?1:31)for(int same=0;same<3;same++){
   restore();seg(16,C0()->rcv_next+(same==2),C0()->snd_una+(same==1),w,0,0);input(T+5);}
  for(size_t cap=0;cap<=66;cap+=(cap<3||cap>60)?1:15)for(int ali=0;ali<3;ali++){
   restore();call_t k=mk(RECV,T+6);k.cap=cap;if(ali==1)k.len=OUT+8;if(ali==2)k.out=NUL;run(&k);}
  for(size_t n=0;n<=66;n+=(n<3||n>61)?1:20)for(int src=0;src<3;src++)for(size_t cap=0;cap<2;cap++){
   restore();call_t k=mk(SEND,T+7);k.n=n;k.data=src==0?DATA:src==1?NUL:OUT;k.cap=cap?54+n-1:1514;run(&k);}
  for(int c=0;c<3;c++){restore();call_t k=mk(ACKN,T+8);k.cap=c==0?1514:c==1?59:60;run(&k);}
  for(int bi=0;bi<2;bi++)for(uint64_t dt=0;dt<6;dt++){static const uint64_t edges[6]={199,200,201,399,400,401};
   restore();uint64_t t=(bi?C0()->tx_since:C0()->since)+edges[dt];
   call_t k=mk(EXPIRE,t);run(&k);restore();peer_data(1,t);restore();k=mk(SEND,t);k.n=1;run(&k);
   restore();k=mk(RECV,t);k.cap=8;run(&k);restore();k=mk(ACKN,t);run(&k);}
  restore();run1(ABORT);run1(ABORT);peer_data(4,T+9);
 }
}
static void stage3(void){
 stage=3;what="payload";
 static const uint32_t isns[4]={0xffffffc0u,0x7fffffe0u,0x11223344u,0};
 static const uint32_t peers[4]={0xffffffe0u,0x7ffffff0u,0x01020304u,0x80000000u};
 for(int i=0;i<4;i++)payload_sweeps(isns[i],peers[(i+1)%4],i==1?UINT64_MAX-150:(uint64_t)(1000+i*100000),0,0);
}
/* ---------------- stage 4: retransmission: RTO, retry budget, windows, partial ACKs, tick wrap ---------------- */
static void stage4(void){
 stage=4;what="retransmit";
 for(int i=0;i<6;i++)for(size_t n=1;n<=64;n=n<4?n+1:n*2){
  uint32_t isn=i&1?0xffffffe0u:0x7ffffff0u;uint64_t t0=i<2?(uint64_t)1000:i<4?UINT64_MAX-120:(uint64_t)0;
  fresh(isn,1,1,i>=4);T=t0;if(!establish(0x55aa0000u+(uint32_t)i,i==5?2:64))exit(8);
  send_n(n,T+2);save();
  for(uint64_t dt=0;dt<=260;dt+=(dt<45||dt>155)?1:7){restore();call_t k=mk(RETX,T+2+dt);run(&k);}
  for(int cap=0;cap<3;cap++){restore();call_t k=mk(RETX,T+52);k.cap=cap==0?59:cap==1?54+n-1:54+n;run(&k);}
  restore();for(int r=0;r<6;r++){call_t k=mk(RETX,T+2+50*(uint64_t)(r+1));run(&k);}
  restore();for(int r=0;r<5;r++){call_t k=mk(RETX,T+2+60*(uint64_t)(r+1));run(&k);seg(16,C0()->rcv_next,C0()->snd_una+(uint32_t)(r&1),r==2?0:(unsigned)(n/2),0,0);input(T+3+60*(uint64_t)(r+1));}
  for(size_t ackd=0;ackd<=n;ackd+=ackd<2?1:(n/2?n/2:1)){restore();seg(16,C0()->rcv_next,C0()->snd_una+(uint32_t)ackd,64,0,0);input(T+20);
   for(int r=0;r<3;r++){call_t k=mk(RETX,T+75+50*(uint64_t)r);run(&k);}}
  restore();seg(16,C0()->rcv_next,C0()->snd_una,0,0,0);input(T+10);{call_t k=mk(RETX,T+60);run(&k);}
  restore();{call_t k=mk(EXPIRE,T+2+200);run(&k);}
 }
 fresh(1,1,0,0);T=10;establish(2,64);send_n(4,T+2);{call_t k=mk(RETX,T+60);run(&k);}   /* not enabled */
 fresh(1,1,1,0);T=10;establish(2,64);{call_t k=mk(RETX,T+60);run(&k);}            /* nothing in flight */
}
/* ---------------- stage 5: closure: FIN-WAIT-1/2, CLOSING, TIME-WAIT, CLOSE-WAIT, LAST-ACK ---------------- */
static const unsigned CFLAGS[12]={16,24,17,25,4,20,2,18,0,1,8,48};
static void closure_sweep(void){
 save();uint32_t rn=C0()->rcv_next,su=C0()->snd_una,sn=C0()->snd_next;
 static const int32_t d[7]={0,1,-1,64,(int32_t)0x80000000,0x7fffffff,2};
 for(int f=0;f<12;f++)for(int a=0;a<7;a++)for(int b=0;b<5;b++)for(size_t n=0;n<=65;n+=(n<2||n>63)?1:31){
  if(f>=4&&n>1)continue;
  restore();seg(CFLAGS[f],rn+(uint32_t)d[a],(b<3?su:sn)+(uint32_t)d[b],(unsigned)(a*13+b),D+1,n);input(T+5);}
 restore();seg(17,rn-1,sn,64,0,0);input(T+6);                        /* duplicate FIN */
 for(int op=0;op<5;op++){restore();call_t k=mk(op==0?FINISH:op==1?ACKN:op==2?RETX:op==3?RECV:SEND,T+7);k.n=3;k.cap=op==3?16:1514;run(&k);}
 /* timer edges exactly at the context's own deadlines (since and tx_since bases) */
 static const uint64_t edges[11]={199,200,201,399,400,23999,24000,24001,47999,48000,48001};
 const uint64_t bases[3]={C0()->since,C0()->tx_since,T};
 for(int bi=0;bi<3;bi++)for(int e=0;e<11;e++){uint64_t t=bases[bi]+edges[e];
  restore();call_t k=mk(EXPIRE,t);run(&k);restore();k=mk(RETX,t);run(&k);restore();peer_data(0,t);restore();k=mk(RECV,t);k.cap=8;run(&k);}
 {restore();call_t k=mk(EXPIRE,T+UINT64_MAX/2);run(&k);}
 for(int r=0;r<3;r++){restore();seg(r==0?4:20,rn+(uint32_t)r,r==2?sn:0,0,0,0);input(T+5);}
}
static void close_states(uint32_t isn,uint32_t peer,uint64_t t0,int retx){
 const uint64_t base=t0;
 /* active close: FIN-WAIT-1, FIN-WAIT-2, CLOSING, TIME-WAIT */
 fresh(isn,1,retx,1);T=base;if(!establish(peer,64))exit(9);{call_t k=mk(FINISH,T+2);run(&k);}closure_sweep();
 restore();seg(16,C0()->rcv_next,C0()->snd_next,64,0,0);input(T+3);closure_sweep();
 fresh(isn,1,retx,1);T=base;establish(peer,64);{call_t k=mk(FINISH,T+2);run(&k);}seg(17,C0()->rcv_next,C0()->snd_una,64,0,0);input(T+3);closure_sweep();
 restore();seg(16,C0()->rcv_next,C0()->snd_next,64,0,0);input(T+4);closure_sweep();
 /* TIME-WAIT and CLOSING reached with unread peer data (TIME-WAIT cleanup needs an empty RX) */
 for(int st=0;st<2;st++){fresh(isn,1,retx,1);T=base;establish(peer,64);{call_t k=mk(FINISH,T+2);run(&k);}
  if(st==0){seg(16,C0()->rcv_next,C0()->snd_next,64,0,0);input(T+3);}            /* FIN-WAIT-2 */
  peer_data(5,T+4);seg(17,C0()->rcv_next,st?C0()->snd_una:C0()->snd_next,64,0,0);input(T+5);closure_sweep();
  if(st==1){restore();seg(16,C0()->rcv_next,C0()->snd_next,64,0,0);input(T+6);closure_sweep();}}
 /* passive close: CLOSE-WAIT, LAST-ACK, with retained data */
 for(int keep=0;keep<2;keep++){fresh(isn,1,retx,1);T=base;establish(peer,64);if(keep)peer_data(5,T+2);
  seg(17,C0()->rcv_next,C0()->snd_next,64,0,0);input(T+3);closure_sweep();
  restore();if(keep){call_t k=mk(RECV,T+4);k.cap=64;run(&k);}{call_t k=mk(FINISH,T+5);run(&k);}closure_sweep();}
 /* FIN at the window edge, FIN with data, data while closing */
 for(size_t fill=60;fill<=64;fill++){fresh(isn,1,retx,1);T=base;establish(peer,64);peer_data(fill,T+2);save();
  for(size_t n=0;n<=5;n++){restore();seg(n?25:17,C0()->rcv_next,C0()->snd_next,64,D,n);input(T+3);}}
 /* FIN retransmission budget */
 if(retx){fresh(isn,1,1,1);T=base;establish(peer,64);{call_t k=mk(FINISH,T+2);run(&k);}
  for(int r=0;r<6;r++){call_t k=mk(RETX,T+2+50*(uint64_t)(r+1));run(&k);k.now--;run(&k);}}
 /* finish refusals: data in flight or unread, zero window, repeated finish */
 fresh(isn,1,retx,1);T=base;establish(peer,0);{call_t k=mk(FINISH,T+2);run(&k);}
 fresh(isn,1,retx,1);T=base;establish(peer,64);send_n(2,T+2);{call_t k=mk(FINISH,T+3);run(&k);}
 fresh(isn,1,retx,1);T=base;establish(peer,64);peer_data(2,T+2);{call_t k=mk(FINISH,T+3);run(&k);run(&k);}
 fresh(isn,1,retx,1);T=base;establish(peer,64);{call_t k=mk(FINISH,T+2);run(&k);run(&k);k.cap=59;run(&k);}
}
static void stage5(void){
 stage=5;what="closure";
 for(int r=0;r<2;r++){close_states(0xfffffff8u,0x7ffffffcu,1000,r);close_states(0x7fffffffu,0xffffffffu,UINT64_MAX-30000,r);}
 close_states(0,0x80000000u,0,1);
}
/* ---------------- stage 6: seeded random sequences with fault injection ---------------- */
static void stage6(void){
 stage=6;what="sequence";
 for(unsigned s=0;s<TCP_SEQUENCES;s++){
  int pay=(int)(next()%4!=0),retx=pay&&(next()&1),clo=pay&&(next()&1);
  uint32_t isn=next();if(s%5==0)isn=0xffffffffu-(next()%200);if(s%7==0)isn=0x7fffffffu-(next()%200);
  uint32_t peer=next();if(s%3==0)peer=0xffffffffu-(next()%100);
  fresh(isn,pay,retx,clo);T=(s%4==0)?UINT64_MAX-(next()%3000):(uint64_t)next();
  for(unsigned step=0;step<40;step++){
   T+=next()%(step&1?3:90);unsigned op=next()%16;struct net_tcp *c=C0();
   uint32_t seq=c->rcv_next,ack=c->snd_una,r=next();
   if(r%8==0){seq+=next()%130-65;}if(r%8==1){ack+=next()%130-65;}if(r%16==2){seq^=0x80000000u;}if(r%16==3){ack=c->snd_next+(next()%3);}
   size_t n=(next()%4==0)?next()%70:0;if(c->state==NET_TCP_LISTEN&&op<8)op=0;
   call_t k=mk(INPUT,T);
   if(next()%10==0){k.cap=next()%3?59:0;}if(next()%25==0){k.out=NUL;}if(next()%25==0){k.len=OUT+8*(long)(next()%8);}if(next()%30==0){k.out=FRAME;}
   switch(op){
   case 0:seg(2,peer,next()%5?0:1,1024,0,0);break;
   case 1:case 2:seg(16,seq,ack,next()%80,0,0);break;
   case 3:case 4:seg(24,seq,ack,next()%80,D+(next()%40),n?n:1+next()%64);break;
   case 5:seg(17+8*(n!=0),seq,ack,next()%80,D,n);break;
   case 6:seg(next()%2?4:20,seq+(next()%3==0),ack,0,0,0);break;
   case 7:seg(next()&255,seq,ack,next()%80,D,n);break;
   default:break;}
   if(op<8){if(next()%6==0){F[next()%FL]^=(uint8_t)(1u<<(next()%8));if(next()&1)fix_ip(F);if(next()&1)fix_tcp(F);}
    k.n=FL;if(next()%12==0)k.n=next()%(FL+3);input_k(k);continue;}
   if(op==8||op==9){k.op=SEND;k.n=1+next()%66;if(next()%20==0)k.data=next()%2?NUL:OUT;run(&k);}
   else if(op==10){k.op=RECV;k.cap=next()%70;run(&k);}
   else if(op==11){k.op=ACKN;run(&k);}
   else if(op==12){k.op=RETX;run(&k);}
   else if(op==13){k.op=FINISH;run(&k);}
   else if(op==14){k.op=EXPIRE;run(&k);}
   else {static const int rare[6]={ABORT,EPAY,ERETX,ECLOSE,CLOSE,ABORT};int qop=rare[next()%6];run1(qop);if(qop==CLOSE){call_t z=mk(INIT,0);z.isn=isn;run(&z);}}
  }
  sequences++;
 }
}
/* ---------------- Rust-only refusals (outside the documented contract) ---------------- */
static void refusals(void){
 static _Alignas(16) unsigned char buf[2048];struct net_tcp *c=(void*)buf;memset(buf,0x5a,sizeof buf);
 if(net_tcp_init(c,G,GI,4001,1)||net_tcp_enable_payload(c))exit(10);
 seg(2,5,0,64,0,0);size_t len=0;uint8_t out[128];memcpy(buf+600,F,FL);
 if(net_tcp_input(c,F,FL,1,out,sizeof out,&len)!=NET_TCP_SYNACK)exit(11);
 seg(16,6,c->snd_next,64,0,0);if(net_tcp_input(c,F,FL,2,out,sizeof out,&len)!=NET_TCP_CONNECTED)exit(12);
 unsigned char before[2048];memcpy(buf+8,F,FL);memcpy(before,buf,sizeof buf);   /* a frame placed inside the context */
 if(net_tcp_input(c,buf+8,FL,3,out,sizeof out,&len)!=NET_ARP_EINVAL||memcmp(buf,before,sizeof buf)){fprintf(stderr,"FAIL frame overlapping context\n");exit(1);}
 if(net_tcp_send(c,buf+400,8,3,out,sizeof out,&len)!=NET_ARP_EINVAL||memcmp(buf,before,sizeof buf)){fprintf(stderr,"FAIL send data overlapping context\n");exit(1);}
 if(net_tcp_receive(c,buf+380,16,&len,3)!=NET_ARP_EINVAL||memcmp(buf,before,sizeof buf)){fprintf(stderr,"FAIL receive output overlapping context\n");exit(1);}
 if(net_tcp_init((void*)(buf+1),G,GI,4001,1)!=NET_ARP_EINVAL||memcmp(buf,before,sizeof buf)){fprintf(stderr,"FAIL misaligned context\n");exit(1);}
 if(net_tcp_input(NULL,F,FL,3,out,sizeof out,&len)!=NET_ARP_EINVAL||net_tcp_expire(NULL,1)!=NET_ARP_EINVAL){fprintf(stderr,"FAIL null context\n");exit(1);}
 net_tcp_close(NULL);net_tcp_abort(NULL);
}
/* ---------------- paired benchmark: B (C TCP, production Rust IPv4) vs R (Rust TCP) ---------------- */
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
static volatile uint64_t sink;
static double timing(unsigned w,int s){
 static _Alignas(64) unsigned char ref[ARENA];static uint8_t syn[64],ackf[64],dat[128];static size_t sl,al,dl;
 unsigned iters=20000;uint64_t total=0;
 for(unsigned i=0;i<iters;i++){
  memcpy(arena[s],tmpl,ARENA);struct net_tcp *c=ctx(s);size_t ol;int r=0;uint8_t *o=arena[s]+OUT;
  if(!sl){memcpy(ref,tmpl,ARENA);struct net_tcp *q=(void*)ref;
   seg(2,0x1000,0,1024,0,0);memcpy(syn,F,FL);sl=FL;net_tcp_input_rA(q,syn,sl,10,ref+OUT,1514,&ol);
   seg(16,0x1001,q->snd_next,1024,0,0);memcpy(ackf,F,FL);al=FL;
   seg(24,0x1001,q->snd_next,1024,D,32);memcpy(dat,F,FL);dl=FL;}
  uint64_t a;
  switch(w){
  case 0:a=ns();r=I[s].input(c,syn,sl,10,o,1514,&ol);break;                                   /* SYN -> SYN-ACK */
  case 1:I[s].input(c,syn,sl,10,o,1514,&ol);a=ns();r=I[s].input(c,ackf,al,11,o,1514,&ol);break;   /* ACK -> ESTABLISHED */
  case 2:I[s].input(c,syn,sl,10,o,1514,&ol);I[s].input(c,ackf,al,11,o,1514,&ol);a=ns();r=I[s].input(c,dat,dl,12,o,1514,&ol);break; /* data -> ACK */
  case 3:I[s].input(c,syn,sl,10,o,1514,&ol);I[s].input(c,ackf,al,11,o,1514,&ol);a=ns();r=I[s].send(c,D,48,12,o,1514,&ol);break;   /* send */
  case 4:I[s].input(c,syn,sl,10,o,1514,&ol);I[s].input(c,ackf,al,11,o,1514,&ol);I[s].input(c,dat,dl,12,o,1514,&ol);a=ns();r=I[s].recv(c,arena[s]+DATA,16,&ol,13);break; /* partial receive */
  default:I[s].input(c,syn,sl,10,o,1514,&ol);I[s].input(c,ackf,al,11,o,1514,&ol);I[s].send(c,D,48,12,o,1514,&ol);a=ns();r=I[s].retx(c,70,o,1514,&ol);break; /* retransmit */
  }
  total+=ns()-a;sink+=(uint64_t)r;
 }
 return (double)total/iters;
}
int main(int argc,char **argv){
 for(int i=0;i<128;i++)D[i]=(uint8_t)(i*37+11);
 for(int i=0;i<1500;i++)DB[i]=(uint8_t)(i*13+5);
 if(argc>1&&!strcmp(argv[1],"--bench")){
  bench=1;fresh(0x11223344u,1,1,1);save();puts("workload,sample,C_ns,Rust_ns");
  for(unsigned w=0;w<6;w++){(void)timing(w,1);(void)timing(w,2);
   for(unsigned s=0;s<9;s++){double a,b;if(s&1){b=timing(w,2);a=timing(w,1);}else{a=timing(w,1);b=timing(w,2);}printf("%u,%u,%.4f,%.4f\n",w,s,a,b);}}
  return 0;
 }
 stage0();stage1();stage2();stage3();stage4();stage5();stage6();refusals();
 unsigned long long total=0;for(int i=0;i<7;i++)total+=cases[i];
 /* every TCP state and every return code the TCP API can produce must occur
  * (TCP codes, generic NET_ARP codes, ARP REPLY through delegation; LEARNED
  * needs a pending ARP query, which the TCP API cannot create) */
 static const int need_rc[22]={-12,-11,-10,-9,-8,-4,-3,-2,-1,0,1,9,10,11,12,13,14,15,16,17,18,19};
 for(int i=0;i<10;i++)if(!seen_state[i]){fprintf(stderr,"FAIL state %d never reached\n",i);return 1;}
 for(int i=0;i<22;i++)if(!seen_rc[need_rc[i]+12]){fprintf(stderr,"FAIL return code %d never produced\n",need_rc[i]);return 1;}
 printf("STAGES %llu %llu %llu %llu %llu %llu %llu\n",cases[0],cases[1],cases[2],cases[3],cases[4],cases[5],cases[6]);
 if((TCP_CASES && total!=TCP_CASES) || sequences!=TCP_SEQUENCES){fprintf(stderr,"FAIL comparison coverage count %llu\n",total);return 1;}
 printf("PASS %llu TCP comparisons (3-way: frozen C/frozen C IPv4, frozen C/Rust IPv4, Rust) and %llu random sequences; whole-arena state, output, immutability and aliasing\n",total,sequences);
 return 0;
}
