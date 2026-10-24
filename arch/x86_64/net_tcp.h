#ifndef VOS_X86_64_NET_TCP_H
#define VOS_X86_64_NET_TCP_H
#include "net_ipv4.h"
#define NET_TCP_HANDSHAKE_TTL 200u
#define NET_TCP_RX_MAX 64u
#define NET_TCP_TX_MAX 64u
#define NET_TCP_TX_TTL 200u
#define NET_TCP_IDLE_TTL 400u
#define NET_TCP_RETRY_TICKS 50u
#define NET_TCP_RETRY_MAX 3u
enum { NET_TCP_CLOSED,NET_TCP_LISTEN,NET_TCP_SYN_RECEIVED,NET_TCP_ESTABLISHED };
enum { NET_TCP_SYNACK=9,NET_TCP_CONNECTED=10,NET_TCP_EXPIRED=-8 };
enum { NET_TCP_DATA_ACK=11,NET_TCP_ACK=12,NET_TCP_TX_DATA=13,NET_TCP_ACKED=14,
       NET_TCP_EMPTY=-9,NET_TCP_WINDOW=-10 };
enum { NET_TCP_RETRANSMITTED=15,NET_TCP_NOT_DUE=-11 };
struct net_tcp {
    struct net_ipv4 ip;
    uint32_t next_isn,isn,peer_isn,snd_next,rcv_next;
    uint8_t peer[4],peer_mac[6];
    uint16_t port,peer_port;
    uint8_t retry_enabled,retry_count;
    uint32_t retry_after; /* Relative to tx_since, bounded by absolute TX TTL. */
    uint64_t since;
    int state;
    uint16_t rx_used,tx_used,peer_window;
    uint8_t payload_enabled,window_dirty;
    uint32_t snd_una,win_seq,win_ack,rcv_adv;
    uint64_t tx_since;
    uint8_t rx[NET_TCP_RX_MAX],tx[NET_TCP_TX_MAX];
};
/* One passive kernel endpoint and one fixed connection record. Single owner,
 * IF=0; same buffer/alias contract as ARP. Opt-in fixed payload buffers; no
 * allocation or app ABI.
 * initial_sequence is supplied by the trusted caller. Deterministic test ISNs
 * are not a production unpredictable ISN generator (RFC 9293 section 3.4.1). */
int net_tcp_init(struct net_tcp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port,uint32_t initial_sequence);
/* Emits only SYN-ACK. Other supported transition is a validated final ACK.
 * Bad/unsupported input and short output leave state/output unchanged.
 * Expired half-open input cannot establish a connection; call expire to clear. */
int net_tcp_input(struct net_tcp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length);
/* Explicit timeout: erase connection state and buffers, return to LISTEN.
 * Payload mode also expires unacknowledged TX and idle established connections.
 * Returns 1 on expiry, 0 otherwise. Local abort has no FIN/RST exchange. */
int net_tcp_expire(struct net_tcp *c,uint64_t now);
/* Local abort only: erase endpoint and state. No FIN/RST exchange implemented. */
void net_tcp_close(struct net_tcp *c);
/* Explicit opt-in preserves the establishment-only profile's zero window.
 * Fixed buffers, one in-flight segment; retransmission is a separate opt-in. */
int net_tcp_enable_payload(struct net_tcp *c);
int net_tcp_send(struct net_tcp *c,const void *data,size_t n,uint64_t now,void *out,size_t capacity,size_t *length);
/* Partial reads copy at most capacity bytes; no read acknowledges new bytes.
 * Caller advertises freed capacity via ack or a later outgoing segment. */
int net_tcp_receive(struct net_tcp *c,void *out,size_t capacity,size_t *length,uint64_t now);
int net_tcp_ack(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length);
/* Explicit local reset: erase all connection buffers/sequence/deadline state,
 * preserve endpoint configuration and return to LISTEN. No wire FIN/RST. */
void net_tcp_abort(struct net_tcp *c);
/* LISTEN-only configuration. Caller polls with monotonic PIT ticks; fixed RTO,
 * finite retries and unchanged absolute TX TTL. No SYN/FIN/RST retransmission.
 * Terminal expiry explicitly aborts the connection and erases its buffers. */
int net_tcp_enable_retransmission(struct net_tcp *c);
int net_tcp_retransmit(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length);
#endif
