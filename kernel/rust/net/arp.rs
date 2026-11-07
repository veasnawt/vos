//! R3-N1 stage 3: ARP parsing and construction, the bounded neighbor table, and
//! the ICMP echo state that resolves through it, exactly as the accepted C
//! (`net_arp.c`, `net_ipv4.c`). Invalid input never changes state; generated
//! frames are returned to the adapter, which copies them out last.
#![forbid(unsafe_code)]
use super::checksum::{be16, checksum, ip_unicast, mac_unicast, put16};
use super::context::*;
use super::ipv4::{begin, ethernet_type, payload, wire_length};

const BROADCAST: [u8; 6] = [255; 6];
const ZERO6: [u8; 6] = [0; 6];
const ZERO4: [u8; 4] = [0; 4];

/// `memset` then mac/ip, after validation (inputs are copies).
pub fn arp_init(a: &mut Arp, mac: &[u8; 6], ip: &[u8; 4]) -> i32 {
    if !mac_unicast(mac) || !ip_unicast(ip) {
        return EINVAL;
    }
    *a = Arp::ZERO;
    a.mac = *mac;
    a.ip = *ip;
    0
}

fn expire(a: &mut Arp, now: u64) {
    for n in a.neighbors.iter_mut() {
        if n.valid != 0 && now.wrapping_sub(n.seen) >= ARP_TTL {
            n.valid = 0;
        }
    }
    if a.pending != 0 && now.wrapping_sub(a.pending_since) >= ARP_QUERY_TTL {
        a.pending = 0;
    }
}

/// Lazy expiry, then a cached lookup. Expiry happens even on a miss.
pub fn neighbor_get(a: &mut Arp, ip: &[u8; 4], now: u64) -> Option<[u8; 6]> {
    expire(a, now);
    a.neighbors.iter().find(|n| n.valid != 0 && n.ip == *ip).map(|n| n.mac)
}

fn learn(a: &mut Arp, ip: &[u8; 4], mac: &[u8; 6], now: u64) {
    expire(a, now);
    let mut slot = 0;
    let mut found = false;
    for (i, n) in a.neighbors.iter().enumerate() {
        if n.valid != 0 && n.ip == *ip {
            slot = i;
            found = true;
            break;
        }
    }
    if !found {
        let mut oldest = 0u64;
        for (i, n) in a.neighbors.iter().enumerate() {
            if n.valid == 0 {
                slot = i;
                break;
            }
            let age = now.wrapping_sub(n.seen);
            if age >= oldest {
                oldest = age;
                slot = i;
            }
        }
    }
    let n = &mut a.neighbors[slot];
    n.ip = *ip;
    n.mac = *mac;
    n.seen = now;
    n.valid = 1;
    if a.pending != 0 && a.pending_ip == *ip {
        a.pending = 0;
    }
}

fn packet(out: &mut [u8], a: &Arp, op: u16, dest: &[u8; 6], target_mac: &[u8; 6], target_ip: &[u8; 4]) {
    let o = &mut out[..ARP_WIRE];
    o.fill(0);
    o[..6].copy_from_slice(dest);
    o[6..12].copy_from_slice(&a.mac);
    put16(&mut o[12..14], 0x0806);
    put16(&mut o[14..16], 1);
    put16(&mut o[16..18], 0x0800);
    o[18] = 6;
    o[19] = 4;
    put16(&mut o[20..22], op);
    o[22..28].copy_from_slice(&a.mac);
    o[28..32].copy_from_slice(&a.ip);
    o[32..38].copy_from_slice(target_mac);
    o[38..42].copy_from_slice(target_ip);
}

/// `ip` is a copy. `output_ok` is the adapter's admission of the caller buffer.
/// On REQUEST, `out[..60]` holds the frame.
pub fn arp_query(a: &mut Arp, ip: &[u8; 4], now: u64, output_ok: bool, out: &mut [u8; ARP_WIRE]) -> i32 {
    if !ip_unicast(ip) || *ip == a.ip {
        return EINVAL;
    }
    if !output_ok {
        return NOSPACE;
    }
    if neighbor_get(a, ip, now).is_some() {
        return CACHED;
    }
    if a.pending != 0 {
        return BUSY;
    }
    packet(out, a, 1, &BROADCAST, &ZERO6, ip);
    a.pending_ip = *ip;
    a.pending_since = now;
    a.pending = 1;
    REQUEST
}

/// Untagged Ethernet-II dispatch; only Ethernet/IPv4 ARP is handled here.
/// `output_ok` is evaluated by the adapter (pure address arithmetic).
/// On REPLY, `out[..60]` holds the frame.
pub fn ethernet_input(a: &mut Arp, p: &[u8], now: u64, output_ok: bool, out: &mut [u8]) -> i32 {
    let kind = match ethernet_type(a, p) {
        Ok(k) => k,
        Err(e) => return e,
    };
    if kind != 0x0806 {
        return UNSUPPORTED;
    }
    if p.len() < 42 {
        return EINVAL;
    }
    if be16(&p[14..16]) != 1 || be16(&p[16..18]) != 0x0800 {
        return UNSUPPORTED;
    }
    let op = be16(&p[20..22]);
    if p[18] != 6 || p[19] != 4 || (op != 1 && op != 2) {
        return EINVAL;
    }
    let sha: [u8; 6] = p[22..28].try_into().unwrap_or(ZERO6);
    let spa: [u8; 4] = p[28..32].try_into().unwrap_or(ZERO4);
    let tha = &p[32..38];
    let tpa = &p[38..42];
    let probe = spa == ZERO4;
    if sha[..] != p[6..12] || (!ip_unicast(&spa) && !(probe && op == 1)) || spa == a.ip {
        return EINVAL;
    }
    if tpa != a.ip {
        return DROP;
    }
    if op == 2 {
        if p[..6] != a.mac || tha != a.mac || a.pending == 0 || now.wrapping_sub(a.pending_since) >= ARP_QUERY_TTL || spa != a.pending_ip {
            return DROP;
        }
        learn(a, &spa, &sha, now);
        return LEARNED;
    }
    if !output_ok {
        return NOSPACE;
    }
    // Build before writing output/state, including when frame and out alias.
    packet(out, a, 2, &sha, &sha, &spa);
    if !probe {
        learn(a, &spa, &sha, now);
    }
    REPLY
}

/// `net_ipv4_init`: validate through a scratch ARP context, then reset.
pub fn ipv4_init(c: &mut Ipv4, mac: &[u8; 6], ip: &[u8; 4]) -> i32 {
    let mut arp = Arp::ZERO;
    let e = arp_init(&mut arp, mac, ip);
    if e != 0 {
        return e;
    }
    *c = Ipv4::ZERO;
    c.arp = arp;
    c.identification = 1;
    0
}

#[inline(always)]
fn build(out: &mut [u8], c: &Ipv4, mac: &[u8; 6], ip: &[u8; 4], kind: u8, id: u16, seq: u16, data: &[u8]) -> usize {
    let wire = begin(c, mac, ip, 1, 8 + data.len(), out);
    let Some(icmp) = out.get_mut(34..42 + data.len()) else {
        return 0;
    };
    icmp[0] = kind;
    put16(&mut icmp[4..6], id);
    put16(&mut icmp[6..8], seq);
    icmp[8..].copy_from_slice(data);
    let sum = checksum(icmp);
    put16(&mut icmp[2..4], sum);
    wire
}

/// Echo origination. `data` is a copy-independent view (may alias the output);
/// the correlation bytes are captured from the constructed packet.
/// On ECHO_REQUEST, `out[..wire_length(8 + data.len())]` holds the frame.
pub fn echo_request(c: &mut Ipv4, peer: &[u8; 4], id: u16, seq: u16, data: &[u8], now: u64, output_ok: bool, out: &mut [u8; ECHO_WIRE_MAX]) -> i32 {
    if !ip_unicast(peer) || *peer == c.arp.ip || data.len() > PING_DATA_MAX {
        return EINVAL;
    }
    if !output_ok {
        return NOSPACE;
    }
    if c.pending != 0 && now.wrapping_sub(c.since) < PING_TTL {
        return BUSY;
    }
    let Some(mac) = neighbor_get(&mut c.arp, peer, now) else {
        return NEIGHBOR;
    };
    build(out, c, &mac, peer, 8, id, seq, data);
    let n = data.len();
    if let (Some(dst), Some(src)) = (c.data.get_mut(..n), out.get(42..42 + n)) {
        dst.copy_from_slice(src);
    }
    c.peer = *peer;
    c.peer_mac = mac;
    c.id = id;
    c.sequence = seq;
    c.data_length = n as u16;
    c.since = now;
    c.pending = 1;
    c.sent = c.sent.wrapping_add(1);
    c.identification = c.identification.wrapping_add(1);
    ECHO_REQUEST
}

/// Dispatch ARP or local IPv4/ICMP. `admit(needed)` is the adapter's output
/// admission for a reply of `needed` bytes (pure address arithmetic). On REPLY,
/// `*produced` bytes of `out` hold the frame.
pub fn ipv4_input(c: &mut Ipv4, p: &[u8], now: u64, admit: &dyn Fn(usize) -> bool, out: &mut [u8; FRAME_MAX], produced: &mut usize) -> i32 {
    let kind = match ethernet_type(&c.arp, p) {
        Ok(k) => k,
        Err(e) => return e,
    };
    if kind == 0x0806 {
        let e = ethernet_input(&mut c.arp, p, now, admit(ARP_WIRE), &mut out[..]);
        if e == REPLY {
            *produced = ARP_WIRE;
        }
        return e;
    }
    let view = match payload(c, p, 1) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let (Some(icmp), Some(head)) = (p.get(view.bytes..view.bytes + view.length), p.get(..34)) else {
        return EINVAL;
    };
    let icmp_length = view.length;
    if icmp_length < 8 || icmp[1] != 0 || checksum(icmp) != 0 {
        return EINVAL;
    }
    if icmp[0] != 0 && icmp[0] != 8 {
        return UNSUPPORTED;
    }
    let data_length = icmp_length - 8;
    let source = &head[26..30];
    if icmp[0] == 0 {
        if c.pending == 0 || now.wrapping_sub(c.since) >= PING_TTL || source != c.peer || head[6..12] != c.peer_mac
            || be16(&icmp[4..6]) != c.id || be16(&icmp[6..8]) != c.sequence || data_length != c.data_length as usize
            || (data_length != 0 && c.data.get(..data_length) != icmp.get(8..)) {
            return DROP;
        }
        c.pending = 0;
        c.matched = c.matched.wrapping_add(1);
        return ECHO_MATCH;
    }
    if !admit(wire_length(icmp_length)) {
        return NOSPACE;
    }
    let mac: [u8; 6] = head[6..12].try_into().unwrap_or(ZERO6);
    let peer: [u8; 4] = head[26..30].try_into().unwrap_or(ZERO4);
    *produced = build(&mut out[..], c, &mac, &peer, 0, be16(&icmp[4..6]), be16(&icmp[6..8]), &icmp[8..]);
    c.answered = c.answered.wrapping_add(1);
    c.identification = c.identification.wrapping_add(1);
    REPLY
}
