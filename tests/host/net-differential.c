#define _GNU_SOURCE
#include "net_udp.h"
#include "rust_kernel.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* R3-N1: frozen accepted C ARP/IPv4/UDP (879c511) versus the Rust-owned core.
 * Every call runs on two identical 8 KiB arenas (context, frame, output, length
 * and view slots); the return value and the ENTIRE arena must match, which
 * checks state, output bytes/lengths, input immutability and aliasing at once. */
/* Exact comparison coverage of stages 1-4 (see main). */
#define NET_CASES 635285ULL
_Static_assert(sizeof(struct net_arp)==160 && sizeof(struct net_ipv4)==288 && sizeof(struct net_udp)==2400 && sizeof(struct net_udp_datagram)==524,"ABI");
#define R(x) x##_reference
extern int R(net_arp_init)(struct net_arp*,const uint8_t*,const uint8_t*);
extern int R(net_neighbor_get)(struct net_arp*,const uint8_t*,uint64_t,uint8_t*);
extern int R(net_arp_query)(struct net_arp*,const uint8_t*,uint64_t,void*,size_t,size_t*);
extern int R(net_ethernet_type)(const struct net_arp*,const void*,size_t,uint16_t*);
extern int R(net_ethernet_input)(struct net_arp*,const void*,size_t,uint64_t,void*,size_t,size_t*);
extern uint16_t R(net_checksum)(const void*,size_t);
extern uint16_t R(net_ipv4_transport_checksum)(const uint8_t*,const uint8_t*,uint8_t,const void*,size_t);
extern int R(net_ipv4_payload)(const struct net_ipv4*,const void*,size_t,uint8_t,struct net_ipv4_payload*);
extern int R(net_ipv4_begin)(const struct net_ipv4*,const uint8_t*,const uint8_t*,uint8_t,size_t,void*,size_t,size_t*);
extern int R(net_ipv4_init)(struct net_ipv4*,const uint8_t*,const uint8_t*);
extern int R(net_ipv4_echo_request)(struct net_ipv4*,const uint8_t*,uint16_t,uint16_t,const void*,size_t,uint64_t,void*,size_t,size_t*);
extern int R(net_ipv4_input)(struct net_ipv4*,const void*,size_t,uint64_t,void*,size_t,size_t*);
extern int R(net_udp_init)(struct net_udp*,const uint8_t*,const uint8_t*,uint16_t);
extern int R(net_udp_send)(struct net_udp*,const uint8_t*,uint16_t,const void*,size_t,uint64_t,void*,size_t,size_t*);
extern int R(net_udp_input)(struct net_udp*,const void*,size_t,uint64_t,void*,size_t,size_t*);
extern int R(net_udp_receive)(struct net_udp*,struct net_udp_datagram*);
extern void R(net_udp_close)(struct net_udp*);

static unsigned long long cases[5],scenarios;static int bench;
static void die(const char *s,unsigned long long n){fprintf(stderr,"FAIL %s case=%llu\n",s,n);exit(1);}
static uint64_t rng=UINT64_C(0x52334e3172757374);
static uint32_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)rng;}
static uint64_t next64(void){return ((uint64_t)next()<<32)|next();}
/* ---------------- arenas ---------------- */
#define ARENA 8192
enum {CTX=0,FRAME=2432,OUT=4096,LEN=6144,VIEW=6160,DGRAM=6208,IN2=6800};
static _Alignas(64) unsigned char arena[2][ARENA],tmpl[ARENA];
static void *at(int s,size_t off){return arena[s]+off;}
static void load(void){memcpy(arena[0],tmpl,ARENA);memcpy(arena[1],tmpl,ARENA);}
static void same(int r0,int r1,const char *what,int stage){
 if(r0!=r1)die(what,cases[stage]);
 if(memcmp(arena[0],arena[1],ARENA))die(what,cases[stage]);
 if(!bench)cases[stage]++;
}
/* Rebase a pointer known to lie inside arena 0 to arena s (aliasing preserved). */
static void *rb(int s,void *p){
 if(!p)return NULL;
 uintptr_t u=(uintptr_t)p,b=(uintptr_t)arena[0];
 return (u>=b && u<b+ARENA)?arena[s]+(u-b):p;
}
/* ---------------- frame builders (independent of both implementations) ---------------- */
static const uint8_t G[6]={0x52,0x54,0,0x12,0x34,0x56},P[6]={0x52,0x54,0,0xaa,0xbb,0xcc},GI[4]={10,0,2,15},PI[4]={10,0,2,2};
static void put(uint8_t *p,unsigned x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static uint16_t csum(const uint8_t *p,size_t n){uint32_t s=0;for(size_t i=0;i<n;i++){s+=(uint32_t)p[i]<<((i&1)?0:8);s=(s&65535)+(s>>16);}return (uint16_t)~s;}
static size_t arp_frame(uint8_t *f,unsigned op,const uint8_t *dst,const uint8_t *sha,const uint8_t *spa,const uint8_t *tha,const uint8_t *tpa){
 memset(f,0,60);memcpy(f,dst,6);memcpy(f+6,sha,6);put(f+12,0x0806);put(f+14,1);put(f+16,0x0800);f[18]=6;f[19]=4;put(f+20,op);
 memcpy(f+22,sha,6);memcpy(f+28,spa,4);memcpy(f+32,tha,6);memcpy(f+38,tpa,4);return 60;
}
static size_t ip_frame(uint8_t *f,uint8_t proto,const uint8_t *src,const uint8_t *dst,const uint8_t *payload,size_t n){
 size_t wire=34+n;if(wire<60)wire=60;memset(f,0,wire);memcpy(f,G,6);memcpy(f+6,P,6);put(f+12,0x0800);
 uint8_t *h=f+14;h[0]=0x45;put(h+2,(unsigned)(20+n));put(h+4,0x1122);put(h+6,0x4000);h[8]=64;h[9]=proto;memcpy(h+12,src,4);memcpy(h+16,dst,4);put(h+10,csum(h,20));
 if(n)memcpy(f+34,payload,n);
 return wire;
}
static void fix_ip(uint8_t *f){put(f+24,0);put(f+24,csum(f+14,(size_t)(f[14]&15)*4));}
static size_t icmp_frame(uint8_t *f,unsigned type,unsigned id,unsigned seq,const uint8_t *data,size_t n){
 uint8_t b[1480];b[0]=(uint8_t)type;b[1]=0;put(b+2,0);put(b+4,id);put(b+6,seq);if(n)memcpy(b+8,data,n);put(b+2,csum(b,8+n));
 return ip_frame(f,1,PI,GI,b,8+n);
}
static uint16_t udp_sum(const uint8_t *src,const uint8_t *dst,const uint8_t *u,size_t n){
 uint8_t b[12+1480];memcpy(b,src,4);memcpy(b+4,dst,4);b[8]=0;b[9]=17;put(b+10,(unsigned)n);memcpy(b+12,u,n);return csum(b,12+n);
}
static size_t udp_frame(uint8_t *f,unsigned sport,unsigned dport,const uint8_t *data,size_t n,int checked){
 uint8_t u[8+1472];put(u,sport);put(u+2,dport);put(u+4,(unsigned)(8+n));put(u+6,0);if(n)memcpy(u+8,data,n);
 if(checked){uint16_t s=udp_sum(PI,GI,u,8+n);put(u+6,s?s:65535);}
 return ip_frame(f,17,PI,GI,u,8+n);
}
/* ---------------- call wrappers over both arenas ---------------- */
#define CTXA(s) ((struct net_arp*)at(s,CTX))
#define CTXI(s) ((struct net_ipv4*)at(s,CTX))
#define CTXU(s) ((struct net_udp*)at(s,CTX))
static int call_input(int s,int which,const void *frame,size_t len,uint64_t now,void *out,size_t cap,size_t *ol){
 int ref=s==0;
 switch(which){
 case 0:return ref?R(net_ethernet_input)(CTXA(s),rb(s,(void*)frame),len,now,rb(s,out),cap,rb(s,ol)):net_ethernet_input(CTXA(s),rb(s,(void*)frame),len,now,rb(s,out),cap,rb(s,ol));
 case 1:return ref?R(net_ipv4_input)(CTXI(s),rb(s,(void*)frame),len,now,rb(s,out),cap,rb(s,ol)):net_ipv4_input(CTXI(s),rb(s,(void*)frame),len,now,rb(s,out),cap,rb(s,ol));
 default:return ref?R(net_udp_input)(CTXU(s),rb(s,(void*)frame),len,now,rb(s,out),cap,rb(s,ol)):net_udp_input(CTXU(s),rb(s,(void*)frame),len,now,rb(s,out),cap,rb(s,ol));
 }
}
/* Compare one frame input (ctx kind `which`) with output at OUT (or aliased). */
static void cmp_input(int which,const uint8_t *frame,size_t len,uint64_t now,int alias,int stage){
 load();
 memcpy(arena[0]+FRAME,frame,len);memcpy(arena[1]+FRAME,frame,len);
 void *f=arena[0]+FRAME,*o=alias==1?arena[0]+FRAME:arena[0]+OUT;size_t cap=alias==2?59:1514;size_t *l=(size_t*)(arena[0]+LEN);
 if(alias==3)l=(size_t*)(arena[0]+OUT+8);   /* length overlapping the packet output: NOSPACE */
 int r0=call_input(0,which,f,len,now,o,cap,l),r1=call_input(1,which,f,len,now,o,cap,l);
 same(r0,r1,"frame input",stage);
}
/* ---------------- context templates ---------------- */
static void ctx_template(int kind,uint64_t now,int neighbors,int pending){
 memset(tmpl,0x5a,ARENA);
 int e;
 if(kind==0)e=R(net_arp_init)((struct net_arp*)tmpl,G,GI);
 else if(kind==1)e=R(net_ipv4_init)((struct net_ipv4*)tmpl,G,GI);
 else e=R(net_udp_init)((struct net_udp*)tmpl,G,GI,4000);
 if(e)die("template init",0);
 struct net_arp *a=(struct net_arp*)tmpl;
 for(int i=0;i<neighbors && i<NET_ARP_NEIGHBORS;i++){a->neighbors[i].valid=1;memcpy(a->neighbors[i].ip,PI,4);a->neighbors[i].ip[3]=(uint8_t)(2+i);memcpy(a->neighbors[i].mac,P,6);a->neighbors[i].mac[5]=(uint8_t)(0xcc+i);a->neighbors[i].seen=now-(uint64_t)i*1000;}
 if(pending){memcpy(a->pending_ip,PI,4);a->pending_since=now;a->pending=1;}
}
/* ---------------- stage 1: checksums ---------------- */
static void stage1(void){
 uint8_t b[1500];
 for(unsigned n=0;n<=2;n++){unsigned lim=n==0?1:n==1?256:65536;
  for(unsigned v=0;v<lim;v++){b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);
   load();memcpy(arena[0]+FRAME,b,n);memcpy(arena[1]+FRAME,b,n);
   int r0=R(net_checksum)(arena[0]+FRAME,n),r1=net_checksum(arena[1]+FRAME,n);same(r0,r1,"checksum",0);}}
 for(unsigned i=0;i<20000;i++){size_t n=next()%1501;for(size_t j=0;j<n;j++)b[j]=(next()&3)?0xff:(uint8_t)next();
  load();memcpy(arena[0]+FRAME,b,n);memcpy(arena[1]+FRAME,b,n);
  same(R(net_checksum)(arena[0]+FRAME,n),net_checksum(arena[1]+FRAME,n),"checksum random",0);}
 for(unsigned i=0;i<20000;i++){size_t n=i<1481?i:next()%1481;uint8_t s[4],d[4];for(int j=0;j<4;j++){s[j]=(i&1)?0xff:(uint8_t)next();d[j]=(i&2)?0:(uint8_t)next();}
  for(size_t j=0;j<n;j++)b[j]=(i&4)?0xff:(uint8_t)next();
  uint8_t proto=(uint8_t)((i&8)?next():17);
  load();memcpy(arena[0]+IN2,s,4);memcpy(arena[1]+IN2,s,4);memcpy(arena[0]+IN2+4,d,4);memcpy(arena[1]+IN2+4,d,4);memcpy(arena[0]+FRAME,b,n);memcpy(arena[1]+FRAME,b,n);
  same(R(net_ipv4_transport_checksum)(arena[0]+IN2,arena[0]+IN2+4,proto,arena[0]+FRAME,n),net_ipv4_transport_checksum(arena[1]+IN2,arena[1]+IN2+4,proto,arena[1]+FRAME,n),"transport checksum",0);}
}
/* ipv4_payload returns pointers into the caller's frame: compare them as arena offsets. */
static void payload_cmp(size_t len,uint8_t proto,const char *what){
 int r0=R(net_ipv4_payload)((void*)arena[0],arena[0]+FRAME,len,proto,(void*)(arena[0]+VIEW));
 int r1=net_ipv4_payload((void*)arena[1],arena[1]+FRAME,len,proto,(void*)(arena[1]+VIEW));
 for(int s=0;s<2;s++)if((s?r1:r0)==1){struct net_ipv4_payload v;memcpy(&v,arena[s]+VIEW,sizeof v);
  v.source=(const uint8_t*)((uintptr_t)v.source-(uintptr_t)arena[s]);v.bytes=(const uint8_t*)((uintptr_t)v.bytes-(uintptr_t)arena[s]);memcpy(arena[s]+VIEW,&v,sizeof v);}
 same(r0,r1,what,1);
}
/* ---------------- stage 2: Ethernet-II / IPv4 validation and envelope ---------------- */
static void stage2(void){
 uint8_t f[1600];
 static const uint8_t B[6]={255,255,255,255,255,255},Z[6]={0},M[6]={1,0,0,0,0,1},O[6]={0x52,0x54,0,9,9,9};
 const uint8_t *dsts[4]={G,B,O,M},*srcs[4]={P,M,Z,G};
 ctx_template(0,1000,0,0);
 for(unsigned d=0;d<4;d++)for(unsigned s=0;s<4;s++)for(unsigned t=0;t<65536;t+=(d==0&&s==0)?1:257){
  memset(f,0,60);memcpy(f,dsts[d],6);memcpy(f+6,srcs[s],6);put(f+12,t);
  load();memcpy(arena[0]+FRAME,f,60);memcpy(arena[1]+FRAME,f,60);
  same(R(net_ethernet_type)((void*)arena[0],arena[0]+FRAME,60,(uint16_t*)(arena[0]+LEN)),net_ethernet_type((void*)arena[1],arena[1]+FRAME,60,(uint16_t*)(arena[1]+LEN)),"ethernet type",1);}
 for(size_t len=0;len<=1516;len+=(len<20||len>1500)?1:97){memset(f,0,1600);memcpy(f,G,6);memcpy(f+6,P,6);put(f+12,0x0800);
  load();memcpy(arena[0]+FRAME,f,1600);memcpy(arena[1]+FRAME,f,1600);
  same(R(net_ethernet_type)((void*)arena[0],arena[0]+FRAME,len,(uint16_t*)(arena[0]+LEN)),net_ethernet_type((void*)arena[1],arena[1]+FRAME,len,(uint16_t*)(arena[1]+LEN)),"ethernet length",1);}
 /* IPv4 payload: header fields exhaustively, truncation, every-offset corruption */
 ctx_template(1,1000,1,0);
 uint8_t pl[64];for(int i=0;i<64;i++)pl[i]=(uint8_t)i;
 size_t base=ip_frame(f,17,PI,GI,pl,40);
 for(unsigned v=0;v<256;v++){uint8_t g[1600];memcpy(g,f,base);g[14]=(uint8_t)v;fix_ip(g);
  for(unsigned proto=0;proto<256;proto+=v==0x45?1:255){g[23]=(uint8_t)proto;fix_ip(g);
   load();memcpy(arena[0]+FRAME,g,base);memcpy(arena[1]+FRAME,g,base);
   payload_cmp(base,17,"ipv4 header byte");}}
 for(unsigned total=0;total<1600;total+=total<100?1:53)for(size_t len=34;len<=1514;len+=len<120?7:211){uint8_t g[1600];memset(g,0,1600);memcpy(g,f,base);put(g+16,total);fix_ip(g);
  load();memcpy(arena[0]+FRAME,g,1600);memcpy(arena[1]+FRAME,g,1600);
  payload_cmp(len,17,"ipv4 total vs frame");}
 for(unsigned frag=0;frag<65536;frag+=1+(frag>0x4010?997:0)){uint8_t g[1600];memcpy(g,f,base);put(g+20,frag);fix_ip(g);
  load();memcpy(arena[0]+FRAME,g,base);memcpy(arena[1]+FRAME,g,base);
  payload_cmp(base,17,"ipv4 fragment word");}
 for(size_t off=0;off<base;off++)for(unsigned k=0;k<6;k++){uint8_t g[1600];memcpy(g,f,base);g[off]^=(uint8_t)(1u<<k);
  load();memcpy(arena[0]+FRAME,g,base);memcpy(arena[1]+FRAME,g,base);
  payload_cmp(base,17,"ipv4 bit flip");}
 for(size_t len=0;len<=base;len++){load();memcpy(arena[0]+FRAME,f,base);memcpy(arena[1]+FRAME,f,base);
  payload_cmp(len,17,"ipv4 truncation");}
 /* Envelope: every payload length, capacity edges and output/length placement */
 for(size_t n=0;n<=1481;n++)for(unsigned k=0;k<4;k++){size_t wire=34+n<60?60:34+n;size_t cap=k==1?wire-1:1600;
  load();uint8_t proto=(uint8_t)n;
  void *o=k==2?NULL:(void*)(arena[0]+OUT);size_t *l=k==3?(size_t*)(arena[0]+OUT+4):(size_t*)(arena[0]+LEN);
  int r0=R(net_ipv4_begin)((void*)arena[0],P,PI,proto,n,rb(0,o),cap,rb(0,l)),r1=net_ipv4_begin((void*)arena[1],P,PI,proto,n,rb(1,o),cap,rb(1,l));
  same(r0,r1,"ipv4 begin",1);}
}
/* ---------------- stage 3: ARP, neighbor table, ICMP echo ---------------- */
static void stage3(void){
 uint8_t f[1600];
 /* init admission: every first MAC byte x every first IP byte */
 for(unsigned m=0;m<256;m++)for(unsigned i=0;i<256;i++){uint8_t mac[6]={(uint8_t)m,0x54,0,1,2,3},ip[4]={(uint8_t)i,1,2,3};if((m+i)%7==0)memset(mac,0,6);
  memset(tmpl,0x5a,ARENA);load();memcpy(arena[0]+IN2,mac,6);memcpy(arena[1]+IN2,mac,6);memcpy(arena[0]+IN2+8,ip,4);memcpy(arena[1]+IN2+8,ip,4);
  same(R(net_arp_init)((void*)arena[0],arena[0]+IN2,arena[0]+IN2+8),net_arp_init((void*)arena[1],arena[1]+IN2,arena[1]+IN2+8),"arp init",2);
  load();same(R(net_ipv4_init)((void*)arena[0],arena[0]+IN2,arena[0]+IN2+8),net_ipv4_init((void*)arena[1],arena[1]+IN2,arena[1]+IN2+8),"ipv4 init",2);}
 /* ARP frames: every field value at each ARP offset, both ops, TTL and wrap edges */
 static const uint64_t times[6]={1000,1199,1200,0,UINT64_MAX,UINT64_C(0x8000000000000000)};
 for(unsigned ti=0;ti<6;ti++)for(int pend=0;pend<2;pend++)for(int nb=0;nb<5;nb+=2){
  uint64_t now=times[ti];ctx_template(0,now,nb,pend);
  uint8_t spa[4];memcpy(spa,PI,4);
  for(unsigned op=1;op<=2;op++){size_t len=arp_frame(f,op,op==1?(const uint8_t[6]){255,255,255,255,255,255}:G,P,spa,op==1?(const uint8_t[6]){0}:G,GI);
   for(size_t off=0;off<42;off++)for(unsigned v=0;v<256;v+=(off>=14&&off<22)?1:51){uint8_t g[60];memcpy(g,f,len);g[off]=(uint8_t)v;
    cmp_input(0,g,len,now+(uint64_t)(v&3)*100,0,2);}
   for(int alias=0;alias<4;alias++)cmp_input(0,f,len,now+150,alias,2);
   for(size_t l2=0;l2<=len;l2++)cmp_input(0,f,l2,now,0,2);}
 }
 /* query: neighbor hit/miss/busy/expiry and wraparound times, output admission */
 for(unsigned ti=0;ti<6;ti++)for(int pend=0;pend<2;pend++)for(int nb=0;nb<5;nb++)for(unsigned d=0;d<8;d++){
  uint64_t now=times[ti];ctx_template(0,now-(d&1)*6000,nb,pend);load();
  uint8_t ip[4];memcpy(ip,PI,4);ip[3]=(uint8_t)(2+d%5);if(d==7)memcpy(ip,GI,4);memcpy(arena[0]+IN2,ip,4);memcpy(arena[1]+IN2,ip,4);
  uint64_t t=now+(uint64_t)d*37;size_t cap=d==6?59:60;
  int r0=R(net_arp_query)((void*)arena[0],arena[0]+IN2,t,arena[0]+OUT,cap,(size_t*)(arena[0]+LEN)),r1=net_arp_query((void*)arena[1],arena[1]+IN2,t,arena[1]+OUT,cap,(size_t*)(arena[1]+LEN));
  same(r0,r1,"arp query",2);
  load();r0=R(net_neighbor_get)((void*)arena[0],arena[0]+IN2,t,arena[0]+IN2);r1=net_neighbor_get((void*)arena[1],arena[1]+IN2,t,arena[1]+IN2);
  same(r0,r1,"neighbor get (output aliases input)",2);}
 /* exact expiry boundaries: neighbor TTL 6000, pending-query TTL 200, ping TTL 200, at normal and wrapped clocks */
 static const uint64_t bases[3]={50000,100,UINT64_MAX-50};
 for(unsigned bi=0;bi<3;bi++)for(uint64_t age=5998;age<=6001;age++){uint64_t now=bases[bi];
  ctx_template(0,now,0,0);struct net_arp *a=(struct net_arp*)tmpl;a->neighbors[1].valid=1;memcpy(a->neighbors[1].ip,PI,4);memcpy(a->neighbors[1].mac,P,6);a->neighbors[1].seen=now-age;
  load();same(R(net_neighbor_get)((void*)arena[0],PI,now,arena[0]+IN2),net_neighbor_get((void*)arena[1],PI,now,arena[1]+IN2),"neighbor TTL boundary",2);
  load();same(R(net_arp_query)((void*)arena[0],PI,now,arena[0]+OUT,60,(size_t*)(arena[0]+LEN)),net_arp_query((void*)arena[1],PI,now,arena[1]+OUT,60,(size_t*)(arena[1]+LEN)),"query at neighbor TTL",2);}
 for(unsigned bi=0;bi<3;bi++)for(uint64_t age=198;age<=201;age++){uint64_t now=bases[bi];
  ctx_template(0,now-age,0,1);
  load();same(R(net_arp_query)((void*)arena[0],(const uint8_t[4]){10,0,2,7},now,arena[0]+OUT,60,(size_t*)(arena[0]+LEN)),net_arp_query((void*)arena[1],(const uint8_t[4]){10,0,2,7},now,arena[1]+OUT,60,(size_t*)(arena[1]+LEN)),"pending query TTL",2);
  size_t len=arp_frame(f,2,G,P,PI,G,GI);cmp_input(0,f,len,now,0,2);
  ctx_template(1,now,2,0);struct net_ipv4 *c=(struct net_ipv4*)tmpl;c->pending=1;c->since=now-age;memcpy(c->peer,PI,4);memcpy(c->peer_mac,P,6);c->id=7;c->sequence=9;c->data_length=0;
  load();same(R(net_ipv4_echo_request)((void*)arena[0],PI,1,2,NULL,0,now,arena[0]+OUT,1514,(size_t*)(arena[0]+LEN)),net_ipv4_echo_request((void*)arena[1],PI,1,2,NULL,0,now,arena[1]+OUT,1514,(size_t*)(arena[1]+LEN)),"ping TTL busy",2);
  size_t er=icmp_frame(f,0,7,9,NULL,0);cmp_input(1,f,er,now,0,2);}
 /* replacement and probes: a full table with equal and mixed ages learns a new peer; RFC 5227 probes reply without learning */
 for(unsigned pat=0;pat<6;pat++)for(unsigned op=1;op<=2;op++){uint64_t now=70000;
  ctx_template(0,now,4,op==2);struct net_arp *a=(struct net_arp*)tmpl;
  for(int i=0;i<4;i++)a->neighbors[i].seen=pat==0?now-100:pat==1?now-(uint64_t)(i%2)*100:pat==2?now-(uint64_t)(3-i)*100:pat==3?now-(uint64_t)(i==2)*500:pat==4?now:now-(uint64_t)i*7;
  uint8_t spa[4]={10,0,2,(uint8_t)(40+pat)};if(op==2)memcpy(a->pending_ip,spa,4);
  size_t len=arp_frame(f,op,op==1?(const uint8_t[6]){255,255,255,255,255,255}:G,P,spa,op==1?(const uint8_t[6]){0}:G,GI);
  cmp_input(0,f,len,now,0,2);}
 for(unsigned k=0;k<8;k++){ctx_template(0,4000,k%5,k&1);uint8_t z[4]={0,0,0,0};
  size_t len=arp_frame(f,1+(k>>2&1),(const uint8_t[6]){255,255,255,255,255,255},P,z,(const uint8_t[6]){0},GI);
  cmp_input(0,f,len,4000+k,0,2);cmp_input(0,f,len,4000+k,1,2);}
 /* ICMP echo: origination for every data length, busy/TTL, then replies/requests field by field */
 for(size_t n=0;n<=65;n++)for(unsigned k=0;k<5;k++){
  ctx_template(1,5000,k==1?0:2,0);struct net_ipv4 *c=(struct net_ipv4*)tmpl;if(k==2){c->pending=1;c->since=4900;}if(k==3){c->pending=1;c->since=4800;}
  load();uint8_t d[80];for(size_t j=0;j<80;j++)d[j]=(uint8_t)(j*7+n);memcpy(arena[0]+IN2,d,80);memcpy(arena[1]+IN2,d,80);
  void *data=k==4?(void*)(arena[0]+OUT):(void*)(arena[0]+IN2);
  int r0=R(net_ipv4_echo_request)((void*)arena[0],PI,0x564f,(uint16_t)n,rb(0,data),n,5000,arena[0]+OUT,1514,(size_t*)(arena[0]+LEN));
  int r1=net_ipv4_echo_request((void*)arena[1],PI,0x564f,(uint16_t)n,rb(1,data),n,5000,arena[1]+OUT,1514,(size_t*)(arena[1]+LEN));
  same(r0,r1,"echo request",2);}
 { ctx_template(1,5000,2,0);load();uint8_t d[16]="VOS ping data!!";
   R(net_ipv4_echo_request)((void*)tmpl,PI,0x564f,1,d,13,5000,arena[0]+OUT,1514,(size_t*)(arena[0]+LEN));
   size_t len=icmp_frame(f,0,0x564f,1,d,13);
   for(size_t off=0;off<len;off++)for(unsigned k=0;k<3;k++){uint8_t g[1600];memcpy(g,f,len);g[off]^=(uint8_t)(1u<<(k*3));if(off>=14&&off<34)fix_ip(g);
    cmp_input(1,g,len,5000+(uint64_t)k*90,0,2);}
   for(int alias=0;alias<4;alias++)cmp_input(1,f,len,5050,alias,2);
   size_t rq=icmp_frame(f,8,0x1234,7,d,13);
   for(size_t off=0;off<rq;off++){uint8_t g[1600];memcpy(g,f,rq);g[off]^=0x10;if(off>=14&&off<34)fix_ip(g);cmp_input(1,g,rq,5000,0,2);}
   for(int alias=0;alias<4;alias++)cmp_input(1,f,rq,5000,alias,2);
   for(size_t n=0;n<=1472;n+=n<80?1:97){uint8_t big[1472];memset(big,(int)n,sizeof big);size_t l=icmp_frame(f,8,1,2,big,n);cmp_input(1,f,l,5000,0,2);} }
}
/* ---------------- stage 4: UDP endpoint ---------------- */
static void stage4(void){
 uint8_t f[1600],d[600];for(int i=0;i<600;i++)d[i]=(uint8_t)(i*13+1);
 /* init: port zero, invalid addresses */
 for(unsigned p=0;p<3;p++)for(unsigned m=0;m<3;m++){memset(tmpl,0x5a,ARENA);load();uint8_t mac[6];memcpy(mac,m==1?(const uint8_t[6]){1,2,3,4,5,6}:G,6);
  same(R(net_udp_init)((void*)arena[0],mac,m==2?(const uint8_t[4]){127,0,0,1}:GI,(uint16_t)(p==0?0:p*4000)),net_udp_init((void*)arena[1],mac,m==2?(const uint8_t[4]){127,0,0,1}:GI,(uint16_t)(p==0?0:p*4000)),"udp init",3);}
 /* send: every payload length, peers, ports, capacities, data aliasing output */
 static const uint8_t peers[6][4]={{10,0,2,2},{10,0,2,9},{0,1,2,3},{127,0,0,1},{224,0,0,1},{10,0,2,15}};
 for(size_t n=0;n<=513;n+=(n<70||n>500)?1:37)for(unsigned k=0;k<12;k++){
  ctx_template(2,9000,k<6?2:0,0);load();memcpy(arena[0]+IN2,d,600);memcpy(arena[1]+IN2,d,600);
  const uint8_t *peer=peers[k%6];uint16_t port=(uint16_t)(k==11?0:5000);size_t cap=k==7?41+n:1514;
  void *data=k==8?(void*)(arena[0]+OUT):(void*)(arena[0]+IN2);
  int r0=R(net_udp_send)((void*)arena[0],peer,port,rb(0,data),n,9000,arena[0]+OUT,cap,(size_t*)(arena[0]+LEN));
  int r1=net_udp_send((void*)arena[1],peer,port,rb(1,data),n,9000,arena[1]+OUT,cap,(size_t*)(arena[1]+LEN));
  same(r0,r1,"udp send",3);}
 /* sends whose computed UDP checksum is exactly 0 (transmitted as 0xffff) */
 { static const size_t lens[5]={2,3,64,511,512};
   for(unsigned li=0;li<5;li++){size_t n=lens[li];uint8_t u[8+512],dz[512];for(size_t j=0;j<n;j++)dz[j]=(uint8_t)(j*29+li);
    unsigned found=0;for(unsigned x=0;x<65536 && !found;x++){dz[n-2]=(uint8_t)(x>>8);dz[n-1]=(uint8_t)x;
     put(u,4000);put(u+2,5000);put(u+4,(unsigned)(8+n));put(u+6,0);memcpy(u+8,dz,n);if(udp_sum(GI,PI,u,8+n)==0)found=1;}
    if(!found)die("zero checksum construction",n);
    ctx_template(2,9000,2,0);load();
    same(R(net_udp_send)((void*)arena[0],PI,5000,dz,n,9000,arena[0]+OUT,1514,(size_t*)(arena[0]+LEN)),net_udp_send((void*)arena[1],PI,5000,dz,n,9000,arena[1]+OUT,1514,(size_t*)(arena[1]+LEN)),"udp send zero checksum",3);
    if(arena[1][OUT+40]!=0xff || arena[1][OUT+41]!=0xff)die("zero UDP checksum must be sent as 0xffff",n);} }
 /* input: datagram lengths, checksum present/absent/wrong, ports, every-offset corruption, queue full */
 for(size_t n=0;n<=513;n+=(n<40||n>505)?1:61)for(unsigned k=0;k<6;k++){
  ctx_template(2,9000,2,0);if(k==5)((struct net_udp*)tmpl)->count=NET_UDP_QUEUE;
  size_t len=n<=1472?udp_frame(f,5000,k==3?4001:k==4?0:4000,d,n,k!=1):0;if(k==2)f[40]^=1;
  cmp_input(2,f,len,9000,0,3);}
 { size_t len=udp_frame(f,5000,4000,d,17,1);
   for(size_t off=0;off<len;off++)for(unsigned k=0;k<4;k++){uint8_t g[1600];memcpy(g,f,len);g[off]=(uint8_t)(g[off]+1+k*63);if(off>=14&&off<34)fix_ip(g);ctx_template(2,9000,2,0);cmp_input(2,g,len,9000,0,3);}
   for(size_t l2=0;l2<=len;l2++){ctx_template(2,9000,2,0);cmp_input(2,f,l2,9000,0,3);}
   for(int alias=0;alias<4;alias++){ctx_template(2,9000,2,0);cmp_input(2,f,len,9000,alias,3);}
   size_t ping=icmp_frame(f,8,3,4,d,9);ctx_template(2,9000,2,0);cmp_input(2,f,ping,9000,0,3);   /* delegated ICMP */
   size_t ar=arp_frame(f,1,(const uint8_t[6]){255,255,255,255,255,255},P,PI,(const uint8_t[6]){0},GI);ctx_template(2,9000,0,0);cmp_input(2,f,ar,9000,0,3); }
 /* exhaustive UDP header fields: every length field value, every checksum value, every destination port */
 { size_t len=udp_frame(f,5000,4000,d,40,1);
   for(unsigned v=0;v<=1500;v++){uint8_t g[1600];memcpy(g,f,len);put(g+38,v);ctx_template(2,9000,2,0);cmp_input(2,g,len,9000,0,3);}
   ctx_template(2,9000,2,0);
   for(unsigned v=0;v<65536;v++){uint8_t g[1600];memcpy(g,f,len);put(g+40,v);cmp_input(2,g,len,9000,0,3);}
   for(unsigned v=0;v<65536;v++){uint8_t g[1600];size_t l=udp_frame(g,5000,v,d,8,v&1);cmp_input(2,g,l,9000,0,3);} }
 /* receive and close */
 for(unsigned head=0;head<4;head++)for(unsigned count=0;count<=4;count++){
  ctx_template(2,9000,0,0);struct net_udp *c=(struct net_udp*)tmpl;c->head=head;c->count=count;for(unsigned q=0;q<4;q++){c->queue[q].port=(uint16_t)(q+1);c->queue[q].length=(uint16_t)q;c->queue[q].data[0]=(uint8_t)q;}
  load();same(R(net_udp_receive)((void*)arena[0],(void*)(arena[0]+DGRAM)),net_udp_receive((void*)arena[1],(void*)(arena[1]+DGRAM)),"udp receive",3);
  load();R(net_udp_close)((void*)arena[0]);net_udp_close((void*)arena[1]);same(0,0,"udp close",3);}
 { ctx_template(2,9000,2,0);R(net_udp_close)((struct net_udp*)tmpl);load();   /* a closed endpoint (port 0) */
   same(R(net_udp_receive)((void*)arena[0],(void*)(arena[0]+DGRAM)),net_udp_receive((void*)arena[1],(void*)(arena[1]+DGRAM)),"udp receive closed",3); }
}
/* ---------------- random operation sequences over the full UDP context ---------------- */
static void sequences(void){
 uint8_t f[1600],d[600];
 for(unsigned s=0;s<3000;s++){
  uint64_t now=(s%3==0)?UINT64_MAX-3000:next64()%100000;
  memset(tmpl,0x5a,ARENA);if(R(net_udp_init)((struct net_udp*)tmpl,G,GI,4000))die("seq init",s);
  load();
  for(unsigned step=0;step<24;step++){
   now+=next()%120;unsigned op=next()%7;size_t len=0;
   for(size_t j=0;j<sizeof d;j++)d[j]=(uint8_t)next();
   if(op==0)len=arp_frame(f,1,(const uint8_t[6]){255,255,255,255,255,255},P,PI,(const uint8_t[6]){0},GI);
   else if(op==1)len=arp_frame(f,2,G,P,PI,G,GI);
   else if(op==2)len=udp_frame(f,5000,4000,d,next()%600,next()&1);
   else if(op==3)len=icmp_frame(f,(next()&1)?8:0,next()&0xffff,next()&0xffff,d,next()%64);
   if(op<=3 && len){if(next()%4==0){f[next()%len]^=(uint8_t)(1u<<(next()%8));}
    memcpy(arena[0]+FRAME,f,len);memcpy(arena[1]+FRAME,f,len);
    int r0=R(net_udp_input)((void*)arena[0],arena[0]+FRAME,len,now,arena[0]+OUT,1514,(size_t*)(arena[0]+LEN));
    int r1=net_udp_input((void*)arena[1],arena[1]+FRAME,len,now,arena[1]+OUT,1514,(size_t*)(arena[1]+LEN));
    if(r0!=r1||memcmp(arena[0],arena[1],ARENA))die("sequence input",s);}
   else if(op==4){size_t n=next()%520;
    int r0=R(net_udp_send)((void*)arena[0],PI,5000,d,n,now,arena[0]+OUT,1514,(size_t*)(arena[0]+LEN));
    int r1=net_udp_send((void*)arena[1],PI,5000,d,n,now,arena[1]+OUT,1514,(size_t*)(arena[1]+LEN));
    if(r0!=r1||memcmp(arena[0],arena[1],ARENA))die("sequence send",s);}
   else if(op==5){int r0=R(net_udp_receive)((void*)arena[0],(void*)(arena[0]+DGRAM)),r1=net_udp_receive((void*)arena[1],(void*)(arena[1]+DGRAM));
    if(r0!=r1||memcmp(arena[0],arena[1],ARENA))die("sequence receive",s);}
   else {int r0=R(net_arp_query)((void*)arena[0],PI,now,arena[0]+OUT,1514,(size_t*)(arena[0]+LEN)),r1=net_arp_query((void*)arena[1],PI,now,arena[1]+OUT,1514,(size_t*)(arena[1]+LEN));
    if(r0!=r1||memcmp(arena[0],arena[1],ARENA))die("sequence query",s);}
  }
  scenarios++;
 }
}
/* ---------------- Rust-only refusals (undefined in the accepted C contract) ---------------- */
static void refusals(void){
 static _Alignas(16) unsigned char buf[4096];struct net_udp *u=(void*)buf;memset(buf,0x5a,sizeof buf);
 if(net_udp_init(u,G,GI,4000))die("refusal init",0);
 unsigned char before[4096];memcpy(before,buf,sizeof buf);
 size_t len=0;
 /* frame or data overlapping the mutated context, and a misaligned context: refused, no change */
 if(net_udp_input(u,buf+8,60,1,buf+3000,1514,&len)!=NET_ARP_EINVAL || memcmp(buf,before,sizeof buf))die("frame overlapping context",0);
 if(net_udp_send(u,PI,5000,buf+16,8,1,buf+3000,1514,&len)!=NET_ARP_EINVAL || memcmp(buf,before,sizeof buf))die("data overlapping context",0);
 if(net_udp_init((void*)(buf+1),G,GI,4000)!=NET_ARP_EINVAL || memcmp(buf,before,sizeof buf))die("misaligned context",0);
 if(net_checksum(NULL,4)!=0xffff || net_ipv4_transport_checksum(NULL,GI,17,NULL,0)!=0xffff)die("null checksum input fails safe",0);
}
/* ---------------- benchmark ---------------- */
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
static volatile uint64_t sink;
static double timing(unsigned w,int rust){
 static uint8_t f[1600],d[600];static size_t ul,il,al;
 if(!ul){ul=udp_frame(f,5000,4000,d,64,1);}
 uint8_t g[1600];size_t len;
 if(w==2){il=icmp_frame(g,8,1,2,d,32);len=il;}else if(w==3){al=arp_frame(g,1,(const uint8_t[6]){255,255,255,255,255,255},P,PI,(const uint8_t[6]){0},GI);len=al;}else{memcpy(g,f,ul);len=ul;}
 unsigned iters=20000;uint64_t total=0;
 for(unsigned i=0;i<iters;i++){
  memcpy(arena[0],tmpl,2400);struct net_udp *c=(void*)arena[0];size_t ol;uint64_t a=ns();int r=0;
  switch(w){
  case 0:r=rust?net_checksum(g+34,len-34):R(net_checksum)(g+34,len-34);break;
  case 1:r=rust?net_udp_input(c,g,len,9000,arena[0]+OUT,1514,&ol):R(net_udp_input)(c,g,len,9000,arena[0]+OUT,1514,&ol);break;
  case 2:r=rust?net_ipv4_input(&c->ip,g,len,9000,arena[0]+OUT,1514,&ol):R(net_ipv4_input)(&c->ip,g,len,9000,arena[0]+OUT,1514,&ol);break;
  case 3:r=rust?net_ethernet_input(&c->ip.arp,g,len,9000,arena[0]+OUT,1514,&ol):R(net_ethernet_input)(&c->ip.arp,g,len,9000,arena[0]+OUT,1514,&ol);break;
  case 4:r=rust?net_udp_send(c,PI,5000,d,64,9000,arena[0]+OUT,1514,&ol):R(net_udp_send)(c,PI,5000,d,64,9000,arena[0]+OUT,1514,&ol);break;
  default:g[23]=6;r=rust?net_udp_input(c,g,len,9000,arena[0]+OUT,1514,&ol):R(net_udp_input)(c,g,len,9000,arena[0]+OUT,1514,&ol);g[23]=17;break; /* invalid input: protocol byte changed, IP checksum now wrong */
  }
  total+=ns()-a;sink+=(uint64_t)r;
 }
 return (double)total/iters;
}
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"--bench")){
  bench=1;ctx_template(2,9000,2,0);puts("workload,sample,C_ns,Rust_ns");
  for(unsigned w=0;w<6;w++){(void)timing(w,0);(void)timing(w,1);
   for(unsigned s=0;s<9;s++){double a,b;if(s&1){b=timing(w,1);a=timing(w,0);}else{a=timing(w,0);b=timing(w,1);}printf("%u,%u,%.4f,%.4f\n",w,s,a,b);}}
  return 0;
 }
 stage1();stage2();stage3();stage4();sequences();
#ifndef NET_INTERMEDIATE_STAGE   /* defined only to verify the intermediate stage commits, before UDP's Rust-only refusals exist */
 refusals();
#else
 (void)refusals;
#endif
 unsigned long long total=cases[0]+cases[1]+cases[2]+cases[3];
 printf("STAGES %llu %llu %llu %llu\n",cases[0],cases[1],cases[2],cases[3]);
 if(total!=NET_CASES || scenarios!=3000)die("comparison coverage count",total);
 printf("PASS %llu network comparisons and %llu random operation sequences; stages checksum/validation/arp-icmp/udp; whole-arena state, output, immutability and aliasing\n",total,scenarios);
 return 0;
}
