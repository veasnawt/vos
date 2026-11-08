//! R3-N1/R3-N2 FFI boundary for the Rust ARP/IPv4/UDP/TCP core. C keeps the
//! public ABI in net_arp.c/net_ipv4.c/net_udp.c/net_tcp.c and forwards here;
//! virtio-net, DMA and boot glue stay C. Contract (net_arp.h): inputs may alias each other and the
//! packet output, but not the context; the length output is separate from the
//! packet output. Inputs that the C copies first are copied first; generated
//! frames are built in locals sized like the C locals and copied out last.
//! Rust-only refusals (no access, no state change) cover cases the accepted C
//! contract leaves undefined: a misaligned context, or a frame/data range that
//! overlaps a context the call mutates or borrows.

// ---------------- stage 1: checksums ----------------
#[path = "../../../kernel/rust/net/checksum.rs"]
pub mod checksum;
#[path = "../../../kernel/rust/net/context.rs"]
pub mod context;
use core::slice;

/// # Safety
/// `bytes` is readable for `length` bytes (caller bounds length <= 1500).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_checksum(bytes: *const u8, length: usize) -> u16 {
    if length == 0 {
        return checksum::checksum(&[]);
    }
    if bytes.is_null() {
        return 0xffff; // contract violation: fail verification safely
    }
    checksum::checksum(unsafe { slice::from_raw_parts(bytes, length) })
}
/// # Safety
/// `source`/`dest` are 4 readable bytes; `bytes` is readable for `n` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_transport_checksum(source: *const u8, dest: *const u8, protocol: u8, bytes: *const u8, n: usize) -> u16 {
    if source.is_null() || dest.is_null() || (bytes.is_null() && n != 0) {
        return 0xffff;
    }
    // SAFETY: pure reads of caller-owned 4-byte addresses and n payload bytes.
    let (s, d) = unsafe { (&*source.cast::<[u8; 4]>(), &*dest.cast::<[u8; 4]>()) };
    let b: &[u8] = if n == 0 { &[] } else { unsafe { slice::from_raw_parts(bytes, n) } };
    checksum::transport_checksum(s, d, protocol, b)
}

// ---------------- stage 2: Ethernet-II / IPv4 validation and envelope ----------------
#[path = "../../../kernel/rust/net/ipv4.rs"]
pub mod ipv4;
use context::*;
use core::{mem::{align_of, size_of}, ptr};

#[repr(C)]
pub struct PayloadView {
    source: *const u8,
    bytes: *const u8,
    length: usize,
}

fn aligned<T>(p: *const T) -> bool {
    !p.is_null() && (p as usize) % align_of::<T>() == 0 && (p as usize).checked_add(size_of::<T>()).is_some()
}
/// True when [a, a+an) and [b, b+bn) intersect (or either range wraps).
fn overlaps(a: usize, an: usize, b: usize, bn: usize) -> bool {
    match (a.checked_add(an), b.checked_add(bn)) {
        (Some(ae), Some(be)) => an != 0 && bn != 0 && a < be && b < ae,
        _ => true,
    }
}
// SAFETY (all helpers below): callers pass pointers the C contract makes valid
// for the stated length; nothing is retained after the call.
unsafe fn copy4(p: *const u8) -> [u8; 4] {
    let mut a = [0u8; 4];
    unsafe { ptr::copy_nonoverlapping(p, a.as_mut_ptr(), 4) };
    a
}
unsafe fn copy6(p: *const u8) -> [u8; 6] {
    let mut a = [0u8; 6];
    unsafe { ptr::copy_nonoverlapping(p, a.as_mut_ptr(), 6) };
    a
}
/// The admitted Ethernet frame view, or None for the C EINVAL cases (null frame,
/// length outside 14..=1514).
unsafe fn frame<'a>(p: *const u8, n: usize) -> Option<&'a [u8]> {
    if p.is_null() || !ipv4::frame_length_ok(n) { None } else { Some(unsafe { slice::from_raw_parts(p, n) }) }
}
fn admit(out: *mut u8, capacity: usize, length: *mut usize, needed: usize) -> bool {
    checksum::output_ok(out as usize, capacity, length as usize, needed)
}

/// # Safety
/// `a` is a live `struct net_arp`; frame readable for `length`; type writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_ethernet_type(a: *const Arp, f: *const u8, length: usize, kind: *mut u16) -> i32 {
    if !aligned(a) || kind.is_null() {
        return EINVAL;
    }
    let Some(p) = (unsafe { frame(f, length) }) else { return EINVAL };
    match ipv4::ethernet_type(unsafe { &*a }, p) {
        Ok(k) => {
            unsafe { kind.write_unaligned(k) };
            1
        }
        Err(e) => e,
    }
}
/// # Safety
/// `c` is a live `struct net_ipv4`; frame readable for `length`; view writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_ipv4_payload(c: *const Ipv4, f: *const u8, length: usize, protocol: u8, view: *mut PayloadView) -> i32 {
    if !aligned(c) || view.is_null() {
        return EINVAL;
    }
    let Some(p) = (unsafe { frame(f, length) }) else { return EINVAL };
    match ipv4::payload(unsafe { &*c }, p, protocol) {
        Ok(v) => {
            let base = p.as_ptr();
            unsafe { view.write_unaligned(PayloadView { source: base.wrapping_add(v.source), bytes: base.wrapping_add(v.bytes), length: v.length }) };
            1
        }
        Err(e) => e,
    }
}
/// # Safety
/// `c` is a live `struct net_ipv4`; mac/ip readable; buffer/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_ipv4_begin(c: *const Ipv4, mac: *const u8, ip: *const u8, protocol: u8, n: usize,
                                                 buffer: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) || mac.is_null() || ip.is_null() || n > 1480 {
        return EINVAL;
    }
    let wire = ipv4::wire_length(n);
    if !admit(buffer, capacity, length, wire) {
        return NOSPACE;
    }
    if overlaps(buffer as usize, wire, c as usize, size_of::<Ipv4>()) {
        return EINVAL;
    }
    let (m, t) = unsafe { (copy6(mac), copy4(ip)) };
    // SAFETY: admitted output, disjoint from the context; mac/ip already copied.
    let out = unsafe { slice::from_raw_parts_mut(buffer, wire) };
    let w = ipv4::begin(unsafe { &*c }, &m, &t, protocol, n, out);
    unsafe { length.write_unaligned(w) };
    0
}

// ---------------- stage 3: ARP, neighbor table, ICMP echo ----------------
#[path = "../../../kernel/rust/net/arp.rs"]
pub mod arp;

unsafe fn emit(local: &[u8], out: *mut u8, length: *mut usize) {
    // SAFETY: output_ok() admitted [out, out+len) and the separate length object.
    unsafe {
        ptr::copy_nonoverlapping(local.as_ptr(), out, local.len());
        length.write_unaligned(local.len());
    }
}
/// # Safety
/// `a` is writable `struct net_arp` storage; mac/ip readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_arp_init(a: *mut Arp, mac: *const u8, ip: *const u8) -> i32 {
    if !aligned(a) || mac.is_null() || ip.is_null() {
        return EINVAL;
    }
    let (m, i) = unsafe { (copy6(mac), copy4(ip)) };
    arp::arp_init(unsafe { &mut *a }, &m, &i)
}
/// # Safety
/// `a` is a live `struct net_arp`; ip readable; mac writable (6 bytes).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_neighbor_get(a: *mut Arp, ip: *const u8, now: u64, mac: *mut u8) -> i32 {
    if !aligned(a) || ip.is_null() || mac.is_null() {
        return 0;
    }
    let i = unsafe { copy4(ip) };
    match arp::neighbor_get(unsafe { &mut *a }, &i, now) {
        Some(m) => {
            unsafe { ptr::copy_nonoverlapping(m.as_ptr(), mac, 6) };
            1
        }
        None => 0,
    }
}
/// # Safety
/// `a` is a live `struct net_arp`; ip readable; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_arp_query(a: *mut Arp, ip: *const u8, now: u64, out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(a) || ip.is_null() {
        return EINVAL;
    }
    let i = unsafe { copy4(ip) };
    let mut local = [0u8; ARP_WIRE];
    let e = arp::arp_query(unsafe { &mut *a }, &i, now, admit(out, capacity, length, ARP_WIRE), &mut local);
    if e == REQUEST {
        unsafe { emit(&local, out, length) };
    }
    e
}
/// # Safety
/// `a` is a live `struct net_arp`; frame readable for `length`; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_ethernet_input(a: *mut Arp, f: *const u8, length: usize, now: u64, out: *mut u8, capacity: usize, out_length: *mut usize) -> i32 {
    if !aligned(a) {
        return EINVAL;
    }
    let Some(p) = (unsafe { frame(f, length) }) else { return EINVAL };
    if overlaps(f as usize, length, a as usize, size_of::<Arp>()) {
        return EINVAL;
    }
    let mut local = [0u8; ARP_WIRE];
    let e = arp::ethernet_input(unsafe { &mut *a }, p, now, admit(out, capacity, out_length, ARP_WIRE), &mut local);
    if e == REPLY {
        unsafe { emit(&local, out, out_length) };
    }
    e
}
/// # Safety
/// `c` is writable `struct net_ipv4` storage; mac/ip readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_ipv4_init(c: *mut Ipv4, mac: *const u8, ip: *const u8) -> i32 {
    if !aligned(c) || mac.is_null() || ip.is_null() {
        return EINVAL;
    }
    let (m, i) = unsafe { (copy6(mac), copy4(ip)) };
    arp::ipv4_init(unsafe { &mut *c }, &m, &i)
}
/// # Safety
/// `c` is a live `struct net_ipv4`; peer readable; data readable for `n`; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_ipv4_echo_request(c: *mut Ipv4, peer: *const u8, id: u16, seq: u16, data: *const u8, n: usize, now: u64,
                                                        out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) || peer.is_null() || n > PING_DATA_MAX || (data.is_null() && n != 0) {
        return EINVAL;
    }
    if overlaps(data as usize, n, c as usize, size_of::<Ipv4>()) {
        return EINVAL;
    }
    let p = unsafe { copy4(peer) };
    let d: &[u8] = if n == 0 { &[] } else { unsafe { slice::from_raw_parts(data, n) } };
    let mut local = [0u8; ECHO_WIRE_MAX];
    let e = arp::echo_request(unsafe { &mut *c }, &p, id, seq, d, now, admit(out, capacity, length, ipv4::wire_length(8 + n)), &mut local);
    if e == ECHO_REQUEST {
        unsafe { emit(&local[..ipv4::wire_length(8 + n)], out, length) };
    }
    e
}
/// # Safety
/// `c` is a live `struct net_ipv4`; frame readable for `length`; out/length per net_arp.h.
#[unsafe(no_mangle)]
#[inline(never)]
pub unsafe extern "C" fn vos_rust_net_ipv4_input(c: *mut Ipv4, f: *const u8, length: usize, now: u64, out: *mut u8, capacity: usize, out_length: *mut usize) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    let Some(p) = (unsafe { frame(f, length) }) else { return EINVAL };
    if overlaps(f as usize, length, c as usize, size_of::<Ipv4>()) {
        return EINVAL;
    }
    let mut local = [0u8; FRAME_MAX];
    let mut produced = 0;
    let gate = |needed: usize| admit(out, capacity, out_length, needed);
    let e = arp::ipv4_input(unsafe { &mut *c }, p, now, &gate, &mut local, &mut produced);
    if e == REPLY {
        if let Some(frame) = local.get(..produced) {
            unsafe { emit(frame, out, out_length) };
        }
    }
    e
}

// ---------------- stage 4: UDP endpoint ----------------
#[path = "../../../kernel/rust/net/udp.rs"]
pub mod udp;

/// # Safety
/// `c` is writable `struct net_udp` storage; mac/ip readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_udp_init(c: *mut Udp, mac: *const u8, ip: *const u8, port: u16) -> i32 {
    if !aligned(c) || port == 0 || mac.is_null() || ip.is_null() {
        return EINVAL;
    }
    let (m, i) = unsafe { (copy6(mac), copy4(ip)) };
    udp::udp_init(unsafe { &mut *c }, &m, &i, port)
}
/// # Safety
/// `c` is a live `struct net_udp`; peer readable; data readable for `n`; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_udp_send(c: *mut Udp, peer: *const u8, port: u16, data: *const u8, n: usize, now: u64,
                                               out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) || peer.is_null() || n > UDP_DATA_MAX || (data.is_null() && n != 0) {
        return EINVAL;
    }
    if overlaps(data as usize, n, c as usize, size_of::<Udp>()) {
        return EINVAL;
    }
    let p = unsafe { copy4(peer) };
    let d: &[u8] = if n == 0 { &[] } else { unsafe { slice::from_raw_parts(data, n) } };
    let wire = ipv4::wire_length(8 + n);
    let mut local = [0u8; UDP_WIRE_MAX];
    let mut produced = 0;
    let e = udp::udp_send(unsafe { &mut *c }, &p, port, d, now, admit(out, capacity, length, wire), &mut local, &mut produced);
    if e == UDP_SENT {
        if let Some(frame) = local.get(..produced) {
            unsafe { emit(frame, out, length) };
        }
    }
    e
}
/// # Safety
/// `c` is a live `struct net_udp`; frame readable for `n`; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_udp_input(c: *mut Udp, f: *const u8, n: usize, now: u64, out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    let Some(p) = (unsafe { frame(f, n) }) else { return EINVAL };
    if overlaps(f as usize, n, c as usize, size_of::<Udp>()) {
        return EINVAL;
    }
    match udp::udp_input(unsafe { &mut *c }, p) {
        udp::UdpIn::Code(e) => e,
        // As the accepted C, non-UDP local frames go to the IPv4/ARP input.
        udp::UdpIn::Ipv4Input => unsafe { vos_rust_net_ipv4_input(ptr::addr_of_mut!((*c).ip), f, n, now, out, capacity, length) },
    }
}
/// # Safety
/// `c` is a live `struct net_udp`; out is a writable `struct net_udp_datagram` outside it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_udp_receive(c: *mut Udp, out: *mut Datagram) -> i32 {
    if !aligned(c) || out.is_null() {
        return EINVAL;
    }
    // Rust-only refusal: the slot copy must not overlap the context it reads and clears.
    if overlaps(out as usize, size_of::<Datagram>(), c as usize, size_of::<Udp>()) {
        return EINVAL;
    }
    match udp::udp_receive_slot(unsafe { &*c }) {
        Ok(slot) => {
            // SAFETY: out is a writable datagram object outside the context
            // (contract); the copy precedes the clear, as the accepted C.
            unsafe { ptr::copy_nonoverlapping(ptr::addr_of!((*c).queue[slot]).cast::<u8>(), out.cast::<u8>(), size_of::<Datagram>()) };
            udp::udp_receive_finish(unsafe { &mut *c });
            0
        }
        Err(e) => e,
    }
}
/// # Safety
/// `c` is null or writable `struct net_udp` storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_udp_close(c: *mut Udp) {
    if aligned(c) {
        udp::udp_close(unsafe { &mut *c });
    }
}

// ---------------- R3-N2: bounded passive TCP ----------------
// Same contract as above (net_tcp.h: "same buffer/alias contract as ARP").
// Init addresses are copied before the context is borrowed, so they may alias
// it, as in the C. Segments are built in a local Sink and copied out last.
// Rust-only refusals (no access, no state change) cover inputs outside the
// contract: a misaligned context, and a frame, send data or receive output
// overlapping the context.
#[path = "../../../kernel/rust/net/tcp.rs"]
pub mod tcp;
use tcp::{Sink, Tcp, TcpIn};

unsafe fn sink_out(s: &Sink, out: *mut u8, length: *mut usize) {
    // SAFETY: a nonzero s.len is set only after output_ok() admitted
    // [out, out+s.len) and the separate length object; the local frame is
    // disjoint from the caller's output.
    if let Some(frame) = s.frame.get(..s.len) {
        if !frame.is_empty() {
            unsafe { emit(frame, out, length) };
        }
    }
}
/// # Safety
/// `c` is writable `struct net_tcp` storage; mac/ip readable (may alias `c`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_init(c: *mut Tcp, mac: *const u8, ip: *const u8, port: u16, initial_sequence: u32) -> i32 {
    if !aligned(c) || port == 0 || mac.is_null() || ip.is_null() {
        return EINVAL;
    }
    let (m, i) = unsafe { (copy6(mac), copy4(ip)) };
    tcp::tcp_init(unsafe { &mut *c }, &m, &i, port, initial_sequence)
}
/// # Safety
/// `c` is a live `struct net_tcp`; frame readable for `n`; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_input(c: *mut Tcp, f: *const u8, n: usize, now: u64, out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    let Some(p) = (unsafe { frame(f, n) }) else { return EINVAL };
    if overlaps(f as usize, n, c as usize, size_of::<Tcp>()) {
        return EINVAL;
    }
    let mut s = Sink::new(out as usize, capacity, length as usize);
    match tcp::tcp_input(unsafe { &mut *c }, p, now, &mut s) {
        TcpIn::Code(e) => {
            unsafe { sink_out(&s, out, length) };
            e
        }
        // As the accepted C, non-TCP local frames go to the IPv4/ARP input.
        TcpIn::Ipv4Input => unsafe { vos_rust_net_ipv4_input(ptr::addr_of_mut!((*c).ip), f, n, now, out, capacity, length) },
    }
}
/// # Safety
/// `c` is a live `struct net_tcp`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_expire(c: *mut Tcp, now: u64) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    tcp::tcp_expire(unsafe { &mut *c }, now)
}
/// # Safety
/// `c` is null or writable `struct net_tcp` storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_close(c: *mut Tcp) {
    if aligned(c) {
        tcp::tcp_close(unsafe { &mut *c });
    }
}
/// # Safety
/// `c` is null or a live `struct net_tcp`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_abort(c: *mut Tcp) {
    if aligned(c) {
        tcp::tcp_abort(unsafe { &mut *c });
    }
}
/// # Safety
/// `c` is a live `struct net_tcp`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_enable_payload(c: *mut Tcp) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    tcp::tcp_enable_payload(unsafe { &mut *c })
}
/// # Safety
/// `c` is a live `struct net_tcp`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_enable_retransmission(c: *mut Tcp) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    tcp::tcp_enable_retransmission(unsafe { &mut *c })
}
/// # Safety
/// `c` is a live `struct net_tcp`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_enable_closure(c: *mut Tcp) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    tcp::tcp_enable_closure(unsafe { &mut *c })
}
/// # Safety
/// `c` is a live `struct net_tcp`; data readable for `n` outside the context
/// (it may alias the output); out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_send(c: *mut Tcp, data: *const u8, n: usize, now: u64, out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    // Rust-only refusal: data inside the context it mutates (outside the contract).
    if !data.is_null() && n <= tcp::TX_MAX && overlaps(data as usize, n, c as usize, size_of::<Tcp>()) {
        return EINVAL;
    }
    // SAFETY: n <= 64 bytes readable at data (contract), outside the context;
    // it may alias the output, which is written only after the core returns.
    let d: Option<&[u8]> = if data.is_null() || n > tcp::TX_MAX { None } else if n == 0 { Some(&[]) } else { Some(unsafe { slice::from_raw_parts(data, n) }) };
    let mut s = Sink::new(out as usize, capacity, length as usize);
    let e = tcp::tcp_send(unsafe { &mut *c }, d, now, &mut s);
    unsafe { sink_out(&s, out, length) };
    e
}

/// # Safety
/// `c` is a live `struct net_tcp`; out writable for capacity outside it; length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_receive(c: *mut Tcp, out: *mut u8, capacity: usize, length: *mut usize, now: u64) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    // Rust-only refusal: the copy must not overlap the context it reads and shifts.
    let span = if capacity < tcp::RX_MAX { capacity } else { tcp::RX_MAX };
    if !out.is_null() && overlaps(out as usize, span, c as usize, size_of::<Tcp>()) {
        return EINVAL;
    }
    match tcp::tcp_receive_begin(unsafe { &*c }, out as usize, capacity, length as usize, now) {
        Ok(n) => {
            // SAFETY: output_ok() admitted [out, out+n) and the separate length
            // object; out is outside the context (checked above); n <= rx_used <= 64.
            unsafe { ptr::copy_nonoverlapping(ptr::addr_of!((*c).rx).cast::<u8>(), out, n) };
            tcp::tcp_receive_finish(unsafe { &mut *c }, n, now);
            unsafe { length.write_unaligned(n) };
            0
        }
        Err(e) => e,
    }
}
/// # Safety
/// `c` is a live `struct net_tcp`; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_ack(c: *mut Tcp, now: u64, out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    let mut s = Sink::new(out as usize, capacity, length as usize);
    let e = tcp::tcp_ack(unsafe { &mut *c }, now, &mut s);
    unsafe { sink_out(&s, out, length) };
    e
}
/// # Safety
/// `c` is a live `struct net_tcp`; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_retransmit(c: *mut Tcp, now: u64, out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    let mut s = Sink::new(out as usize, capacity, length as usize);
    let e = tcp::tcp_retransmit(unsafe { &mut *c }, now, &mut s);
    unsafe { sink_out(&s, out, length) };
    e
}
/// # Safety
/// `c` is a live `struct net_tcp`; out/length per net_arp.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_tcp_finish(c: *mut Tcp, now: u64, out: *mut u8, capacity: usize, length: *mut usize) -> i32 {
    if !aligned(c) {
        return EINVAL;
    }
    let mut s = Sink::new(out as usize, capacity, length as usize);
    let e = tcp::tcp_finish(unsafe { &mut *c }, now, &mut s);
    unsafe { sink_out(&s, out, length) };
    e
}

// ---------------- R3-N3: virtio-net validation decisions ----------------
// Scalars only: the C driver keeps PCI, register
// windows, virtqueue/DMA memory, volatile ring reads, fences, reset,
// notification and ownership, and passes values it has already read.
#[path = "../../../kernel/rust/net/vnet.rs"]
pub mod vnet;

#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_features_ok(offered: u64) -> i32 {
    vnet::features_ok(offered) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_queue_size_ok(size: u32) -> i32 {
    vnet::queue_size_ok(size) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_queues_ok(count: u32) -> i32 {
    vnet::queues_ok(count) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_used_count_ok(count: u32, tx: i32) -> i32 {
    vnet::used_count_ok(count, tx != 0) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_used_entry(seen: u32, id: u32, len: u32, tx: i32) -> u32 {
    vnet::used_entry(seen, id, len, tx != 0)
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_header_ok(flags: u8, gso: u8) -> i32 {
    vnet::header_ok(flags, gso) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_notify_ok(offset: u64, length: u32) -> i32 {
    vnet::notify_ok(offset, length) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_send_length_ok(length: u32) -> i32 {
    vnet::send_length_ok(length) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_tx_wire(length: u32) -> u32 {
    vnet::tx_wire(length)
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_capacity_ok(capacity: u32) -> i32 {
    vnet::capacity_ok(capacity) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vnet_rx_outputs_overlap(frame: u64, capacity: u32, length: u64) -> i32 {
    vnet::rx_outputs_overlap(frame, capacity, length) as i32
}
