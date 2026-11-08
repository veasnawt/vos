//! R3-N2: the bounded passive TCP prototype (accepted N5 `net_tcp.c` at
//! f8731c9), exactly as the accepted C. One passive endpoint, one connection
//! record, fixed 64-byte RX/TX buffers, opt-in payload, retransmission and
//! closure profiles. Unsigned arithmetic wraps as in C; sequence comparisons
//! are the C serial tests. Generated segments are built in the caller's local
//! `Sink` frame and copied out by the adapter last; every response is prepared
//! (and its output admitted) before any state commits, as in the C.
#![forbid(unsafe_code)]
use super::checksum::{be16, output_ok, put16, transport_checksum};
use super::context::{Ipv4, BUSY, DROP, EINVAL, NOSPACE, UNSUPPORTED};
use super::ipv4::{begin, payload};
use core::mem::{align_of, offset_of, size_of};

pub const HANDSHAKE_TTL: u64 = 200;
pub const RX_MAX: usize = 64;
pub const TX_MAX: usize = 64;
pub const TX_TTL: u64 = 200;
pub const IDLE_TTL: u64 = 400;
pub const RETRY_TICKS: u32 = 50;
pub const RETRY_MAX: u8 = 3;
pub const MSL_TICKS: u64 = 12000;
pub const TIME_WAIT_TTL: u64 = 2 * MSL_TICKS;
pub const CLOSE_TTL: u64 = 400;
pub const CLOSE_HARD_TTL: u64 = 4 * MSL_TICKS;

// NET_TCP_CLOSED (0) is the state of a zeroed (closed) context; nothing sets it explicitly.
pub const LISTEN: i32 = 1;
pub const SYN_RECEIVED: i32 = 2;
pub const ESTABLISHED: i32 = 3;
pub const CLOSE_WAIT: i32 = 4;
pub const FIN_WAIT_1: i32 = 5;
pub const FIN_WAIT_2: i32 = 6;
pub const CLOSING: i32 = 7;
pub const LAST_ACK: i32 = 8;
pub const TIME_WAIT: i32 = 9;

pub const SYNACK: i32 = 9;
pub const CONNECTED: i32 = 10;
pub const EXPIRED: i32 = -8;
pub const DATA_ACK: i32 = 11;
pub const ACK: i32 = 12;
pub const TX_DATA: i32 = 13;
pub const ACKED: i32 = 14;
pub const EMPTY: i32 = -9;
pub const WINDOW: i32 = -10;
pub const RETRANSMITTED: i32 = 15;
pub const NOT_DUE: i32 = -11;
pub const FIN_SENT: i32 = 16;
pub const FIN_ACK: i32 = 17;
pub const RESET: i32 = 18;
pub const DONE: i32 = 19;
pub const EOF: i32 = -12;

/// Largest generated segment: Ethernet 14 + IPv4 20 + TCP 20 + 64 data.
pub const WIRE: usize = 54 + TX_MAX;

/// Exact `#[repr(C)]` mirror of `struct net_tcp` (net_tcp.h). The three bytes
/// after `closure_enabled` are the C padding, kept as an explicit field so a
/// whole-context reset writes the same bytes as the C `memset` and field
/// stores never touch it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Tcp {
    pub ip: Ipv4,
    pub next_isn: u32,
    pub isn: u32,
    pub peer_isn: u32,
    pub snd_next: u32,
    pub rcv_next: u32,
    pub peer: [u8; 4],
    pub peer_mac: [u8; 6],
    pub port: u16,
    pub peer_port: u16,
    pub retry_enabled: u8,
    pub retry_count: u8,
    pub retry_after: u32,
    pub since: u64,
    pub state: i32,
    pub rx_used: u16,
    pub tx_used: u16,
    pub peer_window: u16,
    pub payload_enabled: u8,
    pub window_dirty: u8,
    pub snd_una: u32,
    pub win_seq: u32,
    pub win_ack: u32,
    pub rcv_adv: u32,
    pub closure_enabled: u8,
    pub pad0: [u8; 3],
    pub tx_since: u64,
    pub rx: [u8; RX_MAX],
    pub tx: [u8; TX_MAX],
}

const _: () = {
    assert!(size_of::<Tcp>() == 504 && align_of::<Tcp>() == 8);
    assert!(offset_of!(Tcp, next_isn) == 288 && offset_of!(Tcp, isn) == 292 && offset_of!(Tcp, peer_isn) == 296);
    assert!(offset_of!(Tcp, snd_next) == 300 && offset_of!(Tcp, rcv_next) == 304 && offset_of!(Tcp, peer) == 308);
    assert!(offset_of!(Tcp, peer_mac) == 312 && offset_of!(Tcp, port) == 318 && offset_of!(Tcp, peer_port) == 320);
    assert!(offset_of!(Tcp, retry_enabled) == 322 && offset_of!(Tcp, retry_count) == 323 && offset_of!(Tcp, retry_after) == 324);
    assert!(offset_of!(Tcp, since) == 328 && offset_of!(Tcp, state) == 336 && offset_of!(Tcp, rx_used) == 340);
    assert!(offset_of!(Tcp, tx_used) == 342 && offset_of!(Tcp, peer_window) == 344 && offset_of!(Tcp, payload_enabled) == 346);
    assert!(offset_of!(Tcp, window_dirty) == 347 && offset_of!(Tcp, snd_una) == 348 && offset_of!(Tcp, win_seq) == 352);
    assert!(offset_of!(Tcp, win_ack) == 356 && offset_of!(Tcp, rcv_adv) == 360 && offset_of!(Tcp, closure_enabled) == 364);
    assert!(offset_of!(Tcp, pad0) == 365 && offset_of!(Tcp, tx_since) == 368 && offset_of!(Tcp, rx) == 376 && offset_of!(Tcp, tx) == 440);
};

impl Tcp {
    pub const ZERO: Tcp = Tcp { ip: Ipv4::ZERO, next_isn: 0, isn: 0, peer_isn: 0, snd_next: 0, rcv_next: 0, peer: [0; 4], peer_mac: [0; 6],
        port: 0, peer_port: 0, retry_enabled: 0, retry_count: 0, retry_after: 0, since: 0, state: 0, rx_used: 0, tx_used: 0, peer_window: 0,
        payload_enabled: 0, window_dirty: 0, snd_una: 0, win_seq: 0, win_ack: 0, rcv_adv: 0, closure_enabled: 0, pad0: [0; 3], tx_since: 0,
        rx: [0; RX_MAX], tx: [0; TX_MAX] };
}

/// The caller's output (addresses only, never dereferenced here) and the local
/// segment the core builds. `len != 0` after a call means "copy frame[..len]
/// to out and store len to *length", exactly where the C `emitted` / final
/// `memcpy` would have.
pub struct Sink {
    pub out: usize,
    pub capacity: usize,
    pub length: usize,
    pub frame: [u8; WIRE],
    pub len: usize,
}
impl Sink {
    pub fn new(out: usize, capacity: usize, length: usize) -> Sink {
        Sink { out, capacity, length, frame: [0; WIRE], len: 0 }
    }
}

/// A fixed 20-byte TCP header (options are rejected before any use).
type Header = [u8; 20];
#[inline(always)]
fn get16(h: &Header, at: usize) -> u16 {
    be16(&h[at..at + 2])
}
#[inline(always)]
fn get32(h: &Header, at: usize) -> u32 {
    u32::from_be_bytes([h[at], h[at + 1], h[at + 2], h[at + 3]])
}
#[inline(always)]
fn put32(f: &mut [u8; WIRE], at: usize, n: u32) {
    f[at..at + 4].copy_from_slice(&n.to_be_bytes());
}

/// `net_tcp_init`: `mac`/`ip` are copies taken before the context is touched
/// (they may alias it); a failed IPv4 init leaves the context untouched.
pub fn tcp_init(c: &mut Tcp, mac: &[u8; 6], ip: &[u8; 4], port: u16, initial_sequence: u32) -> i32 {
    if port == 0 {
        return EINVAL;
    }
    let mut ipctx = Ipv4::ZERO;
    let e = super::arp::ipv4_init(&mut ipctx, mac, ip);
    if e != 0 {
        return e;
    }
    *c = Tcp::ZERO;
    c.ip = ipctx;
    c.port = port;
    c.next_isn = initial_sequence;
    c.state = LISTEN;
    0
}

/// `net_tcp_abort`: erase connection state and buffers; keep the endpoint
/// (IPv4 context, port, next ISN, profile flags) and the padding bytes.
pub fn tcp_abort(c: &mut Tcp) {
    if c.port == 0 {
        return;
    }
    c.peer = [0; 4];
    c.peer_mac = [0; 6];
    c.peer_port = 0;
    c.isn = 0;
    c.peer_isn = 0;
    c.snd_next = 0;
    c.rcv_next = 0;
    c.since = 0;
    c.state = LISTEN;
    c.rx_used = 0;
    c.tx_used = 0;
    c.peer_window = 0;
    c.window_dirty = 0;
    c.snd_una = 0;
    c.win_seq = 0;
    c.win_ack = 0;
    c.rcv_adv = 0;
    c.tx_since = 0;
    c.retry_count = 0;
    c.retry_after = 0;
    c.rx = [0; RX_MAX];
    c.tx = [0; TX_MAX];
}

/// `net_tcp_close`: erase the complete context (the C `memset`).
pub fn tcp_close(c: &mut Tcp) {
    *c = Tcp::ZERO;
}

fn fin_pending(c: &Tcp) -> bool {
    c.state == FIN_WAIT_1 || c.state == CLOSING || c.state == LAST_ACK
}
fn peer_finished(c: &Tcp) -> bool {
    c.state == CLOSE_WAIT || c.state == CLOSING || c.state == LAST_ACK || c.state == TIME_WAIT
}
/// The C `unsigned` receive window (wraps if a corrupted rx_used exceeds 64).
fn receive_window(c: &Tcp) -> u32 {
    if c.closure_enabled != 0 && peer_finished(c) { 0 } else { (RX_MAX as u32).wrapping_sub(c.rx_used as u32) }
}
fn closure_timeout(c: &Tcp, now: u64) -> bool {
    if c.state == TIME_WAIT {
        return now.wrapping_sub(c.since) >= CLOSE_HARD_TTL;
    }
    now.wrapping_sub(c.since) >= CLOSE_TTL || ((c.tx_used != 0 || fin_pending(c)) && now.wrapping_sub(c.tx_since) >= TX_TTL)
}
fn payload_timeout(c: &Tcp, now: u64) -> bool {
    now.wrapping_sub(c.since) >= IDLE_TTL || (c.tx_used != 0 && now.wrapping_sub(c.tx_since) >= TX_TTL)
}

/// `net_tcp_expire`. Returns 1 on abort expiry, DONE on completed TIME-WAIT.
pub fn tcp_expire(c: &mut Tcp, now: u64) -> i32 {
    if c.port == 0 {
        return EINVAL;
    }
    if c.closure_enabled != 0 && c.state == TIME_WAIT && now.wrapping_sub(c.tx_since) >= TIME_WAIT_TTL && c.rx_used == 0 {
        tcp_abort(c);
        return DONE;
    }
    if c.closure_enabled != 0 && c.state >= CLOSE_WAIT {
        if !closure_timeout(c, now) {
            return 0;
        }
        tcp_abort(c);
        return 1;
    }
    if !((c.state == SYN_RECEIVED && now.wrapping_sub(c.since) >= HANDSHAKE_TTL)
        || (c.state == ESTABLISHED && c.payload_enabled != 0 && payload_timeout(c, now))) {
        return 0;
    }
    tcp_abort(c);
    1
}

pub fn tcp_enable_payload(c: &mut Tcp) -> i32 {
    if c.port == 0 {
        return EINVAL;
    }
    if c.state != LISTEN {
        return BUSY;
    }
    c.payload_enabled = 1;
    0
}
pub fn tcp_enable_retransmission(c: &mut Tcp) -> i32 {
    if c.port == 0 || c.payload_enabled == 0 {
        return EINVAL;
    }
    if c.state != LISTEN {
        return BUSY;
    }
    c.retry_enabled = 1;
    0
}
pub fn tcp_enable_closure(c: &mut Tcp) -> i32 {
    if c.port == 0 || c.payload_enabled == 0 {
        return EINVAL;
    }
    if c.state != LISTEN {
        return BUSY;
    }
    c.closure_enabled = 1;
    0
}

fn ready(c: &Tcp, now: u64) -> i32 {
    if c.port == 0 || c.payload_enabled == 0 {
        return EINVAL;
    }
    if c.state == ESTABLISHED {
        return if payload_timeout(c, now) { EXPIRED } else { 0 };
    }
    if c.closure_enabled == 0 || c.state < CLOSE_WAIT || c.state > TIME_WAIT {
        return EINVAL;
    }
    if closure_timeout(c, now) { EXPIRED } else { 0 }
}

/// The C `shift`: drop `consumed` leading bytes of the first `count`, move the
/// rest down and zero the vacated tail; bytes at and after `count` are kept.
///
/// Direction and bounds: the move runs forward (destination index i below
/// source index i+consumed), so every read index is above every index already
/// written and each read sees an original byte, exactly as the C loop
/// `p[i]=p[i+consumed]`. It is an in-place byte loop, never a memmove call
/// (checked on the production archive). Valid contexts have
/// consumed <= count <= 64; for a corrupted context, where the C would run
/// past the array, every access here is bounds-checked by `get`/`get_mut`
/// and simply stops at the end of the 64-byte buffer.
fn shift(p: &mut [u8; 64], count: u32, consumed: u32) {
    let keep = count.wrapping_sub(consumed) as usize;
    let consumed = consumed as usize;
    let mut i = 0usize;
    while i < keep {
        let Some(&v) = p.get(i.wrapping_add(consumed)) else { break };
        if let Some(d) = p.get_mut(i) {
            *d = v;
        }
        i += 1;
    }
    let mut j = keep;
    while j < count as usize {
        let Some(d) = p.get_mut(j) else { break };
        *d = 0;
        j += 1;
    }
}

#[cfg(test)]
pub fn shift_for_tests(p: &mut [u8; 64], count: u32, consumed: u32) {
    shift(p, count, consumed)
}

/// Sequence, acknowledgment and window words of a generated segment.
#[derive(Clone, Copy)]
struct Words {
    seq: u32,
    ack: u32,
    window: u32,
}
/// The C `packet`: admission of the caller's output first (NOSPACE), then the
/// IPv4 envelope and a 20-byte TCP header (plus `data`) in the local frame.
/// Nothing in the context changes. Returns the wire length.
fn packet(c: &Tcp, w: Words, data: &[u8], s: &mut Sink) -> Result<usize, i32> {
    let Words { seq, ack, window } = w;
    let n = data.len();
    if n > TX_MAX {
        return Err(EINVAL); // unreachable: every caller bounds n by TX_MAX
    }
    let wire = if 54 + n < 60 { 60 } else { 54 + n };
    if !output_ok(s.out, s.capacity, s.length, wire) {
        return Err(NOSPACE);
    }
    let size = begin(&c.ip, &c.peer_mac, &c.peer, 6, 20 + n, &mut s.frame);
    let f = &mut s.frame;
    put16(&mut f[34..36], c.port);
    put16(&mut f[36..38], c.peer_port);
    put32(f, 38, seq);
    put32(f, 42, ack);
    f[46] = 0x50;
    f[47] = if n != 0 { 24 } else { 16 };
    put16(&mut f[48..50], window as u16);
    if n != 0 {
        // As the C `if (n) memcpy(...)`: no copy call for a bare ACK/FIN.
        if let Some(d) = f.get_mut(54..54 + n) {
            d.copy_from_slice(data);
        }
    }
    let sum = match f.get(34..54 + n) {
        Some(h) => transport_checksum(&c.ip.arp.ip, &c.peer, 6, h),
        None => 0,
    };
    put16(&mut f[50..52], sum);
    Ok(size)
}
/// The C `emitted`: the copy-out (done by the adapter from `s`), then the IPv4
/// identification and advertised-window bookkeeping.
fn emitted(c: &mut Tcp, size: usize, s: &mut Sink) {
    s.len = size;
    c.ip.identification = c.ip.identification.wrapping_add(1);
    c.rcv_adv = c.rcv_next.wrapping_add(receive_window(c));
    c.window_dirty = 0;
}

pub fn tcp_ack(c: &mut Tcp, now: u64, s: &mut Sink) -> i32 {
    let e = ready(c, now);
    if e != 0 {
        return e;
    }
    let size = match packet(c, Words { seq: c.snd_next, ack: c.rcv_next, window: receive_window(c) }, &[], s) {
        Ok(n) => n,
        Err(e) => return e,
    };
    emitted(c, size, s);
    ACK
}

/// `data` is None for the C `!data || n > NET_TCP_TX_MAX` cases; it may alias
/// the caller's output, which the adapter writes only after this returns.
pub fn tcp_send(c: &mut Tcp, data: Option<&[u8]>, now: u64, s: &mut Sink) -> i32 {
    let e = ready(c, now);
    if e != 0 {
        return e;
    }
    if c.state != ESTABLISHED && c.state != CLOSE_WAIT {
        return EINVAL;
    }
    let Some(data) = data else {
        return EINVAL;
    };
    let n = data.len();
    if n == 0 || n > TX_MAX {
        return EINVAL;
    }
    if c.tx_used != 0 {
        return BUSY;
    }
    if n > c.peer_window as usize {
        return WINDOW;
    }
    let size = match packet(c, Words { seq: c.snd_next, ack: c.rcv_next, window: receive_window(c) }, data, s) {
        Ok(n) => n,
        Err(e) => return e,
    };
    c.tx = [0; TX_MAX];
    if let (Some(dst), Some(src)) = (c.tx.get_mut(..n), s.frame.get(54..54 + n)) {
        dst.copy_from_slice(src);
    }
    c.tx_used = n as u16;
    c.snd_next = c.snd_next.wrapping_add(n as u32);
    c.tx_since = now;
    if c.state == ESTABLISHED {
        c.since = now;
    }
    c.retry_count = 0;
    c.retry_after = if c.retry_enabled != 0 { RETRY_TICKS } else { 0 };
    emitted(c, size, s);
    TX_DATA
}

fn fin_packet(c: &mut Tcp, seq: u32, s: &mut Sink) -> i32 {
    let size = match packet(c, Words { seq, ack: c.rcv_next, window: receive_window(c) }, &[], s) {
        Ok(n) => n,
        Err(e) => return e,
    };
    s.frame[47] = 17;
    put16(&mut s.frame[50..52], 0);
    let sum = transport_checksum(&c.ip.arp.ip, &c.peer, 6, &s.frame[34..54]);
    put16(&mut s.frame[50..52], sum);
    emitted(c, size, s);
    0
}

pub fn tcp_retransmit(c: &mut Tcp, now: u64, s: &mut Sink) -> i32 {
    if c.retry_enabled == 0 {
        return EINVAL;
    }
    let e = ready(c, now);
    if e == EXPIRED {
        tcp_abort(c);
        return e;
    }
    if e != 0 {
        return e;
    }
    if c.closure_enabled != 0 && fin_pending(c) {
        let elapsed = now.wrapping_sub(c.tx_since);
        if c.tx_used != 0 || c.snd_next.wrapping_sub(c.snd_una) != 1 {
            return EINVAL;
        }
        if elapsed < c.retry_after as u64 {
            return NOT_DUE;
        }
        if c.retry_count >= RETRY_MAX {
            tcp_abort(c);
            return EXPIRED;
        }
        if c.peer_window == 0 {
            return WINDOW; // No persist probes.
        }
        let e = fin_packet(c, c.snd_una, s);
        if e != 0 {
            return e;
        }
        c.retry_count = c.retry_count.wrapping_add(1);
        c.retry_after = (elapsed as u32).wrapping_add(RETRY_TICKS);
        return RETRANSMITTED;
    }
    if c.tx_used == 0 {
        return EMPTY;
    }
    if c.tx_used as u32 != c.snd_next.wrapping_sub(c.snd_una) {
        return EINVAL;
    }
    let elapsed = now.wrapping_sub(c.tx_since);
    if elapsed < c.retry_after as u64 {
        return NOT_DUE;
    }
    if c.retry_count >= RETRY_MAX {
        tcp_abort(c);
        return EXPIRED;
    }
    let mut n = c.tx_used as usize;
    if n > c.peer_window as usize {
        n = c.peer_window as usize;
    }
    if n == 0 {
        return WINDOW; // Persist probes are outside this profile.
    }
    let Some(data) = c.tx.get(..n) else {
        return EINVAL; // corrupted context only (tx_used > 64)
    };
    let size = match packet(c, Words { seq: c.snd_una, ack: c.rcv_next, window: receive_window(c) }, data, s) {
        Ok(n) => n,
        Err(e) => return e,
    };
    c.retry_count = c.retry_count.wrapping_add(1);
    c.retry_after = (elapsed as u32).wrapping_add(RETRY_TICKS);
    emitted(c, size, s);
    RETRANSMITTED
}

/// `net_tcp_receive`, step 1: Ok(n) bytes to copy from `c.rx` to the caller
/// (admitted), or the C return code. The adapter copies, then calls finish.
pub fn tcp_receive_begin(c: &Tcp, out: usize, capacity: usize, length: usize, now: u64) -> Result<usize, i32> {
    let e = ready(c, now);
    if e != 0 {
        return Err(e);
    }
    if capacity == 0 || out == 0 || length == 0 {
        return Err(NOSPACE);
    }
    if c.rx_used == 0 {
        return Err(if c.closure_enabled != 0 && peer_finished(c) { EOF } else { EMPTY });
    }
    let mut n = c.rx_used as usize;
    if n > capacity {
        n = capacity;
    }
    if !output_ok(out, capacity, length, n) {
        return Err(NOSPACE);
    }
    Ok(n)
}
/// `net_tcp_receive`, step 2, after the copy-out: drop the bytes read.
pub fn tcp_receive_finish(c: &mut Tcp, n: usize, now: u64) {
    shift(&mut c.rx, c.rx_used as u32, n as u32);
    c.rx_used = c.rx_used.wrapping_sub(n as u16);
    c.window_dirty = 1;
    if c.state == ESTABLISHED {
        c.since = now;
    }
}

/// Copy `n` segment bytes into the receive buffer at rx_used (the C memcpy;
/// callers have bounded n by the window, so it fits for a valid context).
fn store(c: &mut Tcp, data: &[u8]) {
    let at = c.rx_used as usize;
    if let Some(dst) = c.rx.get_mut(at..at + data.len()) {
        dst.copy_from_slice(data);
    }
    c.rx_used = c.rx_used.wrapping_add(data.len() as u16);
    c.rcv_next = c.rcv_next.wrapping_add(data.len() as u32);
}

fn payload_input(c: &mut Tcp, h: &Header, data: &[u8], now: u64, s: &mut Sink) -> i32 {
    let n = data.len();
    if payload_timeout(c, now) {
        return EXPIRED;
    }
    let seq = get32(h, 4);
    let ack = get32(h, 8);
    let delta = ack.wrapping_sub(c.snd_una);
    let outstanding = c.snd_next.wrapping_sub(c.snd_una);
    let ack_current = delta <= outstanding;
    // Older ACKs are ignored; future/ambiguous ACKs invalidate the segment.
    if !ack_current && c.snd_una.wrapping_sub(ack) >= 0x8000_0000 {
        return DROP;
    }
    let mut room = c.rcv_adv.wrapping_sub(c.rcv_next);
    let actual = (RX_MAX as u32).wrapping_sub(c.rx_used as u32);
    if room > actual {
        room = actual;
    }
    let sequence_ok = if n != 0 {
        seq == c.rcv_next && n as u64 <= room as u64
    } else if room != 0 {
        seq.wrapping_sub(c.rcv_next) < room
    } else {
        seq == c.rcv_next
    };
    if !sequence_ok {
        return tcp_ack(c, now, s);
    }
    let mut size = 0;
    if n != 0 {
        size = match packet(c, Words { seq: c.snd_next, ack: c.rcv_next.wrapping_add(n as u32), window: actual.wrapping_sub(n as u32) }, &[], s) {
            Ok(w) => w,
            Err(e) => return e,
        };
    }
    if ack_current {
        if delta != 0 {
            shift(&mut c.tx, c.tx_used as u32, delta);
            c.tx_used = c.tx_used.wrapping_sub(delta as u16);
            c.snd_una = ack;
            c.since = now;
            if c.tx_used == 0 {
                c.tx_since = 0;
                c.retry_after = 0;
                c.retry_count = 0;
            } else if c.retry_enabled != 0 {
                c.retry_after = (now.wrapping_sub(c.tx_since) as u32).wrapping_add(RETRY_TICKS);
            }
        }
        if seq.wrapping_sub(c.win_seq) < 0x8000_0000 && (seq != c.win_seq || ack.wrapping_sub(c.win_ack) < 0x8000_0000) {
            let window = get16(h, 14);
            if window != c.peer_window {
                c.since = now;
            }
            c.peer_window = window;
            c.win_seq = seq;
            c.win_ack = ack;
        }
    }
    if n == 0 {
        return ACKED;
    }
    store(c, data);
    c.since = now;
    emitted(c, size, s);
    DATA_ACK
}

fn time_wait(c: &mut Tcp, now: u64) {
    c.state = TIME_WAIT;
    c.tx_since = now;
    c.retry_after = 0;
    c.retry_count = 0;
}

pub fn tcp_finish(c: &mut Tcp, now: u64, s: &mut Sink) -> i32 {
    if c.closure_enabled == 0 {
        return EINVAL;
    }
    let e = ready(c, now);
    if e != 0 {
        return e;
    }
    if c.state != ESTABLISHED && c.state != CLOSE_WAIT {
        return BUSY;
    }
    // No queued FIN flag or extra buffer: caller retries after data is drained.
    if c.tx_used != 0 || c.rx_used != 0 {
        return BUSY;
    }
    if c.peer_window == 0 {
        return WINDOW;
    }
    let e = fin_packet(c, c.snd_next, s);
    if e != 0 {
        return e;
    }
    if c.state == ESTABLISHED {
        c.since = now;
        c.state = FIN_WAIT_1;
    } else {
        c.state = LAST_ACK;
    }
    c.snd_next = c.snd_next.wrapping_add(1);
    c.tx_since = now;
    c.retry_count = 0;
    c.retry_after = RETRY_TICKS;
    FIN_SENT
}

fn closure_input(c: &mut Tcp, h: &Header, data: &[u8], now: u64, s: &mut Sink) -> i32 {
    let n = data.len();
    if (c.state == ESTABLISHED && payload_timeout(c, now)) || (c.state >= CLOSE_WAIT && closure_timeout(c, now)) {
        return EXPIRED;
    }
    let flags = h[13];
    let seq = get32(h, 4);
    let ack = get32(h, 8);
    let mut room = receive_window(c);
    let advertised = c.rcv_adv.wrapping_sub(c.rcv_next);
    if advertised < room {
        room = advertised;
    }
    if flags == 4 || flags == 20 {
        if n != 0 || c.state == TIME_WAIT {
            return DROP;
        }
        if seq == c.rcv_next {
            tcp_abort(c);
            return RESET;
        }
        if room != 0 && seq.wrapping_sub(c.rcv_next) < room {
            return tcp_ack(c, now, s);
        }
        return DROP;
    }
    if flags != 16 && flags != 24 && flags != 17 && flags != 25 {
        return DROP;
    }
    let delta = ack.wrapping_sub(c.snd_una);
    let outstanding = c.snd_next.wrapping_sub(c.snd_una);
    let current = delta <= outstanding;
    if !current && c.snd_una.wrapping_sub(ack) >= 0x8000_0000 {
        return DROP;
    }
    let mut fin = flags & 1 != 0;
    let received = peer_finished(c);
    let duplicate_fin = fin && received && seq.wrapping_add(n as u32) == c.rcv_next.wrapping_sub(1);
    let sequence_ok = if duplicate_fin {
        true
    } else if n != 0 || fin {
        !received && seq == c.rcv_next && n as u64 <= room as u64 && (!fin || n != 0 || room != 0)
    } else if room != 0 {
        seq.wrapping_sub(c.rcv_next) < room
    } else {
        seq == c.rcv_next
    };
    if !sequence_ok {
        return tcp_ack(c, now, s);
    }
    // A FIN just beyond the advertised right edge is not consumed. Its data
    // can fill the real buffer; the peer retries FIN after a window update.
    if fin && !duplicate_fin && n as u64 == room as u64 {
        fin = false;
    }
    // Prepare any required response before committing ACK/data/FIN changes.
    let mut size = 0;
    let advance = if duplicate_fin { 0 } else { (n as u32).wrapping_add(fin as u32) };
    if n != 0 || fin {
        let window = if fin || received { 0 } else { (RX_MAX as u32).wrapping_sub(c.rx_used as u32).wrapping_sub(n as u32) };
        size = match packet(c, Words { seq: c.snd_next, ack: c.rcv_next.wrapping_add(advance), window: window }, &[], s) {
            Ok(w) => w,
            Err(e) => return e,
        };
    }
    if current {
        if delta != 0 {
            if fin_pending(c) {
                // One FIN occupies one sequence number, never TX buffer bytes.
                if delta != 1 || outstanding != 1 || c.tx_used != 0 {
                    return EINVAL;
                }
                c.snd_una = ack;
                c.retry_after = 0;
                c.retry_count = 0;
                if c.state == LAST_ACK {
                    tcp_abort(c);
                    return DONE;
                }
                if c.state == CLOSING {
                    time_wait(c, now);
                } else {
                    c.state = FIN_WAIT_2;
                    c.tx_since = 0;
                }
            } else {
                if delta > c.tx_used as u32 {
                    return EINVAL;
                }
                shift(&mut c.tx, c.tx_used as u32, delta);
                c.tx_used = c.tx_used.wrapping_sub(delta as u16);
                c.snd_una = ack;
                if c.state == ESTABLISHED {
                    c.since = now;
                }
                if c.tx_used == 0 {
                    c.tx_since = 0;
                    c.retry_after = 0;
                    c.retry_count = 0;
                } else if c.retry_enabled != 0 {
                    c.retry_after = (now.wrapping_sub(c.tx_since) as u32).wrapping_add(RETRY_TICKS);
                }
            }
        }
        if seq.wrapping_sub(c.win_seq) < 0x8000_0000 && (seq != c.win_seq || ack.wrapping_sub(c.win_ack) < 0x8000_0000) {
            c.peer_window = get16(h, 14);
            c.win_seq = seq;
            c.win_ack = ack;
        }
    }
    if duplicate_fin {
        if c.state == TIME_WAIT {
            c.tx_since = now;
        }
        emitted(c, size, s);
        return ACK;
    }
    if n == 0 && !fin {
        return ACKED;
    }
    if n != 0 {
        store(c, data);
    }
    if fin {
        c.rcv_next = c.rcv_next.wrapping_add(1);
        if c.state == ESTABLISHED {
            c.state = CLOSE_WAIT;
            c.since = now;
        } else if c.state == FIN_WAIT_1 {
            c.state = CLOSING;
        } else if c.state == FIN_WAIT_2 {
            time_wait(c, now);
        }
    } else if c.state == ESTABLISHED {
        c.since = now;
    }
    emitted(c, size, s);
    if fin { FIN_ACK } else { DATA_ACK }
}

/// Result of `tcp_input`: a code, or the C delegation of non-TCP local frames
/// to `net_ipv4_input` (done by the adapter, as for UDP).
pub enum TcpIn {
    Code(i32),
    Ipv4Input,
}

/// `net_tcp_input`. `p` is the caller's admitted frame (14..=1514 bytes).
pub fn tcp_input(c: &mut Tcp, p: &[u8], now: u64, s: &mut Sink) -> TcpIn {
    if c.port == 0 {
        return TcpIn::Code(EINVAL);
    }
    let view = match payload(&c.ip, p, 6) {
        Ok(v) => v,
        Err(UNSUPPORTED) => return TcpIn::Ipv4Input,
        Err(e) => return TcpIn::Code(e),
    };
    TcpIn::Code(segment(c, p, view.source, view.bytes, view.length, now, s))
}

fn segment(c: &mut Tcp, p: &[u8], source_at: usize, bytes: usize, length: usize, now: u64, s: &mut Sink) -> i32 {
    let (Some(tcp), Some(src), Some(mac)) = (p.get(bytes..bytes + length), p.get(source_at..source_at + 4), p.get(6..12)) else {
        return EINVAL;
    };
    let (Some(h), Some(data)) = (tcp.first_chunk::<20>(), tcp.get(20..)) else {
        return EINVAL; // view.length < 20
    };
    let source = [src[0], src[1], src[2], src[3]];
    let peer_mac = [mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]];
    let header = (h[12] >> 4) as usize * 4;
    if header < 20 || header > length || h[12] & 15 != 0 || get16(h, 0) == 0 || get16(h, 2) == 0
        || transport_checksum(&source, &c.ip.arp.ip, 6, tcp) != 0 {
        return EINVAL;
    }
    if header != 20 || get16(h, 18) != 0 {
        return UNSUPPORTED;
    }
    let data_length = data.len();
    if data_length != 0 && (c.payload_enabled == 0 || (c.state != ESTABLISHED && !(c.closure_enabled != 0 && c.state >= CLOSE_WAIT))) {
        return UNSUPPORTED;
    }
    if get16(h, 2) != c.port {
        return DROP;
    }
    let flags = h[13];
    let seq = get32(h, 4);
    let ack = get32(h, 8);
    let peer_port = get16(h, 0);
    if c.closure_enabled != 0 && c.state >= SYN_RECEIVED {
        if peer_port != c.peer_port || source != c.peer || peer_mac != c.peer_mac {
            return DROP;
        }
        if c.state == SYN_RECEIVED && (flags == 4 || flags == 20) {
            if now.wrapping_sub(c.since) >= HANDSHAKE_TTL {
                return EXPIRED;
            }
            if data_length != 0 || seq != c.rcv_next {
                return DROP;
            }
            tcp_abort(c);
            return RESET;
        }
        if c.state >= ESTABLISHED {
            return closure_input(c, h, data, now, s);
        }
    }
    if flags != 2 && flags != 16 && !(flags == 24 && c.payload_enabled != 0 && c.state == ESTABLISHED) {
        return UNSUPPORTED;
    }
    if c.state == ESTABLISHED && c.payload_enabled != 0 {
        if (flags != 16 && flags != 24) || peer_port != c.peer_port || source != c.peer || peer_mac != c.peer_mac {
            return DROP;
        }
        return payload_input(c, h, data, now, s);
    }
    if c.state == LISTEN {
        if flags != 2 || ack != 0 {
            return DROP;
        }
        let size = begin(&c.ip, &peer_mac, &source, 6, 20, &mut s.frame);
        // Validate caller capacity before changing any connection or output.
        if !output_ok(s.out, s.capacity, s.length, size) {
            return NOSPACE;
        }
        let f = &mut s.frame;
        put16(&mut f[34..36], c.port);
        put16(&mut f[36..38], peer_port);
        put32(f, 38, c.next_isn);
        put32(f, 42, seq.wrapping_add(1));
        f[46] = 0x50;
        f[47] = 18;
        // Legacy profile keeps zero window; opt-in payload capacity is real.
        put16(&mut f[48..50], if c.payload_enabled != 0 { RX_MAX as u16 } else { 0 });
        let sum = transport_checksum(&c.ip.arp.ip, &source, 6, &f[34..54]);
        put16(&mut f[50..52], sum);
        c.peer = source;
        c.peer_mac = peer_mac;
        c.peer_port = peer_port;
        c.isn = c.next_isn;
        c.next_isn = c.next_isn.wrapping_add(0x10000);
        c.peer_isn = seq;
        c.snd_next = c.isn.wrapping_add(1);
        c.rcv_next = seq.wrapping_add(1);
        c.since = now;
        c.state = SYN_RECEIVED;
        c.ip.identification = c.ip.identification.wrapping_add(1);
        if c.payload_enabled != 0 {
            c.snd_una = c.snd_next;
            c.rcv_adv = c.rcv_next.wrapping_add(RX_MAX as u32);
        }
        s.len = size;
        return SYNACK;
    }
    if c.state == SYN_RECEIVED && now.wrapping_sub(c.since) >= HANDSHAKE_TTL {
        return EXPIRED;
    }
    if c.state != SYN_RECEIVED || flags != 16 || peer_port != c.peer_port || source != c.peer || peer_mac != c.peer_mac
        || seq != c.rcv_next || ack != c.snd_next {
        return DROP;
    }
    c.state = ESTABLISHED;
    if c.payload_enabled != 0 {
        c.peer_window = get16(h, 14);
        c.win_seq = seq;
        c.win_ack = ack;
        c.since = now;
    }
    CONNECTED
}
