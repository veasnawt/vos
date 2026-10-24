/* Optimized hosted retry model, not a sanitizer or kernel throughput test. */
#define main legacy_tcp_checks_main
#include "net-tcp.c"
#undef main
#include <stdlib.h>
#include <time.h>
static volatile uint64_t sink;
static uint64_t stamp(void) {struct timespec t;if(clock_gettime(CLOCK_MONOTONIC_RAW,&t)) abort();return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static double sample(const struct net_tcp *saved,unsigned iterations)
{
    uint8_t out[128];size_t n;uint64_t start=stamp();
    for (unsigned i=0;i<iterations;i++) {
        struct net_tcp c=*saved;
        if (net_tcp_retransmit(&c,53,out,sizeof(out),&n)!=NET_TCP_RETRANSMITTED || n!=118 || c.tx_used!=64) abort();
        sink+=out[54]+c.retry_count;
    }
    return (double)(stamp()-start)/iterations;
}
int main(void)
{
    struct net_tcp c;uint8_t syn[1514],ack[1514],out[128],data[64];size_t n;
    memset(data,0x5a,sizeof(data));packet(syn,2,12,0,5,0);packet(ack,16,13,0x11223345,5,0);
    if (net_tcp_init(&c,G,GI,4001,0x11223344) || net_tcp_enable_payload(&c) || net_tcp_enable_retransmission(&c) ||
        net_tcp_input(&c,syn,60,1,out,sizeof(out),&n)!=NET_TCP_SYNACK || net_tcp_input(&c,ack,60,2,out,sizeof(out),&n)!=NET_TCP_CONNECTED ||
        net_tcp_send(&c,data,64,3,out,sizeof(out),&n)!=NET_TCP_TX_DATA) abort();
    sample(&c,3000);printf("{\"kind\":\"optimized_host_retry_with_504_byte_state_restore\",\"context_bytes\":%zu,\"iterations\":20000,\"ns_per_retry\":[",sizeof(c));
    for (unsigned i=0;i<7;i++) printf("%s%.3f",i?",":"",sample(&c,20000));
    printf("],\"sink\":%llu}\n",(unsigned long long)sink);return 0;
}
