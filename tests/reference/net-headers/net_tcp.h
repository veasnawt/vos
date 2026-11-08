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
/* Existing enum values are stable; closure is separately opt-in. */
enum { NET_TCP_CLOSED,NET_TCP_LISTEN,NET_TCP_SYN_RECEIVED,NET_TCP_ESTABLISHED,
       NET_TCP_CLOSE_WAIT,NET_TCP_FIN_WAIT_1,NET_TCP_FIN_WAIT_2,
       NET_TCP_CLOSING,NET_TCP_LAST_ACK,NET_TCP_TIME_WAIT };
/* 100 Hz caller clock. Internet-style 2-minute MSL, not a shortened demo linger. */
#define NET_TCP_MSL_TICKS 12000u
#define NET_TCP_TIME_WAIT_TTL (2u*NET_TCP_MSL_TICKS)
#define NET_TCP_CLOSE_TTL 400u
#define NET_TCP_CLOSE_HARD_TTL (4u*NET_TCP_MSL_TICKS)
enum { NET_TCP_FIN_SENT=16,NET_TCP_FIN_ACK=17,NET_TCP_RESET=18,NET_TCP_DONE=19,NET_TCP_EOF=-12 };
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
    uint8_t closure_enabled; /* Uses pre-existing padding; context remains 504 B. */
    uint64_t tx_since;
    uint8_t rx[NET_TCP_RX_MAX],tx[NET_TCP_TX_MAX];
};
/* One passive kernel endpoint and one fixed connection record. Single owner,
 * IF=0; same buffer/alias contract as ARP. Opt-in fixed payload buffers; no
 * allocation or app ABI.
 * initial_sequence is supplied by the trusted caller. Deterministic test ISNs
 * are not a production unpredictable ISN generator (RFC 9293 section 3.4.1). */
int net_tcp_init(struct net_tcp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port,uint32_t initial_sequence);
/* Applies the enabled handshake/payload/closure profile; may emit SYN-ACK or ACK.
 * Invalid sequences may elicit an ACK; required response buffers are checked before commit.
 * Expired half-open input cannot establish a connection; call expire to clear. */
int net_tcp_input(struct net_tcp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length);
/* Explicit timeout: erase connection state and buffers, return to LISTEN.
 * Payload mode also expires unacknowledged TX and idle established connections.
 * Returns 1 on abort expiry, DONE on completed TIME-WAIT, 0 otherwise. */
int net_tcp_expire(struct net_tcp *c,uint64_t now);
/* Local abort only: erase endpoint and state. Does not emit a wire packet. */
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
 * preserve endpoint configuration and return to LISTEN. Does not emit a wire packet. */
void net_tcp_abort(struct net_tcp *c);
/* LISTEN-only configuration. Caller polls with monotonic PIT ticks; fixed RTO,
 * finite retries and unchanged absolute TX TTL. No SYN/RST retransmission; opt-in closure can retry FIN.
 * Terminal expiry explicitly aborts the connection and erases its buffers. */
int net_tcp_enable_retransmission(struct net_tcp *c);
int net_tcp_retransmit(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length);
/* LISTEN-only opt-in, requires payload mode. Single owner, same buffer contract.
 * finish returns BUSY until both buffers are drained, WINDOW on zero peer window.
 * FIN retries reuse the fixed data retry budget; no new allocation.
 * receive returns EOF after buffered peer data has been drained.
 * expire returns DONE after normal TIME-WAIT cleanup, 1 after an abort deadline.
 * LAST-ACK completion returns DONE from input; RESET explicitly discards buffers.
 * Controls with unsupported options still fail closed. No authentication of peers.
 */
int net_tcp_enable_closure(struct net_tcp *c);
int net_tcp_finish(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length);
#endif
