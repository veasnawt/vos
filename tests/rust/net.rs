extern crate net_core;
use net_core::arp::{arp_init, arp_query, echo_request, ethernet_input, ipv4_init, ipv4_input, neighbor_get};
use net_core::checksum::{checksum, ip_unicast, mac_unicast, output_ok, transport_checksum};
use net_core::context::*;
use net_core::ipv4::{begin, ethernet_type, payload, wire_length, Payload};
use net_core::udp::{udp_close, udp_init, udp_input, udp_receive_finish, udp_receive_slot, udp_send, UdpIn};

const G: [u8; 6] = [0x52, 0x54, 0, 0x12, 0x34, 0x56];
const P: [u8; 6] = [0x52, 0x54, 0, 0xaa, 0xbb, 0xcc];
const GI: [u8; 4] = [10, 0, 2, 15];
const PI: [u8; 4] = [10, 0, 2, 2];
const BC: [u8; 6] = [255; 6];
fn put(p: &mut [u8], x: u16) { p[0] = (x >> 8) as u8; p[1] = x as u8; }
fn get(p: &[u8]) -> u16 { (p[0] as u16) << 8 | p[1] as u16 }
fn reference(p: &[u8]) -> u16 {
    let mut s: u32 = 0;
    for (i, b) in p.iter().enumerate() { s += (*b as u32) << if i & 1 == 1 { 0 } else { 8 }; s = (s & 65535) + (s >> 16); }
    !(s as u16)
}
fn arp_frame(op: u16, dst: [u8; 6], sha: [u8; 6], spa: [u8; 4], tha: [u8; 6], tpa: [u8; 4]) -> Vec<u8> {
    let mut f = vec![0u8; 60];
    f[..6].copy_from_slice(&dst); f[6..12].copy_from_slice(&sha); put(&mut f[12..], 0x0806); put(&mut f[14..], 1); put(&mut f[16..], 0x0800);
    f[18] = 6; f[19] = 4; put(&mut f[20..], op); f[22..28].copy_from_slice(&sha); f[28..32].copy_from_slice(&spa);
    f[32..38].copy_from_slice(&tha); f[38..42].copy_from_slice(&tpa); f
}
fn fix_ip(f: &mut [u8]) { put(&mut f[24..], 0); let h = (f[14] & 15) as usize * 4; let s = reference(&f[14..14 + h]); put(&mut f[24..], s); }
fn ip_frame(proto: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = vec![0u8; (34 + payload.len()).max(60)];
    f[..6].copy_from_slice(&G); f[6..12].copy_from_slice(&P); put(&mut f[12..], 0x0800);
    f[14] = 0x45; put(&mut f[16..], (20 + payload.len()) as u16); put(&mut f[18..], 0x1122); put(&mut f[20..], 0x4000); f[22] = 64; f[23] = proto;
    f[26..30].copy_from_slice(&PI); f[30..34].copy_from_slice(&GI); f[34..34 + payload.len()].copy_from_slice(payload); fix_ip(&mut f); f
}
fn icmp(kind: u8, id: u16, seq: u16, data: &[u8]) -> Vec<u8> {
    let mut b = vec![kind, 0, 0, 0, 0, 0, 0, 0]; put(&mut b[4..], id); put(&mut b[6..], seq); b.extend_from_slice(data);
    let s = reference(&b); put(&mut b[2..], s); ip_frame(1, &b)
}
fn udp_sum(src: &[u8; 4], dst: &[u8; 4], u: &[u8]) -> u16 {
    let mut b = Vec::new(); b.extend_from_slice(src); b.extend_from_slice(dst); b.extend_from_slice(&[0, 17]); b.extend_from_slice(&(u.len() as u16).to_be_bytes()); b.extend_from_slice(u); reference(&b)
}
fn udp_frame(dport: u16, data: &[u8], checked: bool) -> Vec<u8> {
    let mut u = vec![0u8; 8]; put(&mut u[0..], 5000); put(&mut u[2..], dport); put(&mut u[4..], (8 + data.len()) as u16); u.extend_from_slice(data);
    if checked { let s = udp_sum(&PI, &GI, &u); put(&mut u[6..], if s == 0 { 65535 } else { s }); }
    ip_frame(17, &u)
}
fn arp_ctx() -> Arp { let mut a = Arp::ZERO; assert_eq!(arp_init(&mut a, &G, &GI), 0); a }
fn ip_ctx() -> Ipv4 { let mut c = Ipv4::ZERO; assert_eq!(ipv4_init(&mut c, &G, &GI), 0); c }
fn learned(c: &mut Arp, ip: [u8; 4], mac: [u8; 6], now: u64) {
    c.neighbors[0] = Neighbor { ip, mac, pad0: [0; 6], seen: now, valid: 1, pad1: [0; 4] };
}
fn udp_ctx() -> Udp { let mut c = Udp::ZERO; assert_eq!(udp_init(&mut c, &G, &GI, 4000), 0); learned(&mut c.ip.arp, PI, P, 100); c }
fn yes(_: usize) -> bool { true }

// ---------------- stage 1 ----------------
#[test] fn checksum_rfc1071_vector() { assert_eq!(checksum(&[0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7]), !0xddf2); }
#[test] fn checksum_odd_byte_padded_high() { assert_eq!(checksum(&[0x12]), !0x1200); assert_eq!(checksum(&[0x12, 0x34, 0x56]), reference(&[0x12, 0x34, 0x56])); }
#[test] fn checksum_empty_is_ffff() { assert_eq!(checksum(&[]), 0xffff); }
#[test] fn checksum_end_around_carry() { let b = [0xffu8; 10]; assert_eq!(checksum(&b), reference(&b)); assert_eq!(checksum(&[0xff, 0xff, 0x00, 0x01]), reference(&[0xff, 0xff, 0x00, 0x01])); }
#[test] fn checksum_all_short_inputs() { for v in 0..=0xffffu32 { let b = [(v >> 8) as u8, v as u8]; assert_eq!(checksum(&b), reference(&b)); } }
#[test] fn transport_pseudoheader() {
    let u = [0x13, 0x88, 0x0f, 0xa0, 0x00, 0x0b, 0x00, 0x00, 1, 2, 3];
    assert_eq!(transport_checksum(&PI, &GI, 17, &u), udp_sum(&PI, &GI, &u));
    assert_ne!(transport_checksum(&PI, &GI, 6, &u), transport_checksum(&PI, &GI, 17, &u));
}
#[test] fn transport_address_carry() {
    let u = [0x13, 0x88, 0x0f, 0xa0, 0x00, 0x09, 0x00, 0x00, 0xfe];
    let (s, d) = ([0xff, 0xff, 0xff, 0xfe], [0x80, 0x01, 0x80, 0x02]);
    assert_eq!(transport_checksum(&s, &d, 17, &u), udp_sum(&s, &d, &u));
}
#[test] fn ip_unicast_bounds() { for (a, ok) in [(0u8, false), (1, true), (126, true), (127, false), (128, true), (223, true), (224, false), (255, false)] { assert_eq!(ip_unicast(&[a, 1, 2, 3]), ok, "{a}"); } }
#[test] fn mac_unicast_rules() { assert!(mac_unicast(&G)); assert!(!mac_unicast(&[1, 0, 0, 0, 0, 0])); assert!(!mac_unicast(&[0; 6])); assert!(mac_unicast(&[0, 0, 0, 0, 0, 1])); }
#[test] fn output_admission() {
    assert!(output_ok(0x1000, 60, 0x2000, 60)); assert!(!output_ok(0, 60, 0x2000, 60)); assert!(!output_ok(0x1000, 60, 0, 60));
    assert!(!output_ok(0x1000, 59, 0x2000, 60)); assert!(!output_ok(usize::MAX - 10, 60, 0x2000, 60)); assert!(!output_ok(0x1000, 60, usize::MAX - 3, 60));
    assert!(!output_ok(0x1000, 60, 0x1000 + 59, 60)); assert!(!output_ok(0x1000, 60, 0x1000 - 7, 60)); assert!(output_ok(0x1000, 60, 0x1000 - 8, 60)); assert!(output_ok(0x1000, 60, 0x1000 + 60, 60));
}
// ---------------- stage 2 ----------------
#[test] fn ethernet_destinations() {
    let a = arp_ctx(); let mut f = ip_frame(17, &[0; 26]);
    assert_eq!(ethernet_type(&a, &f), Ok(0x0800)); f[..6].copy_from_slice(&BC); assert_eq!(ethernet_type(&a, &f), Ok(0x0800));
    f[..6].copy_from_slice(&P); assert_eq!(ethernet_type(&a, &f), Err(DROP));
}
#[test] fn ethernet_sources() {
    let a = arp_ctx(); let mut f = ip_frame(17, &[0; 26]);
    f[6] = 1; assert_eq!(ethernet_type(&a, &f), Err(EINVAL)); f[6..12].copy_from_slice(&[0; 6]); assert_eq!(ethernet_type(&a, &f), Err(EINVAL));
    f[6..12].copy_from_slice(&G); assert_eq!(ethernet_type(&a, &f), Err(EINVAL));
}
#[test] fn ethernet_type_classes() {
    let a = arp_ctx(); let mut f = ip_frame(17, &[0; 26]);
    for (t, r) in [(0u16, Err(UNSUPPORTED)), (1500, Err(UNSUPPORTED)), (1501, Err(EINVAL)), (0x05ff, Err(EINVAL)), (0x0600, Ok(0x0600)), (0xffff, Ok(0xffff))] { put(&mut f[12..], t); assert_eq!(ethernet_type(&a, &f), r, "{t:#x}"); }
}
#[test] fn ethernet_short_frame() { let a = arp_ctx(); assert_eq!(ethernet_type(&a, &[0u8; 13]), Err(EINVAL)); }
#[test] fn payload_valid_offsets() { let c = ip_ctx(); let f = udp_frame(4000, b"abc", true); assert_eq!(payload(&c, &f, 17), Ok(Payload { source: 26, bytes: 34, length: 11 })); }
#[test] fn payload_version_and_ihl() {
    let c = ip_ctx(); let base = udp_frame(4000, b"abc", true);
    let mut f = base.clone(); f[14] = 0x65; fix_ip(&mut f); assert_eq!(payload(&c, &f, 17), Err(EINVAL));
    let mut f = base.clone(); f[14] = 0x44; f[24] = 0; f[25] = 0; let s = reference(&f[14..30]); put(&mut f[24..], s); assert_eq!(payload(&c, &f, 17), Err(EINVAL));
    let mut f = base.clone(); f[14] = 0x46; fix_ip(&mut f); assert_eq!(payload(&c, &f, 17), Err(UNSUPPORTED));
    let mut f = base; f[14] = 0x4f; assert_eq!(payload(&c, &f, 17), Err(EINVAL));
}
#[test] fn payload_total_length() {
    let c = ip_ctx(); let base = udp_frame(4000, b"abc", true); let len = base.len();
    for (t, ok) in [(19u16, false), (20, true), (31, true), (46, true), (47, false)] { let mut f = base.clone(); put(&mut f[16..], t); fix_ip(&mut f); assert_eq!(payload(&c, &f[..len], 17).is_ok(), ok, "{t}"); }
}
#[test] fn payload_ttl_zero() { let c = ip_ctx(); let mut f = udp_frame(4000, b"a", true); f[22] = 0; fix_ip(&mut f); assert_eq!(payload(&c, &f, 17), Err(EINVAL)); f[22] = 1; fix_ip(&mut f); assert!(payload(&c, &f, 17).is_ok()); }
#[test] fn payload_header_checksum() { let c = ip_ctx(); let mut f = udp_frame(4000, b"a", true); f[24] ^= 1; assert_eq!(payload(&c, &f, 17), Err(EINVAL)); }
#[test] fn payload_fragment_rules() {
    let c = ip_ctx(); let base = udp_frame(4000, b"a", true);
    for (w, r) in [(0x8000u16, Err(EINVAL)), (0x2000, Err(UNSUPPORTED)), (0x0001, Err(UNSUPPORTED)), (0x1fff, Err(UNSUPPORTED)), (0x4000, Ok(())), (0x0000, Ok(()))] {
        let mut f = base.clone(); put(&mut f[20..], w); fix_ip(&mut f); assert_eq!(payload(&c, &f, 17).map(|_| ()), r, "{w:#x}");
    }
}
#[test] fn payload_protocol_and_addresses() {
    let c = ip_ctx(); let base = udp_frame(4000, b"a", true);
    assert_eq!(payload(&c, &base, 6), Err(UNSUPPORTED));
    let mut f = base.clone(); f[33] = 99; fix_ip(&mut f); assert_eq!(payload(&c, &f, 17), Err(DROP));
    let mut f = base.clone(); f[26] = 127; fix_ip(&mut f); assert_eq!(payload(&c, &f, 17), Err(EINVAL));
    let mut f = base.clone(); f[26..30].copy_from_slice(&GI); fix_ip(&mut f); assert_eq!(payload(&c, &f, 17), Err(EINVAL));
    let mut f = base; f[..6].copy_from_slice(&BC); assert_eq!(payload(&c, &f, 17), Err(DROP));
}
#[test] fn payload_short_ip() { let c = ip_ctx(); let f = udp_frame(4000, b"a", true); assert_eq!(payload(&c, &f[..33], 17), Err(EINVAL)); }
#[test] fn begin_layout() {
    let c = ip_ctx(); let mut out = [0xaau8; 200]; let w = begin(&c, &P, &PI, 17, 100, &mut out);
    assert_eq!(w, 134); assert_eq!(&out[..6], &P); assert_eq!(&out[6..12], &G); assert_eq!(get(&out[12..]), 0x0800); assert_eq!(out[14], 0x45);
    assert_eq!(get(&out[16..]), 120); assert_eq!(get(&out[18..]), 1); assert_eq!(get(&out[20..]), 0x4000); assert_eq!(out[22], 64); assert_eq!(out[23], 17);
    assert_eq!(&out[26..30], &GI); assert_eq!(&out[30..34], &PI); assert_eq!(reference(&out[14..34]), 0); assert!(out[34..134].iter().all(|&b| b == 0)); assert_eq!(out[134], 0xaa);
}
#[test] fn begin_minimum_frame() { let c = ip_ctx(); let mut out = [0xaau8; 80]; assert_eq!(begin(&c, &P, &PI, 1, 0, &mut out), 60); assert!(out[34..60].iter().all(|&b| b == 0)); assert_eq!(out[60], 0xaa); assert_eq!(wire_length(26), 60); assert_eq!(wire_length(27), 61); }
// ---------------- stage 3 ----------------
#[test] fn arp_init_validation() {
    let mut a = Arp::ZERO; a.pending = 7;
    assert_eq!(arp_init(&mut a, &[1, 0, 0, 0, 0, 0], &GI), EINVAL); assert_eq!(a.pending, 7);
    assert_eq!(arp_init(&mut a, &G, &[224, 0, 0, 1]), EINVAL); assert_eq!(arp_init(&mut a, &G, &GI), 0); assert_eq!(a.pending, 0); assert_eq!(a.mac, G); assert_eq!(a.ip, GI);
}
#[test] fn neighbor_expiry_boundary() {
    let mut a = arp_ctx(); learned(&mut a, PI, P, 1000);
    assert_eq!(neighbor_get(&mut a, &PI, 6999), Some(P)); assert_eq!(neighbor_get(&mut a, &PI, 7000), None); assert_eq!(a.neighbors[0].valid, 0);
}
#[test] fn neighbor_expiry_wraps() { let mut a = arp_ctx(); learned(&mut a, PI, P, u64::MAX - 10); assert_eq!(neighbor_get(&mut a, &PI, 100), Some(P)); assert_eq!(neighbor_get(&mut a, &PI, 6000), None); }
#[test] fn query_cached_busy_request() {
    let mut a = arp_ctx(); let mut out = [0u8; 60];
    assert_eq!(arp_query(&mut a, &PI, 10, true, &mut out), REQUEST); assert_eq!(&out[..6], &BC); assert_eq!(get(&out[20..]), 1); assert_eq!(&out[38..42], &PI); assert_eq!(&out[32..38], &[0; 6]);
    assert_eq!(a.pending, 1); assert_eq!(a.pending_ip, PI); assert_eq!(a.pending_since, 10);
    assert_eq!(arp_query(&mut a, &[10, 0, 2, 3], 209, true, &mut out), BUSY); assert_eq!(arp_query(&mut a, &[10, 0, 2, 3], 210, true, &mut out), REQUEST);
    learned(&mut a, PI, P, 300); assert_eq!(arp_query(&mut a, &PI, 300, true, &mut out), CACHED);
}
#[test] fn query_rejects_self_and_nospace() {
    let mut a = arp_ctx(); let mut out = [0u8; 60];
    assert_eq!(arp_query(&mut a, &GI, 10, true, &mut out), EINVAL); assert_eq!(arp_query(&mut a, &PI, 10, false, &mut out), NOSPACE); assert_eq!(a.pending, 0);
}
#[test] fn input_request_replies_and_learns() {
    let mut a = arp_ctx(); let mut out = [0u8; 60]; let f = arp_frame(1, BC, P, PI, [0; 6], GI);
    assert_eq!(ethernet_input(&mut a, &f, 50, true, &mut out), REPLY); assert_eq!(&out[..6], &P); assert_eq!(get(&out[20..]), 2); assert_eq!(&out[28..32], &GI); assert_eq!(&out[32..38], &P); assert_eq!(&out[38..42], &PI);
    assert_eq!(neighbor_get(&mut a, &PI, 50), Some(P));
}
#[test] fn input_probe_replies_without_learning() {
    let mut a = arp_ctx(); let mut out = [0u8; 60]; let f = arp_frame(1, BC, P, [0; 4], [0; 6], GI);
    assert_eq!(ethernet_input(&mut a, &f, 50, true, &mut out), REPLY); assert!(a.neighbors.iter().all(|n| n.valid == 0)); assert_eq!(&out[38..42], &[0; 4]);
}
#[test] fn input_reply_rules() {
    let mut a = arp_ctx(); let mut out = [0u8; 60]; let f = arp_frame(2, G, P, PI, G, GI);
    assert_eq!(ethernet_input(&mut a, &f, 50, true, &mut out), DROP);
    assert_eq!(arp_query(&mut a, &PI, 40, true, &mut out), REQUEST);
    assert_eq!(ethernet_input(&mut a, &f, 240, true, &mut out), DROP);
    assert_eq!(ethernet_input(&mut a, &f, 239, true, &mut out), LEARNED); assert_eq!(a.pending, 0); assert_eq!(neighbor_get(&mut a, &PI, 239), Some(P));
    let mut b = arp_ctx(); assert_eq!(arp_query(&mut b, &PI, 40, true, &mut out), REQUEST); let bc = arp_frame(2, BC, P, PI, G, GI); assert_eq!(ethernet_input(&mut b, &bc, 41, true, &mut out), DROP);
    let other = arp_frame(2, G, P, [10, 0, 2, 3], G, GI); assert_eq!(ethernet_input(&mut b, &other, 41, true, &mut out), DROP); assert_eq!(b.pending, 1);
}
#[test] fn input_field_validation() {
    let mut a = arp_ctx(); let mut out = [0u8; 60]; let base = arp_frame(1, BC, P, PI, [0; 6], GI);
    let mut f = base.clone(); f[38] = 9; assert_eq!(ethernet_input(&mut a, &f, 1, true, &mut out), DROP);
    let mut f = base.clone(); f[22] ^= 2; assert_eq!(ethernet_input(&mut a, &f, 1, true, &mut out), EINVAL);
    let mut f = base.clone(); put(&mut f[14..], 6); assert_eq!(ethernet_input(&mut a, &f, 1, true, &mut out), UNSUPPORTED);
    let mut f = base.clone(); f[18] = 8; assert_eq!(ethernet_input(&mut a, &f, 1, true, &mut out), EINVAL);
    let mut f = base.clone(); put(&mut f[20..], 3); assert_eq!(ethernet_input(&mut a, &f, 1, true, &mut out), EINVAL);
    let mut f = base.clone(); f[28..32].copy_from_slice(&GI); assert_eq!(ethernet_input(&mut a, &f, 1, true, &mut out), EINVAL);
    assert_eq!(ethernet_input(&mut a, &base[..41], 1, true, &mut out), EINVAL); assert_eq!(ethernet_input(&mut a, &base, 1, false, &mut out), NOSPACE);
    assert!(a.neighbors.iter().all(|n| n.valid == 0));
}
#[test] fn learn_slot_selection() {
    let mut a = arp_ctx(); for i in 0..4 { a.neighbors[i] = Neighbor { ip: [10, 0, 2, 100 + i as u8], mac: P, pad0: [0; 6], seen: 500, valid: 1, pad1: [0; 4] }; }
    let mut out = [0u8; 60]; let f = arp_frame(1, BC, P, [10, 0, 2, 9], [0; 6], GI);
    assert_eq!(ethernet_input(&mut a, &f, 600, true, &mut out), REPLY); assert_eq!(a.neighbors[3].ip, [10, 0, 2, 9]);
    a.neighbors[1].seen = 100; let f = arp_frame(1, BC, P, [10, 0, 2, 8], [0; 6], GI); assert_eq!(ethernet_input(&mut a, &f, 600, true, &mut out), REPLY); assert_eq!(a.neighbors[1].ip, [10, 0, 2, 8]);
    a.neighbors[2].seen = 600; a.neighbors[2].valid = 0; let f = arp_frame(1, BC, P, [10, 0, 2, 7], [0; 6], GI); assert_eq!(ethernet_input(&mut a, &f, 600, true, &mut out), REPLY); assert_eq!(a.neighbors[2].ip, [10, 0, 2, 7]);
    let f = arp_frame(1, BC, [0x52, 0x54, 0, 1, 1, 1], [10, 0, 2, 8], [0; 6], GI); let mut f2 = f.clone(); f2[6..12].copy_from_slice(&[0x52, 0x54, 0, 1, 1, 1]);
    assert_eq!(ethernet_input(&mut a, &f2, 650, true, &mut out), REPLY); assert_eq!(a.neighbors[1].mac, [0x52, 0x54, 0, 1, 1, 1]); assert_eq!(a.neighbors[1].seen, 650);
}
#[test] fn echo_request_layout_and_state() {
    let mut c = ip_ctx(); learned(&mut c.arp, PI, P, 100); let mut out = [0u8; ECHO_WIRE_MAX];
    assert_eq!(echo_request(&mut c, &PI, 0x564f, 7, b"ping", 150, true, &mut out), ECHO_REQUEST);
    assert_eq!(&out[..6], &P); assert_eq!(out[34], 8); assert_eq!(get(&out[38..]), 0x564f); assert_eq!(get(&out[40..]), 7); assert_eq!(&out[42..46], b"ping"); assert_eq!(reference(&out[34..46]), 0);
    assert_eq!((c.pending, c.id, c.sequence, c.data_length, c.sent, c.identification), (1, 0x564f, 7, 4, 1, 2)); assert_eq!(&c.data[..4], b"ping"); assert_eq!(c.peer_mac, P);
}
#[test] fn echo_request_busy_and_neighbor() {
    let mut c = ip_ctx(); let mut out = [0u8; ECHO_WIRE_MAX];
    assert_eq!(echo_request(&mut c, &PI, 1, 1, b"", 10, true, &mut out), NEIGHBOR); learned(&mut c.arp, PI, P, 10);
    assert_eq!(echo_request(&mut c, &PI, 1, 1, b"", 10, true, &mut out), ECHO_REQUEST); assert_eq!(echo_request(&mut c, &PI, 1, 2, b"", 209, true, &mut out), BUSY);
    assert_eq!(echo_request(&mut c, &PI, 1, 2, b"", 210, true, &mut out), ECHO_REQUEST); assert_eq!(echo_request(&mut c, &GI, 1, 2, b"", 999, true, &mut out), EINVAL);
    assert_eq!(echo_request(&mut c, &PI, 1, 2, &[0; 65], 999, true, &mut out), EINVAL); assert_eq!(echo_request(&mut c, &PI, 1, 2, b"", 999, false, &mut out), NOSPACE);
}
#[test] fn echo_reply_match_rules() {
    let mut c = ip_ctx(); learned(&mut c.arp, PI, P, 100); let mut out = [0u8; ECHO_WIRE_MAX]; let mut frame = [0u8; FRAME_MAX]; let mut n = 0;
    assert_eq!(echo_request(&mut c, &PI, 9, 3, b"abc", 100, true, &mut out), ECHO_REQUEST);
    for bad in [icmp(0, 9, 4, b"abc"), icmp(0, 8, 3, b"abc"), icmp(0, 9, 3, b"abd"), icmp(0, 9, 3, b"ab")] { assert_eq!(ipv4_input(&mut c, &bad, 120, &yes, &mut frame, &mut n), DROP); }
    assert_eq!(ipv4_input(&mut c, &icmp(0, 9, 3, b"abc"), 300, &yes, &mut frame, &mut n), DROP);
    assert_eq!(ipv4_input(&mut c, &icmp(0, 9, 3, b"abc"), 299, &yes, &mut frame, &mut n), ECHO_MATCH); assert_eq!((c.pending, c.matched), (0, 1));
    assert_eq!(ipv4_input(&mut c, &icmp(0, 9, 3, b"abc"), 299, &yes, &mut frame, &mut n), DROP);
}
#[test] fn echo_request_is_answered() {
    let mut c = ip_ctx(); let mut frame = [0u8; FRAME_MAX]; let mut n = 0; let f = icmp(8, 0x1234, 5, b"hello vos");
    assert_eq!(ipv4_input(&mut c, &f, 1, &yes, &mut frame, &mut n), REPLY); assert_eq!(n, 60); assert_eq!(&frame[..6], &P); assert_eq!(frame[34], 0);
    assert_eq!(get(&frame[38..]), 0x1234); assert_eq!(get(&frame[40..]), 5); assert_eq!(&frame[42..51], b"hello vos"); assert_eq!(reference(&frame[34..51]), 0); assert_eq!((c.answered, c.identification), (1, 2));
    assert_eq!(ipv4_input(&mut c, &f, 1, &|_| false, &mut frame, &mut n), NOSPACE);
}
#[test] fn icmp_validation() {
    let mut c = ip_ctx(); let mut frame = [0u8; FRAME_MAX]; let mut n = 0;
    let mut f = icmp(8, 1, 1, b"x"); f[35] = 1; let s = { f[36] = 0; f[37] = 0; reference(&f[34..43]) }; put(&mut f[36..], s); assert_eq!(ipv4_input(&mut c, &f, 1, &yes, &mut frame, &mut n), EINVAL);
    let mut f = icmp(8, 1, 1, b"x"); f[36] ^= 1; assert_eq!(ipv4_input(&mut c, &f, 1, &yes, &mut frame, &mut n), EINVAL);
    assert_eq!(ipv4_input(&mut c, &icmp(3, 1, 1, b"x"), 1, &yes, &mut frame, &mut n), UNSUPPORTED); assert_eq!(c.answered, 0);
}
#[test] fn ipv4_input_dispatches_arp() {
    let mut c = ip_ctx(); let mut frame = [0u8; FRAME_MAX]; let mut n = 0; let f = arp_frame(1, BC, P, PI, [0; 6], GI);
    assert_eq!(ipv4_input(&mut c, &f, 1, &yes, &mut frame, &mut n), REPLY); assert_eq!(n, ARP_WIRE);
}
#[test] fn ipv4_init_rules() { let mut c = Ipv4::ZERO; c.sent = 9; assert_eq!(ipv4_init(&mut c, &[0; 6], &GI), EINVAL); assert_eq!(c.sent, 9); assert_eq!(ipv4_init(&mut c, &G, &GI), 0); assert_eq!((c.sent, c.identification), (0, 1)); }
// ---------------- stage 4 ----------------
#[test] fn udp_init_rules() { let mut c = Udp::ZERO; assert_eq!(udp_init(&mut c, &G, &GI, 0), EINVAL); assert_eq!(udp_init(&mut c, &G, &[127, 0, 0, 1], 9), EINVAL); assert_eq!(udp_init(&mut c, &G, &GI, 9), 0); assert_eq!((c.port, c.ip.identification), (9, 1)); }
#[test] fn udp_send_layout() {
    let mut c = udp_ctx(); let mut out = [0u8; UDP_WIRE_MAX]; let mut n = 0;
    assert_eq!(udp_send(&mut c, &PI, 5000, b"hello", 150, true, &mut out, &mut n), UDP_SENT); assert_eq!(n, 60);
    assert_eq!(out[23], 17); assert_eq!(get(&out[34..]), 4000); assert_eq!(get(&out[36..]), 5000); assert_eq!(get(&out[38..]), 13); assert_eq!(&out[42..47], b"hello");
    assert_eq!(udp_sum(&GI, &PI, &out[34..47]), 0); assert_eq!(c.ip.identification, 2);
}
#[test] fn udp_send_zero_checksum_is_ffff() {
    let mut c = udp_ctx(); let mut out = [0u8; UDP_WIRE_MAX]; let mut n = 0; let mut data = [0u8; 2];
    for x in 0..=0xffffu32 { data = [(x >> 8) as u8, x as u8]; let mut u = vec![0x0f, 0xa0, 0x13, 0x88, 0, 10, 0, 0]; u.extend_from_slice(&data); if udp_sum(&GI, &PI, &u) == 0 { break; } }
    assert_eq!(udp_send(&mut c, &PI, 5000, &data, 150, true, &mut out, &mut n), UDP_SENT); assert_eq!(get(&out[40..]), 0xffff);
}
#[test] fn udp_send_rejections() {
    let mut c = udp_ctx(); let mut out = [0u8; UDP_WIRE_MAX]; let mut n = 0;
    for peer in [[0u8, 1, 2, 3], [127, 0, 0, 1], [224, 0, 0, 1], GI] { assert_eq!(udp_send(&mut c, &peer, 5000, b"", 150, true, &mut out, &mut n), EINVAL); }
    assert_eq!(udp_send(&mut c, &PI, 0, b"", 150, true, &mut out, &mut n), EINVAL); assert_eq!(udp_send(&mut c, &PI, 5000, &[0; 513], 150, true, &mut out, &mut n), EINVAL);
    assert_eq!(udp_send(&mut c, &PI, 5000, b"", 150, false, &mut out, &mut n), NOSPACE); assert_eq!(udp_send(&mut c, &[10, 0, 2, 3], 5000, b"", 150, true, &mut out, &mut n), NEIGHBOR);
    assert_eq!(c.ip.identification, 1);
}
#[test] fn udp_input_queues_fifo() {
    let mut c = udp_ctx();
    for i in 0..4u8 { assert_eq!(udp_input(&mut c, &udp_frame(4000, &[i; 3], i & 1 == 0)), UdpIn::Code(UDP_QUEUED)); }
    assert_eq!(udp_input(&mut c, &udp_frame(4000, b"x", true)), UdpIn::Code(UDP_FULL)); assert_eq!(c.count, 4);
    for i in 0..4u8 { let s = udp_receive_slot(&c).unwrap(); assert_eq!(c.queue[s].data[..3], [i; 3]); assert_eq!(c.queue[s].checksum_present, (i & 1 == 0) as i32); assert_eq!(c.queue[s].port, 5000); udp_receive_finish(&mut c); assert!(c.queue[s].data.iter().all(|&b| b == 0)); }
    assert_eq!(udp_receive_slot(&c), Err(UDP_EMPTY)); assert_eq!(c.head, 0);
}
#[test] fn udp_input_validation() {
    let mut c = udp_ctx(); let base = udp_frame(4000, b"payload", true);
    let mut f = base.clone(); f[41] ^= 1; assert_eq!(udp_input(&mut c, &f), UdpIn::Code(EINVAL));
    let mut f = udp_frame(4000, b"payload", false); put(&mut f[38..], 14); assert_eq!(udp_input(&mut c, &f), UdpIn::Code(EINVAL));
    let mut f = base.clone(); put(&mut f[38..], 7); assert_eq!(udp_input(&mut c, &f), UdpIn::Code(EINVAL));
    assert_eq!(udp_input(&mut c, &udp_frame(4001, b"x", true)), UdpIn::Code(DROP)); assert_eq!(udp_input(&mut c, &udp_frame(0, b"x", true)), UdpIn::Code(EINVAL));
    assert_eq!(udp_input(&mut c, &udp_frame(4000, &[7; 513], true)), UdpIn::Code(NOSPACE)); assert_eq!(c.count, 0);
    assert_eq!(udp_input(&mut c, &udp_frame(4000, b"abc", false)), UdpIn::Code(UDP_QUEUED));
}
#[test] fn udp_input_delegates_other_protocols() { let mut c = udp_ctx(); assert_eq!(udp_input(&mut c, &icmp(8, 1, 1, b"x")), UdpIn::Ipv4Input); assert_eq!(udp_input(&mut c, &arp_frame(1, BC, P, PI, [0; 6], GI)), UdpIn::Ipv4Input); }
#[test] fn udp_closed_endpoint() { let mut c = udp_ctx(); udp_close(&mut c); assert_eq!(c.port, 0); assert_eq!(udp_input(&mut c, &udp_frame(4000, b"x", true)), UdpIn::Code(EINVAL)); assert_eq!(udp_receive_slot(&c), Err(EINVAL)); }
#[test] fn udp_head_wraps() {
    let mut c = udp_ctx(); c.head = 3; c.count = 0;
    assert_eq!(udp_input(&mut c, &udp_frame(4000, b"q", true)), UdpIn::Code(UDP_QUEUED)); assert_eq!(c.queue[3].data[0], b'q');
    assert_eq!(udp_input(&mut c, &udp_frame(4000, b"r", true)), UdpIn::Code(UDP_QUEUED)); assert_eq!(c.queue[0].data[0], b'r');
    assert_eq!(udp_receive_slot(&c), Ok(3)); udp_receive_finish(&mut c); assert_eq!((c.head, c.count), (0, 1));
}
#[test] fn constants() { assert_eq!((ARP_TTL, ARP_QUERY_TTL, PING_TTL, NEIGHBORS, UDP_QUEUE, UDP_DATA_MAX, PING_DATA_MAX), (6000, 200, 200, 4, 4, 512, 64)); }
