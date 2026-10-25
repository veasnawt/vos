/* Optimized hosted closure model; time jumps are API ticks, not live PIT time. */
#define main legacy_tcp_tests_main
#include "net-tcp.c"
#undef main
#include <time.h>
#include <stdlib.h>
static volatile unsigned long sink;
static uint64_t stamp(void) {struct timespec t;if(clock_gettime(CLOCK_MONOTONIC_RAW,&t)) abort();return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static double sample(const struct net_tcp *saved,const uint8_t *fin)
{
    uint64_t start=stamp();uint8_t out[128];size_t n;
    for(unsigned i=0;i<20000;i++) {
        struct net_tcp c=*saved;
        if(net_tcp_finish(&c,3,out,sizeof(out),&n)!=NET_TCP_FIN_SENT ||
           net_tcp_input(&c,fin,60,4,out,sizeof(out),&n)!=NET_TCP_FIN_ACK ||
           c.state!=NET_TCP_TIME_WAIT || net_tcp_expire(&c,4+NET_TCP_TIME_WAIT_TTL)!=NET_TCP_DONE) abort();
        sink+=c.closure_enabled;
    }
    return (double)(stamp()-start)/20000;
}
int main(void)
{
    struct net_tcp c;uint8_t p[1514],out[128],fin[1514];size_t n;
    if(net_tcp_init(&c,G,GI,4001,0x11223344) || net_tcp_enable_payload(&c) || net_tcp_enable_retransmission(&c) || net_tcp_enable_closure(&c)) abort();
    packet(p,2,12,0,5,0);if(net_tcp_input(&c,p,60,1,out,sizeof(out),&n)!=NET_TCP_SYNACK) abort();
    packet(p,16,13,0x11223345,5,0);if(net_tcp_input(&c,p,60,2,out,sizeof(out),&n)!=NET_TCP_CONNECTED) abort();
    packet(fin,17,13,0x11223346,5,0);sample(&c,fin);
    printf("{\"kind\":\"optimized_host_active_FIN_peer_FIN_and_linger_cleanup_with_context_restore\",\"context_bytes\":%zu,\"iterations\":20000,\"ns_per_close\":[",sizeof(c));
    for(unsigned i=0;i<7;i++) printf("%s%.3f",i?",":"",sample(&c,fin));
    printf("],\"sink\":%lu}\n",sink);return 0;
}
