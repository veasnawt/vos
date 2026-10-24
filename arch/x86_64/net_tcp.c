#include "net_tcp.h"
#include "lib.h"
_Static_assert(sizeof(struct net_tcp)<=512,"bounded TCP context");
_Static_assert(NET_TCP_RETRY_TICKS*(NET_TCP_RETRY_MAX+1)<=NET_TCP_TX_TTL,"bounded retry horizon");
static uint16_t get16(const uint8_t *p) {return (uint16_t)((unsigned)p[0]*256+p[1]);}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static void put32(uint8_t *p,uint32_t n) {p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;}
int net_tcp_init(struct net_tcp *c,const uint8_t mac[6],const uint8_t ip[4],uint16_t port,uint32_t initial_sequence)
{
    if (!c || !port) return NET_ARP_EINVAL;
    struct net_ipv4 ipctx;int e=net_ipv4_init(&ipctx,mac,ip);if (e) return e;
    memset(c,0,sizeof(*c));memcpy(&c->ip,&ipctx,sizeof(ipctx));c->port=port;c->next_isn=initial_sequence;c->state=NET_TCP_LISTEN;return 0;
}
void net_tcp_abort(struct net_tcp *c)
{
    if (!c || !c->port) return;
    memset(c->peer,0,sizeof(c->peer));memset(c->peer_mac,0,sizeof(c->peer_mac));c->peer_port=0;
    c->isn=c->peer_isn=c->snd_next=c->rcv_next=0;c->since=0;c->state=NET_TCP_LISTEN;
    c->rx_used=c->tx_used=c->peer_window=0;c->window_dirty=0;
    c->snd_una=c->win_seq=c->win_ack=c->rcv_adv=0;c->tx_since=0;
    c->retry_count=0;c->retry_after=0;
    memset(c->rx,0,sizeof(c->rx));memset(c->tx,0,sizeof(c->tx));
}
static int payload_timeout(const struct net_tcp *c,uint64_t now)
{
    return now-c->since>=NET_TCP_IDLE_TTL || (c->tx_used && now-c->tx_since>=NET_TCP_TX_TTL);
}
int net_tcp_expire(struct net_tcp *c,uint64_t now)
{
    if (!c || !c->port) return NET_ARP_EINVAL;
    if (!((c->state==NET_TCP_SYN_RECEIVED && now-c->since>=NET_TCP_HANDSHAKE_TTL) ||
          (c->state==NET_TCP_ESTABLISHED && c->payload_enabled && payload_timeout(c,now)))) return 0;
    net_tcp_abort(c);return 1;
}
int net_tcp_enable_payload(struct net_tcp *c)
{
    if (!c || !c->port) return NET_ARP_EINVAL;
    if (c->state!=NET_TCP_LISTEN) return NET_ARP_BUSY;
    c->payload_enabled=1;return 0;
}
int net_tcp_enable_retransmission(struct net_tcp *c)
{
    if (!c || !c->port || !c->payload_enabled) return NET_ARP_EINVAL;
    if (c->state!=NET_TCP_LISTEN) return NET_ARP_BUSY;
    c->retry_enabled=1;return 0;
}
static int ready(const struct net_tcp *c,uint64_t now)
{
    if (!c || !c->port || !c->payload_enabled || c->state!=NET_TCP_ESTABLISHED) return NET_ARP_EINVAL;
    return payload_timeout(c,now)?NET_TCP_EXPIRED:0;
}
static int output_ok(void *out,size_t capacity,size_t *length,size_t n)
{
    uintptr_t a=(uintptr_t)out,b=(uintptr_t)length;
    return out && length && capacity>=n && a+n>a && b+sizeof(*length)>b &&
           !(a<b+sizeof(*length) && b<a+n);
}
static void shift(uint8_t *p,unsigned count,unsigned consumed)
{
    for (unsigned i=0;i<count-consumed;i++) p[i]=p[i+consumed];
    memset(p+count-consumed,0,consumed);
}
static int packet(const struct net_tcp *c,uint32_t seq,uint32_t ack,unsigned window,const void *data,size_t n,
                  uint8_t packet[54+NET_TCP_TX_MAX],void *out,size_t capacity,size_t *length,size_t *size)
{
    size_t wire=54+n;if (wire<60) wire=60;
    if (!output_ok(out,capacity,length,wire)) return NET_ARP_NOSPACE;
    int e=net_ipv4_begin(&c->ip,c->peer_mac,c->peer,6,20+n,packet,54+NET_TCP_TX_MAX,size);if (e) return e;
    uint8_t *h=packet+34;put16(h,c->port);put16(h+2,c->peer_port);put32(h+4,seq);put32(h+8,ack);
    h[12]=0x50;h[13]=n?24:16;put16(h+14,(uint16_t)window);
    if (n) memcpy(h+20,data,n);
    put16(h+16,net_ipv4_transport_checksum(c->ip.arp.ip,c->peer,6,h,20+n));return 0;
}
static void emitted(struct net_tcp *c,const uint8_t *packet,size_t n,void *out,size_t *length)
{
    memcpy(out,packet,n);*length=n;c->ip.identification++;
    c->rcv_adv=c->rcv_next+(NET_TCP_RX_MAX-c->rx_used);c->window_dirty=0;
}
int net_tcp_ack(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length)
{
    int e=ready(c,now);if (e) return e;
    uint8_t p[54+NET_TCP_TX_MAX];size_t n;
    e=packet(c,c->snd_next,c->rcv_next,NET_TCP_RX_MAX-c->rx_used,NULL,0,p,out,capacity,length,&n);if (e) return e;
    emitted(c,p,n,out,length);return NET_TCP_ACK;
}
int net_tcp_send(struct net_tcp *c,const void *data,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    int e=ready(c,now);if (e) return e;
    if (!data || !n || n>NET_TCP_TX_MAX) return NET_ARP_EINVAL;
    if (c->tx_used) return NET_ARP_BUSY;
    if (n>c->peer_window) return NET_TCP_WINDOW;
    uint8_t p[54+NET_TCP_TX_MAX];size_t size;
    e=packet(c,c->snd_next,c->rcv_next,NET_TCP_RX_MAX-c->rx_used,data,n,p,out,capacity,length,&size);if (e) return e;
    memset(c->tx,0,sizeof(c->tx));memcpy(c->tx,p+54,n);c->tx_used=(uint16_t)n;
    c->snd_next+=(uint32_t)n;c->tx_since=now;c->since=now;
    c->retry_count=0;c->retry_after=c->retry_enabled?NET_TCP_RETRY_TICKS:0;
    emitted(c,p,size,out,length);return NET_TCP_TX_DATA;
}
int net_tcp_retransmit(struct net_tcp *c,uint64_t now,void *out,size_t capacity,size_t *length)
{
    if (!c || !c->retry_enabled) return NET_ARP_EINVAL;
    int e=ready(c,now);
    if (e==NET_TCP_EXPIRED) {net_tcp_abort(c);return e;}
    if (e) return e;
    if (!c->tx_used) return NET_TCP_EMPTY;
    if (c->tx_used!=(uint32_t)(c->snd_next-c->snd_una)) return NET_ARP_EINVAL;
    uint64_t elapsed=now-c->tx_since;
    if (elapsed<c->retry_after) return NET_TCP_NOT_DUE;
    if (c->retry_count>=NET_TCP_RETRY_MAX) {net_tcp_abort(c);return NET_TCP_EXPIRED;}
    size_t n=c->tx_used;if (n>c->peer_window) n=c->peer_window;
    if (!n) return NET_TCP_WINDOW; /* Persist probes are outside this profile. */
    uint8_t p[54+NET_TCP_TX_MAX];size_t size;
    e=packet(c,c->snd_una,c->rcv_next,NET_TCP_RX_MAX-c->rx_used,c->tx,n,p,out,capacity,length,&size);if (e) return e;
    c->retry_count++;c->retry_after=(uint32_t)elapsed+NET_TCP_RETRY_TICKS;
    emitted(c,p,size,out,length);return NET_TCP_RETRANSMITTED;
}
int net_tcp_receive(struct net_tcp *c,void *out,size_t capacity,size_t *length,uint64_t now)
{
    int e=ready(c,now);if (e) return e;
    if (!capacity || !out || !length) return NET_ARP_NOSPACE;
    if (!c->rx_used) return NET_TCP_EMPTY;
    size_t n=c->rx_used;if (n>capacity) n=capacity;
    if (!output_ok(out,capacity,length,n)) return NET_ARP_NOSPACE;
    memcpy(out,c->rx,n);shift(c->rx,c->rx_used,(unsigned)n);c->rx_used-=(uint16_t)n;
    *length=n;c->window_dirty=1;c->since=now;return 0;
}
static int payload_input(struct net_tcp *c,const uint8_t *tcp,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    if (payload_timeout(c,now)) return NET_TCP_EXPIRED;
    uint32_t seq=get32(tcp+4),ack=get32(tcp+8),delta=ack-c->snd_una,outstanding=c->snd_next-c->snd_una;
    int ack_current=delta<=outstanding;
    /* Older ACKs are ignored; future/ambiguous ACKs invalidate the segment. */
    if (!ack_current && (uint32_t)(c->snd_una-ack)>=0x80000000u) return NET_ARP_DROP;
    uint32_t room=c->rcv_adv-c->rcv_next;unsigned actual=NET_TCP_RX_MAX-c->rx_used;
    if (room>actual) room=actual;
    int sequence_ok=n?(seq==c->rcv_next && n<=room):(room?(uint32_t)(seq-c->rcv_next)<room:seq==c->rcv_next);
    if (!sequence_ok) return net_tcp_ack(c,now,out,capacity,length);
    uint8_t p[54+NET_TCP_TX_MAX];size_t size=0;
    if (n) {
        int e=packet(c,c->snd_next,c->rcv_next+(uint32_t)n,actual-(unsigned)n,NULL,0,p,out,capacity,length,&size);if (e) return e;
    }
    if (ack_current) {
        if (delta) {
            shift(c->tx,c->tx_used,(unsigned)delta);c->tx_used-=(uint16_t)delta;c->snd_una=ack;c->since=now;
            if (!c->tx_used) {c->tx_since=0;c->retry_after=0;c->retry_count=0;}
            else if (c->retry_enabled) c->retry_after=(uint32_t)(now-c->tx_since)+NET_TCP_RETRY_TICKS;
        }
        if ((uint32_t)(seq-c->win_seq)<0x80000000u && (seq!=c->win_seq || (uint32_t)(ack-c->win_ack)<0x80000000u)) {
            unsigned window=get16(tcp+14);if (window!=c->peer_window) c->since=now;
            c->peer_window=(uint16_t)window;c->win_seq=seq;c->win_ack=ack;
        }
    }
    if (!n) return NET_TCP_ACKED;
    memcpy(c->rx+c->rx_used,tcp+20,n);c->rx_used+=(uint16_t)n;c->rcv_next+=(uint32_t)n;c->since=now;
    emitted(c,p,size,out,length);return NET_TCP_DATA_ACK;
}
int net_tcp_input(struct net_tcp *c,const void *frame,size_t n,uint64_t now,void *out,size_t capacity,size_t *length)
{
    if (!c || !c->port) return NET_ARP_EINVAL;
    struct net_ipv4_payload view;int e=net_ipv4_payload(&c->ip,frame,n,6,&view);
    if (e==NET_ARP_UNSUPPORTED) return net_ipv4_input(&c->ip,frame,n,now,out,capacity,length);
    if (e!=1) return e;
    const uint8_t *tcp=view.bytes,*p=frame;
    if (view.length<20) return NET_ARP_EINVAL;
    size_t header=(tcp[12]>>4)*4u;
    if (header<20 || header>view.length || (tcp[12]&15) || !get16(tcp) || !get16(tcp+2) ||
        net_ipv4_transport_checksum(view.source,c->ip.arp.ip,6,tcp,view.length)) return NET_ARP_EINVAL;
    if (header!=20 || get16(tcp+18)) return NET_ARP_UNSUPPORTED;
    size_t data_length=view.length-20;
    if (data_length && (!c->payload_enabled || c->state!=NET_TCP_ESTABLISHED)) return NET_ARP_UNSUPPORTED;
    if (get16(tcp+2)!=c->port) return NET_ARP_DROP;
    uint8_t flags=tcp[13];uint32_t seq=get32(tcp+4),ack=get32(tcp+8);
    if (flags!=2 && flags!=16 && !(flags==24 && c->payload_enabled && c->state==NET_TCP_ESTABLISHED)) return NET_ARP_UNSUPPORTED;
    if (c->state==NET_TCP_ESTABLISHED && c->payload_enabled) {
        if ((flags!=16 && flags!=24) || get16(tcp)!=c->peer_port || memcmp(view.source,c->peer,4) || memcmp(p+6,c->peer_mac,6)) return NET_ARP_DROP;
        return payload_input(c,tcp,data_length,now,out,capacity,length);
    }
    if (c->state==NET_TCP_LISTEN) {
        if (flags!=2 || ack) return NET_ARP_DROP;
        uint8_t packet[60],peer[4],mac[6];size_t size;
        memcpy(peer,view.source,4);memcpy(mac,p+6,6);
        e=net_ipv4_begin(&c->ip,mac,peer,6,20,packet,sizeof(packet),&size);if (e) return e;
        /* Validate caller capacity before changing any connection or output. */
        uintptr_t a=(uintptr_t)out,b=(uintptr_t)length;
        if (!out || !length || capacity<size || a+size<=a || b+sizeof(*length)<=b ||
            (a<b+sizeof(*length) && b<a+size)) return NET_ARP_NOSPACE;
        uint8_t *reply=packet+34;put16(reply,c->port);put16(reply+2,get16(tcp));
        put32(reply+4,c->next_isn);put32(reply+8,seq+1u);reply[12]=0x50;reply[13]=18;
        put16(reply+14,c->payload_enabled?NET_TCP_RX_MAX:0);
        /* Legacy profile keeps zero window; opt-in payload capacity is real. */
        put16(reply+16,net_ipv4_transport_checksum(c->ip.arp.ip,peer,6,reply,20));
        memcpy(c->peer,peer,4);memcpy(c->peer_mac,mac,6);c->peer_port=get16(tcp);
        c->isn=c->next_isn;c->next_isn+=0x10000u;c->peer_isn=seq;c->snd_next=c->isn+1u;c->rcv_next=seq+1u;
        c->since=now;c->state=NET_TCP_SYN_RECEIVED;c->ip.identification++;
        if (c->payload_enabled) {c->snd_una=c->snd_next;c->rcv_adv=c->rcv_next+NET_TCP_RX_MAX;}
        memcpy(out,packet,size);*length=size;return NET_TCP_SYNACK;
    }
    if (c->state==NET_TCP_SYN_RECEIVED && now-c->since>=NET_TCP_HANDSHAKE_TTL) return NET_TCP_EXPIRED;
    if (c->state!=NET_TCP_SYN_RECEIVED || flags!=16 || get16(tcp)!=c->peer_port ||
        memcmp(view.source,c->peer,4) || memcmp(p+6,c->peer_mac,6) || seq!=c->rcv_next || ack!=c->snd_next) return NET_ARP_DROP;
    c->state=NET_TCP_ESTABLISHED;
    if (c->payload_enabled) {c->peer_window=get16(tcp+14);c->win_seq=seq;c->win_ack=ack;c->since=now;}
    return NET_TCP_CONNECTED;
}
void net_tcp_close(struct net_tcp *c) {if (c) memset(c,0,sizeof(*c));}
