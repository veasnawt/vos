//! R3-N1 stage 2: Ethernet-II and local IPv4 validation and the IPv4 envelope
//! builder, exactly as the accepted C `net_ethernet_type`, `net_ipv4_payload`
//! and `net_ipv4_begin`. No state is modified.
#![forbid(unsafe_code)]
use super::checksum::{be16, checksum, ip_unicast, mac_unicast, put16};
use super::context::{Arp, Ipv4, DROP, EINVAL, UNSUPPORTED, FRAME_MAX};

const BROADCAST: [u8; 6] = [255; 6];

/// `frame` is the caller's frame (length already admitted as 14..=1514).
/// Ok(type) for a local/broadcast Ethernet-II frame; Err(code) otherwise.
pub fn ethernet_type(a: &Arp, p: &[u8]) -> Result<u16, i32> {
    // Repeats the adapter's admission so every index below is provably in bounds.
    let Some(p) = p.get(..14) else {
        return Err(EINVAL);
    };
    if p[..6] != a.mac && p[..6] != BROADCAST {
        return Err(DROP);
    }
    if !mac_unicast(&p[6..12]) || p[6..12] == a.mac {
        return Err(EINVAL);
    }
    let kind = be16(&p[12..14]);
    if kind <= 1500 {
        return Err(UNSUPPORTED); // IEEE 802.3 length/LLC, not Ethernet-II.
    }
    if kind < 0x0600 {
        return Err(EINVAL);
    }
    Ok(kind)
}

/// Frame-length admission shared by every frame entry (the C `length<14 ||
/// length>VNET_FRAME_MAX` test).
pub fn frame_length_ok(length: usize) -> bool {
    (14..=FRAME_MAX).contains(&length)
}

/// Offsets into the caller's frame for a validated local IPv4 payload.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Payload {
    pub source: usize,
    pub bytes: usize,
    pub length: usize,
}

pub fn payload(c: &Ipv4, p: &[u8], protocol: u8) -> Result<Payload, i32> {
    let kind = ethernet_type(&c.arp, p)?;
    if kind != 0x0800 {
        return Err(UNSUPPORTED);
    }
    if p[..6] != c.arp.mac {
        return Err(DROP); // No broadcast echo replies.
    }
    let Some(ip) = p.get(14..) else {
        return Err(EINVAL);
    };
    let available = ip.len();
    if available < 20 {
        return Err(EINVAL);
    }
    let header = (ip[0] & 15) as usize * 4;
    if ip[0] >> 4 != 4 || header < 20 || header > available {
        return Err(EINVAL);
    }
    let Some(head) = ip.get(..header) else {
        return Err(EINVAL);
    };
    let total = be16(&ip[2..4]) as usize;
    if total < header || total > available || total > 1500 || ip[8] == 0 || checksum(head) != 0 {
        return Err(EINVAL);
    }
    let fragments = be16(&ip[6..8]);
    if fragments & 0x8000 != 0 {
        return Err(EINVAL);
    }
    if header != 20 || fragments & 0x3fff != 0 || ip[9] != protocol {
        return Err(UNSUPPORTED);
    }
    if ip[16..20] != c.arp.ip {
        return Err(DROP);
    }
    if !ip_unicast(&ip[12..16]) || ip[12..16] == c.arp.ip {
        return Err(EINVAL);
    }
    Ok(Payload { source: 14 + 12, bytes: 14 + header, length: total - header })
}

/// Bounded Ethernet/IPv4 envelope with a zeroed payload and padding. The caller
/// has admitted `n <= 1480` and `out.len() >= wire_length(n)`; `mac`/`ip` are
/// copies. Returns the wire length; bytes beyond it are untouched.
pub fn begin(c: &Ipv4, mac: &[u8; 6], ip: &[u8; 4], protocol: u8, n: usize, out: &mut [u8]) -> usize {
    let total = 20 + n;
    let mut wire = 14 + total;
    if wire < 60 {
        wire = 60;
    }
    // Every caller supplies at least `wire_length(n)` bytes (the C local or the
    // admitted output); an impossible short buffer produces nothing.
    let Some(o) = out.get_mut(..wire) else {
        return 0;
    };
    o.fill(0);
    o[..6].copy_from_slice(mac);
    o[6..12].copy_from_slice(&c.arp.mac);
    put16(&mut o[12..14], 0x0800);
    let h = &mut o[14..34];
    h[0] = 0x45;
    put16(&mut h[2..4], total as u16);
    put16(&mut h[4..6], c.identification);
    put16(&mut h[6..8], 0x4000);
    h[8] = 64;
    h[9] = protocol;
    h[12..16].copy_from_slice(&c.arp.ip);
    h[16..20].copy_from_slice(ip);
    let sum = checksum(h);
    put16(&mut h[10..12], sum);
    wire
}

/// Wire length of an envelope for an `n`-byte payload (minimum frame 60).
pub fn wire_length(n: usize) -> usize {
    let w = 14 + 20 + n;
    if w < 60 { 60 } else { w }
}

