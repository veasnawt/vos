//! R3-N1 protocol contexts: exact `#[repr(C)]` mirrors of `net_arp.h`,
//! `net_ipv4.h` and `net_udp.h`. Every byte, padding included, is an explicit
//! field, so a whole-context reset or copy defines the same bytes as the C
//! `memset`/`memcpy` and field stores never touch padding.
#![forbid(unsafe_code)]
use core::mem::{align_of, offset_of, size_of};

pub const NEIGHBORS: usize = 4;
pub const ARP_TTL: u64 = 6000;
pub const ARP_QUERY_TTL: u64 = 200;
pub const ARP_WIRE: usize = 60;
pub const PING_DATA_MAX: usize = 64;
pub const PING_TTL: u64 = 200;
pub const UDP_DATA_MAX: usize = 512;
pub const UDP_QUEUE: u32 = 4;
pub const FRAME_MAX: usize = 1514;

pub const DROP: i32 = 0;
pub const REPLY: i32 = 1;
pub const LEARNED: i32 = 2;
pub const REQUEST: i32 = 3;
pub const CACHED: i32 = 4;
pub const EINVAL: i32 = -1;
pub const UNSUPPORTED: i32 = -2;
pub const NOSPACE: i32 = -3;
pub const BUSY: i32 = -4;
pub const ECHO_REQUEST: i32 = 5;
pub const ECHO_MATCH: i32 = 6;
pub const NEIGHBOR: i32 = -5;
pub const UDP_QUEUED: i32 = 7;
pub const UDP_SENT: i32 = 8;
pub const UDP_FULL: i32 = -6;
pub const UDP_EMPTY: i32 = -7;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Neighbor {
    pub ip: [u8; 4],
    pub mac: [u8; 6],
    pub pad0: [u8; 6],
    pub seen: u64,
    pub valid: i32,
    pub pad1: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Arp {
    pub mac: [u8; 6],
    pub ip: [u8; 4],
    pub pending_ip: [u8; 4],
    pub pad0: [u8; 2],
    pub neighbors: [Neighbor; NEIGHBORS],
    pub pending_since: u64,
    pub pending: i32,
    pub pad1: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Ipv4 {
    pub arp: Arp,
    pub identification: u16,
    pub id: u16,
    pub sequence: u16,
    pub data_length: u16,
    pub peer: [u8; 4],
    pub peer_mac: [u8; 6],
    pub data: [u8; PING_DATA_MAX],
    pub pad0: [u8; 6],
    pub since: u64,
    pub sent: u64,
    pub matched: u64,
    pub answered: u64,
    pub pending: i32,
    pub pad1: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Datagram {
    pub source: [u8; 4],
    pub port: u16,
    pub length: u16,
    pub checksum_present: i32,
    pub data: [u8; UDP_DATA_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Udp {
    pub ip: Ipv4,
    pub port: u16,
    pub pad0: [u8; 2],
    pub head: u32,
    pub count: u32,
    pub queue: [Datagram; UDP_QUEUE as usize],
    pub pad1: [u8; 4],
}

const _: () = {
    assert!(size_of::<Neighbor>() == 32 && align_of::<Neighbor>() == 8);
    assert!(offset_of!(Neighbor, mac) == 4 && offset_of!(Neighbor, seen) == 16 && offset_of!(Neighbor, valid) == 24);
    assert!(size_of::<Arp>() == 160 && align_of::<Arp>() == 8);
    assert!(offset_of!(Arp, ip) == 6 && offset_of!(Arp, pending_ip) == 10 && offset_of!(Arp, neighbors) == 16);
    assert!(offset_of!(Arp, pending_since) == 144 && offset_of!(Arp, pending) == 152);
    assert!(size_of::<Ipv4>() == 288 && offset_of!(Ipv4, identification) == 160 && offset_of!(Ipv4, id) == 162);
    assert!(offset_of!(Ipv4, sequence) == 164 && offset_of!(Ipv4, data_length) == 166 && offset_of!(Ipv4, peer) == 168);
    assert!(offset_of!(Ipv4, peer_mac) == 172 && offset_of!(Ipv4, data) == 178 && offset_of!(Ipv4, since) == 248);
    assert!(offset_of!(Ipv4, sent) == 256 && offset_of!(Ipv4, matched) == 264 && offset_of!(Ipv4, answered) == 272);
    assert!(offset_of!(Ipv4, pending) == 280);
    assert!(size_of::<Datagram>() == 524 && align_of::<Datagram>() == 4 && offset_of!(Datagram, port) == 4);
    assert!(offset_of!(Datagram, length) == 6 && offset_of!(Datagram, checksum_present) == 8 && offset_of!(Datagram, data) == 12);
    assert!(size_of::<Udp>() == 2400 && offset_of!(Udp, port) == 288 && offset_of!(Udp, head) == 292);
    assert!(offset_of!(Udp, count) == 296 && offset_of!(Udp, queue) == 300);
};

impl Neighbor {
    pub const ZERO: Neighbor = Neighbor { ip: [0; 4], mac: [0; 6], pad0: [0; 6], seen: 0, valid: 0, pad1: [0; 4] };
}
impl Arp {
    pub const ZERO: Arp = Arp { mac: [0; 6], ip: [0; 4], pending_ip: [0; 4], pad0: [0; 2], neighbors: [Neighbor::ZERO; NEIGHBORS], pending_since: 0, pending: 0, pad1: [0; 4] };
}
impl Ipv4 {
    pub const ZERO: Ipv4 = Ipv4 { arp: Arp::ZERO, identification: 0, id: 0, sequence: 0, data_length: 0, peer: [0; 4], peer_mac: [0; 6],
        data: [0; PING_DATA_MAX], pad0: [0; 6], since: 0, sent: 0, matched: 0, answered: 0, pending: 0, pad1: [0; 4] };
}
impl Datagram {
    pub const ZERO: Datagram = Datagram { source: [0; 4], port: 0, length: 0, checksum_present: 0, data: [0; UDP_DATA_MAX] };
}
impl Udp {
    pub const ZERO: Udp = Udp { ip: Ipv4::ZERO, port: 0, pad0: [0; 2], head: 0, count: 0, queue: [Datagram::ZERO; UDP_QUEUE as usize], pad1: [0; 4] };
}

/// Local packet buffer sizes, equal to the accepted C locals.
pub const ECHO_WIRE_MAX: usize = 42 + PING_DATA_MAX;
pub const UDP_WIRE_MAX: usize = 42 + UDP_DATA_MAX;
