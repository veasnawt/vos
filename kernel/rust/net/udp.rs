//! R3-N1 stage 4: one bounded UDP endpoint (validation, send, the 4-slot receive
//! queue and close), exactly as the accepted C `net_udp.c`. Invalid, oversized
//! and queue-full input never modifies state or output.
#![forbid(unsafe_code)]
use super::arp::{ipv4_init, neighbor_get};
use super::checksum::{be16, put16, transport_checksum};
use super::context::*;
use super::ipv4::{begin, payload};

pub fn udp_init(c: &mut Udp, mac: &[u8; 6], ip: &[u8; 4], port: u16) -> i32 {
    if port == 0 {
        return EINVAL;
    }
    let mut ipctx = Ipv4::ZERO;
    let e = ipv4_init(&mut ipctx, mac, ip);
    if e != 0 {
        return e;
    }
    *c = Udp::ZERO;
    c.ip = ipctx;
    c.port = port;
    0
}

/// `peer` is a copy; `data` may alias the caller's output, which the adapter
/// writes last. On UDP_SENT, `*produced` bytes of `out` hold the frame.
pub fn udp_send(c: &mut Udp, peer: &[u8; 4], port: u16, data: &[u8], now: u64, output_ok: bool,
                out: &mut [u8; UDP_WIRE_MAX], produced: &mut usize) -> i32 {
    if c.port == 0 || port == 0 || peer[0] == 0 || peer[0] == 127 || peer[0] >= 224 || *peer == c.ip.arp.ip
        || data.len() > UDP_DATA_MAX {
        return EINVAL;
    }
    if !output_ok {
        return NOSPACE;
    }
    let Some(mac) = neighbor_get(&mut c.ip.arp, peer, now) else {
        return NEIGHBOR;
    };
    let n = data.len();
    *produced = begin(&c.ip, &mac, peer, 17, 8 + n, &mut out[..]);
    let local = c.ip.arp.ip;
    let Some(udp) = out.get_mut(34..42 + n) else {
        return EINVAL;
    };
    put16(&mut udp[0..2], c.port);
    put16(&mut udp[2..4], port);
    put16(&mut udp[4..6], (8 + n) as u16);
    if let Some(body) = udp.get_mut(8..) {
        body.copy_from_slice(data);
    }
    let sum = transport_checksum(&local, peer, 17, udp);
    put16(&mut udp[6..8], if sum != 0 { sum } else { 65535 });
    c.ip.identification = c.ip.identification.wrapping_add(1);
    UDP_SENT
}

/// Result of UDP input: a return code, or delegation of a non-UDP local frame to
/// the IPv4/ARP input entry (as the accepted C calls `net_ipv4_input`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UdpIn {
    Code(i32),
    Ipv4Input,
}

pub fn udp_input(c: &mut Udp, p: &[u8]) -> UdpIn {
    if c.port == 0 {
        return UdpIn::Code(EINVAL);
    }
    let view = match payload(&c.ip, p, 17) {
        Ok(v) => v,
        Err(UNSUPPORTED) => return UdpIn::Ipv4Input,
        Err(e) => return UdpIn::Code(e),
    };
    let (Some(udp), Some(src)) = (p.get(view.bytes..view.bytes + view.length), p.get(view.source..view.source + 4)) else {
        return UdpIn::Code(EINVAL);
    };
    if view.length < 8 {
        return UdpIn::Code(EINVAL);
    }
    let size = be16(&udp[4..6]) as usize;
    // This bounded profile has no UDP options or IP payload trailer.
    if size < 8 || size != view.length || be16(&udp[2..4]) == 0 {
        return UdpIn::Code(EINVAL);
    }
    if be16(&udp[2..4]) != c.port {
        return UdpIn::Code(DROP);
    }
    let present = be16(&udp[6..8]) != 0;
    let source = [src[0], src[1], src[2], src[3]];
    let Some(datagram) = udp.get(..size) else {
        return UdpIn::Code(EINVAL);
    };
    if present && transport_checksum(&source, &c.ip.arp.ip, 17, datagram) != 0 {
        return UdpIn::Code(EINVAL);
    }
    if size - 8 > UDP_DATA_MAX {
        return UdpIn::Code(NOSPACE);
    }
    if c.count == UDP_QUEUE {
        return UdpIn::Code(UDP_FULL);
    }
    let slot = (c.head.wrapping_add(c.count) % UDP_QUEUE) as usize;
    let d = &mut c.queue[slot];
    *d = Datagram::ZERO;
    d.source = source;
    d.port = be16(&udp[0..2]);
    d.length = (size - 8) as u16;
    d.checksum_present = present as i32;
    if let (Some(dst), Some(body)) = (d.data.get_mut(..size - 8), datagram.get(8..)) {
        dst.copy_from_slice(body);
    }
    c.count = c.count.wrapping_add(1);
    UdpIn::Code(UDP_QUEUED)
}

/// Receive, step 1: Ok(slot) of the head datagram, which the adapter copies to
/// the caller's object exactly as the C `memcpy`. The accepted C indexes
/// `queue[head]` directly; a valid context always has `head < 4`, and the modulo
/// keeps a corrupted context in bounds instead of reading past the queue.
pub fn udp_receive_slot(c: &Udp) -> Result<usize, i32> {
    if c.port == 0 {
        return Err(EINVAL);
    }
    if c.count == 0 {
        return Err(UDP_EMPTY);
    }
    Ok((c.head % UDP_QUEUE) as usize)
}

/// Receive, step 2 (after the copy): advance the queue and clear the copied
/// head slot. The accepted C clears before advancing; with one owner, IF=0 and
/// no callback in between, the order of these stores is unobservable and the
/// resulting context bytes are identical.
pub fn udp_receive_finish(c: &mut Udp) {
    let slot = (c.head % UDP_QUEUE) as usize;
    c.head = c.head.wrapping_add(1) % UDP_QUEUE;
    c.count = c.count.wrapping_sub(1);
    c.queue[slot] = Datagram::ZERO;
}

pub fn udp_close(c: &mut Udp) {
    *c = Udp::ZERO;
}
