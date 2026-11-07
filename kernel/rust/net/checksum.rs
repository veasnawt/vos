//! R3-N1 stage 1: Internet checksums and stateless address predicates, exactly
//! as the accepted C (`net_ipv4.c`, `net_arp.c`). Unsigned arithmetic wraps as
//! in C; the caller bounds lengths as documented in `net_ipv4.h`.
#![forbid(unsafe_code)]
// Intermediate R3-N1 stage: items used by the remaining stages are not yet referenced.
#![allow(dead_code)]

/// Folded RFC 1071 one's-complement sum of big-endian words, odd byte padded
/// high (the C loop and fold, before the final inversion).
#[inline(always)]
fn folded_sum(bytes: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    let mut rest = bytes;
    while let [hi, lo, tail @ ..] = rest {
        sum = sum.wrapping_add((*hi as u32) * 256 + *lo as u32);
        rest = tail;
    }
    if let [last] = rest {
        sum = sum.wrapping_add((*last as u32) << 8);
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum as u16
}

#[inline(always)]
fn fold4(a: &[u8; 4]) -> u16 {
    let s = ((a[0] as u32) << 8 | a[1] as u32) + ((a[2] as u32) << 8 | a[3] as u32);
    ((s & 0xffff) + (s >> 16)) as u16
}

/// RFC 1071 Internet checksum: the inverted folded sum.
#[inline(always)]
pub fn checksum(bytes: &[u8]) -> u16 {
    !folded_sum(bytes)
}

/// IPv4 pseudoheader plus transport bytes. The length term is the C
/// `(uint32_t)n` truncation. Does not apply UDP's zero mapping.
#[inline(always)]
pub fn transport_checksum(source: &[u8; 4], dest: &[u8; 4], protocol: u8, bytes: &[u8]) -> u16 {
    // The C `(uint16_t)~net_checksum(x)` is exactly the folded sum of x; for a
    // 4-byte address that is fold(word0 + word1).
    let mut sum = (fold4(source) as u32)
        .wrapping_add(fold4(dest) as u32)
        .wrapping_add(protocol as u32)
        .wrapping_add(bytes.len() as u32)
        .wrapping_add(folded_sum(bytes) as u32);
    while sum >> 16 != 0 {
        sum = (sum & 65535) + (sum >> 16);
    }
    !(sum as u16)
}

/// Unicast IPv4 address accepted by the bounded profile (not 0/8, 127/8 or class D/E).
pub fn ip_unicast(ip: &[u8]) -> bool {
    ip[0] != 0 && ip[0] != 127 && ip[0] < 224
}

/// Unicast, nonzero Ethernet address.
pub fn mac_unicast(mac: &[u8]) -> bool {
    mac[0] & 1 == 0 && mac[..6] != [0u8; 6]
}

pub fn be16(p: &[u8]) -> u16 {
    (p[0] as u16) * 256 + p[1] as u16
}

pub fn put16(p: &mut [u8], n: u16) {
    p[0] = (n >> 8) as u8;
    p[1] = n as u8;
}

/// The accepted C output admission: non-null output and length pointers, enough
/// capacity, no address wrap, and the length object disjoint from the packet
/// bytes. Pure address arithmetic; nothing is dereferenced.
pub fn output_ok(out: usize, capacity: usize, length: usize, needed: usize) -> bool {
    const LEN: usize = core::mem::size_of::<usize>();
    out != 0
        && length != 0
        && capacity >= needed
        && out.wrapping_add(needed) > out
        && length.wrapping_add(LEN) > length
        && !(out < length.wrapping_add(LEN) && length < out.wrapping_add(needed))
}
