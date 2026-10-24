/* Optimized hosted protocol model, separate from sanitizer and QEMU results. */
#define main tcp_checks_main
#include "net-tcp.c"
#undef main
#include <time.h>
#include <stdlib.h>
static volatile uint64_t bench_sink;
static uint64_t stamp(void)
{
    struct timespec t;if (clock_gettime(CLOCK_MONOTONIC_RAW,&t)) abort();
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
static double sample(unsigned iterations,int payload)
{
    const uint32_t initial=0x11223344;
    uint8_t syn[1514],ack[1514],reply[128];size_t n;
    packet(syn,2,12,0,5,0);packet(ack,16,13,initial+1,5,0);
#ifdef VOS_TCP_PAYLOAD_BENCH
    uint8_t incoming[1514],complete[1514],received[2];
    packet(incoming,24,13,initial+1,5,2);incoming[54]='A';incoming[55]='B';fix_tcp(incoming,22);
    packet(complete,16,15,initial+3,5,0);
#else
    (void)payload;
#endif
    uint64_t start=stamp();
    for (unsigned i=0;i<iterations;i++) {
        struct net_tcp c;
        if (net_tcp_init(&c,G,GI,4001,initial)) abort();
#ifdef VOS_TCP_PAYLOAD_BENCH
        if (payload && net_tcp_enable_payload(&c)) abort();
#endif
        if (net_tcp_input(&c,syn,60,1,reply,sizeof(reply),&n)!=NET_TCP_SYNACK ||
            net_tcp_input(&c,ack,60,2,reply,sizeof(reply),&n)!=NET_TCP_CONNECTED) abort();
#ifdef VOS_TCP_PAYLOAD_BENCH
        if (payload) {
            if (net_tcp_input(&c,incoming,60,3,reply,sizeof(reply),&n)!=NET_TCP_DATA_ACK ||
                net_tcp_receive(&c,received,sizeof(received),&n,4) || n!=2 ||
                net_tcp_send(&c,"CD",2,5,reply,sizeof(reply),&n)!=NET_TCP_TX_DATA ||
                net_tcp_input(&c,complete,60,6,reply,sizeof(reply),&n)!=NET_TCP_ACKED || c.tx_used) abort();
            bench_sink+=received[0]+c.snd_una;
        }
#endif
        bench_sink+=(unsigned)c.state;net_tcp_close(&c);
    }
    return (double)(stamp()-start)/iterations;
}
int main(void)
{
    sample(3000,0);
    printf("{\"kind\":\"optimized_host_protocol_model\",\"iterations_per_sample\":20000,\"context_bytes\":%zu,\"legacy_ns_per_handshake\":[",sizeof(struct net_tcp));
    for (unsigned i=0;i<7;i++) printf("%s%.3f",i?",":"",sample(20000,0));
#ifdef VOS_TCP_PAYLOAD_BENCH
    sample(3000,1);printf("],\"payload_ns_per_two_byte_bidirectional_transaction_with_setup\":[");
    for (unsigned i=0;i<7;i++) printf("%s%.3f",i?",":"",sample(20000,1));
#endif
    printf("],\"sink\":%llu}\n",(unsigned long long)bench_sink);return 0;
}
