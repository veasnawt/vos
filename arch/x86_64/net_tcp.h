#ifndef VOS_X86_64_NET_TCP_H
#define VOS_X86_64_NET_TCP_H
#include "net_ipv4.h"
#define NET_TCP_HANDSHAKE_TTL 200u
enum { NET_TCP_CLOSED,NET_TCP_LISTEN,NET_TCP_SYN_RECEIVED,NET_TCP_ESTABLISHED };
enum { NET_TCP_SYNACK=9,NET_TCP_CONNECTED=10,NET_TCP_EXPIRED=-8 };
struct net_tcp {
    struct net_ipv4 ip;
    uint32_t next_isn,isn,peer_isn,snd_next,rcv_next;
    uint8_t peer[4],peer_mac[6];
    uint16_t port,peer_port;
    uint64_t since;
    int state;
};
/* One passive kernel endpoint and one fixed connection record. Single owner,
 * IF=0; same buffer/alias contract as ARP. No data queue, allocation or app ABI.
 * initial_sequence is supplied by the trusted caller. Deterministic test ISNs
 * are not a production unpredictable ISN generator (RFC 9293 section 3.4.1). */
int net_tcp_init(struct net_tcp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port,uint32_t initial_sequence);
/* Emits only SYN-ACK. Other supported transition is a validated final ACK.
 * Bad/unsupported input and short output leave state/output unchanged.
 * Expired half-open input cannot establish a connection; call expire to clear. */
int net_tcp_input(struct net_tcp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length);
/* Explicit half-open timeout: erase peer/sequence state, return to LISTEN.
 * Returns 1 on expiry, 0 otherwise. Established cleanup is caller-owned close. */
int net_tcp_expire(struct net_tcp *c,uint64_t now);
/* Local abort only: erase endpoint and state. No FIN/RST exchange implemented. */
void net_tcp_close(struct net_tcp *c);
#endif
