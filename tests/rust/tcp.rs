// R3-N2 safe-core tests: the Rust TCP state machine driven directly (no C, no
// FFI). Frames are built here and checked with the R3-N1 checksum core.
use crate::net::checksum::{checksum, transport_checksum};
use crate::net::context::*;
use crate::net::tcp::*;

const G: [u8; 6] = [0x52, 0x54, 0, 0x12, 0x34, 0x56];
const P: [u8; 6] = [0x52, 0x54, 0, 0xaa, 0xbb, 0xcc];
const GI: [u8; 4] = [10, 0, 2, 15];
const PI: [u8; 4] = [10, 0, 2, 2];

fn seg(flags: u8, seq: u32, ack: u32, win: u16, data: &[u8]) -> ([u8; 1514], usize) {
    let mut f = [0u8; 1514];
    let wire = if 54 + data.len() < 60 { 60 } else { 54 + data.len() };
    f[..6].copy_from_slice(&G);
    f[6..12].copy_from_slice(&P);
    f[12..14].copy_from_slice(&0x0800u16.to_be_bytes());
    f[14] = 0x45;
    f[16..18].copy_from_slice(&((40 + data.len()) as u16).to_be_bytes());
    f[20..22].copy_from_slice(&0x4000u16.to_be_bytes());
    f[22] = 64;
    f[23] = 6;
    f[26..30].copy_from_slice(&PI);
    f[30..34].copy_from_slice(&GI);
    let s = checksum(&f[14..34]);
    f[24..26].copy_from_slice(&s.to_be_bytes());
    f[34..36].copy_from_slice(&5001u16.to_be_bytes());
    f[36..38].copy_from_slice(&4001u16.to_be_bytes());
    f[38..42].copy_from_slice(&seq.to_be_bytes());
    f[42..46].copy_from_slice(&ack.to_be_bytes());
    f[46] = 0x50;
    f[47] = flags;
    f[48..50].copy_from_slice(&win.to_be_bytes());
    f[54..54 + data.len()].copy_from_slice(data);
    let t = transport_checksum(&PI, &GI, 6, &f[34..54 + data.len()]);
    f[50..52].copy_from_slice(&t.to_be_bytes());
    (f, wire)
}
fn sink() -> Sink {
    // Distinct, aligned, non-overlapping fake addresses: admission is pure arithmetic.
    Sink::new(0x10000, 1514, 0x20000)
}
fn input(c: &mut Tcp, f: &([u8; 1514], usize), now: u64) -> (i32, Sink) {
    let mut s = sink();
    let r = match tcp_input(c, &f.0[..f.1], now, &mut s) {
        TcpIn::Code(e) => e,
        TcpIn::Ipv4Input => 1000,
    };
    (r, s)
}
fn endpoint(isn: u32, pay: bool, retx: bool, clo: bool) -> Tcp {
    let mut c = Tcp::ZERO;
    assert_eq!(tcp_init(&mut c, &G, &GI, 4001, isn), 0);
    if pay {
        assert_eq!(tcp_enable_payload(&mut c), 0);
    }
    if retx {
        assert_eq!(tcp_enable_retransmission(&mut c), 0);
    }
    if clo {
        assert_eq!(tcp_enable_closure(&mut c), 0);
    }
    c
}
fn established(isn: u32, peer: u32, win: u16, retx: bool, clo: bool) -> Tcp {
    let mut c = endpoint(isn, true, retx, clo);
    assert_eq!(input(&mut c, &seg(2, peer, 0, 1024, &[]), 10).0, SYNACK);
    let sn = c.snd_next;
    assert_eq!(input(&mut c, &seg(16, peer.wrapping_add(1), sn, win, &[]), 11).0, CONNECTED);
    c
}
fn get32(f: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([f[at], f[at + 1], f[at + 2], f[at + 3]])
}

#[test]
fn shift_matches_forward_copy_exhaustively() {
    for count in 0..=64u32 {
        for consumed in 0..=count {
            let mut p = [0u8; 64];
            for (i, b) in p.iter_mut().enumerate() {
                *b = (i as u8).wrapping_mul(7).wrapping_add(1);
            }
            let old = p;
            crate::net::tcp::shift_for_tests(&mut p, count, consumed);
            for i in 0..64u32 {
                let want = if i < count - consumed { old[(i + consumed) as usize] } else if i < count { 0 } else { old[i as usize] };
                assert_eq!(p[i as usize], want, "count {count} consumed {consumed} index {i}");
            }
        }
    }
}
#[test]
fn shift_out_of_contract_stays_in_bounds() {
    for (count, consumed) in [(65u32, 1u32), (200, 3), (5, 9), (u32::MAX, 0), (64, 65)] {
        let mut p = [0x5au8; 64];
        crate::net::tcp::shift_for_tests(&mut p, count, consumed); // must not panic or overrun
    }
}
#[test]
fn init_rules_and_failed_init_leaves_context() {
    let mut c = Tcp::ZERO;
    c.rx[0] = 9;
    assert_eq!(tcp_init(&mut c, &G, &GI, 0, 1), EINVAL);
    assert_eq!(tcp_init(&mut c, &[1, 0, 0, 0, 0, 1], &GI, 4001, 1), EINVAL);
    assert_eq!(tcp_init(&mut c, &G, &[127, 0, 0, 1], 4001, 1), EINVAL);
    assert_eq!(c.rx[0], 9);
    assert_eq!(tcp_init(&mut c, &G, &GI, 4001, 0x1234_5678), 0);
    assert!(c.state == LISTEN && c.next_isn == 0x1234_5678 && c.ip.identification == 1 && c.rx[0] == 0 && c.port == 4001);
}
#[test]
fn profile_enable_ordering() {
    let mut c = endpoint(1, false, false, false);
    assert_eq!(tcp_enable_retransmission(&mut c), EINVAL);
    assert_eq!(tcp_enable_closure(&mut c), EINVAL);
    assert_eq!(tcp_enable_payload(&mut c), 0);
    assert_eq!(tcp_enable_closure(&mut c), 0);
    let mut d = established(1, 2, 64, false, false);
    assert_eq!(tcp_enable_payload(&mut d), BUSY);
    tcp_close(&mut d);
    assert_eq!(tcp_enable_payload(&mut d), EINVAL);
}
#[test]
fn syn_ack_layout_window_and_isn_step() {
    for pay in [false, true] {
        let mut c = endpoint(0xffff_fff0, pay, false, false);
        let (r, s) = input(&mut c, &seg(2, 0xffff_ffff, 0, 512, &[]), 100);
        assert_eq!(r, SYNACK);
        let f = &s.frame[..s.len];
        assert_eq!(s.len, 60);
        assert_eq!(f[47], 18);
        assert_eq!(get32(f, 38), 0xffff_fff0);
        assert_eq!(get32(f, 42), 0);
        assert_eq!(u16::from_be_bytes([f[48], f[49]]), if pay { 64 } else { 0 });
        assert_eq!(transport_checksum(&GI, &PI, 6, &f[34..54]), 0);
        assert_eq!(checksum(&f[14..34]), 0);
        assert_eq!(c.next_isn, 0x0000_fff0);
        assert!(c.state == SYN_RECEIVED && c.rcv_next == 0 && c.snd_next == 0xffff_fff1 && c.ip.identification == 2);
    }
}
#[test]
fn listen_rejections() {
    let mut c = endpoint(1, true, false, false);
    assert_eq!(input(&mut c, &seg(2, 5, 9, 1, &[]), 1).0, DROP); // SYN with ACK field
    assert_eq!(input(&mut c, &seg(18, 5, 0, 1, &[]), 1).0, UNSUPPORTED);
    let mut f = seg(2, 5, 0, 1, &[]);
    f.0[52] = 1; // urgent pointer
    let t = transport_checksum(&PI, &GI, 6, &{
        let mut h = [0u8; 20];
        h.copy_from_slice(&f.0[34..54]);
        h[16] = 0;
        h[17] = 0;
        h
    });
    f.0[50..52].copy_from_slice(&t.to_be_bytes());
    assert_eq!(input(&mut c, &f, 1).0, UNSUPPORTED);
    let mut bad = seg(2, 5, 0, 1, &[]);
    bad.0[51] ^= 1;
    assert_eq!(input(&mut c, &bad, 1).0, EINVAL);
    assert_eq!(c.state, LISTEN);
}
#[test]
fn listen_nospace_changes_nothing() {
    let mut c = endpoint(7, true, false, false);
    let before = (c.state, c.next_isn, c.ip.identification);
    let mut s = Sink::new(0x10000, 59, 0x20000);
    let f = seg(2, 5, 0, 1, &[]);
    assert!(matches!(tcp_input(&mut c, &f.0[..f.1], 1, &mut s), TcpIn::Code(NOSPACE)));
    assert_eq!((c.state, c.next_isn, c.ip.identification, s.len), (before.0, before.1, before.2, 0));
    let mut s = Sink::new(0x10000, 1514, 0x10008); // length inside the packet output
    assert!(matches!(tcp_input(&mut c, &f.0[..f.1], 1, &mut s), TcpIn::Code(NOSPACE)));
}
#[test]
fn handshake_ttl_boundary() {
    for (dt, want) in [(199u64, CONNECTED), (200, EXPIRED)] {
        let mut c = endpoint(1, false, false, false);
        input(&mut c, &seg(2, 50, 0, 1, &[]), u64::MAX - 100);
        let sn = c.snd_next;
        assert_eq!(input(&mut c, &seg(16, 51, sn, 1, &[]), (u64::MAX - 100).wrapping_add(dt)).0, want);
        assert_eq!(tcp_expire(&mut c, (u64::MAX - 100).wrapping_add(dt)), if want == EXPIRED { 1 } else { 0 });
    }
}
#[test]
fn data_ack_window_and_partial_receive() {
    let mut c = established(0x7fff_fff0, 0xffff_fff8, 64, false, false);
    let rn = c.rcv_next;
    let data: [u8; 20] = core::array::from_fn(|i| i as u8 + 1);
    let una = c.snd_una;
    let (r, s) = input(&mut c, &seg(24, rn, una, 64, &data), 20);
    assert_eq!(r, DATA_ACK);
    assert_eq!(get32(&s.frame, 42), rn.wrapping_add(20));
    assert_eq!(u16::from_be_bytes([s.frame[48], s.frame[49]]), 44);
    assert_eq!(c.rx_used, 20);
    assert_eq!(tcp_receive_begin(&c, 0x10000, 8, 0x20000, 21), Ok(8));
    tcp_receive_finish(&mut c, 8, 21);
    assert!(c.rx_used == 12 && c.rx[..12] == data[8..] && c.rx[12..20] == [0u8; 8] && c.window_dirty == 1);
    assert_eq!(tcp_receive_begin(&c, 0x10000, 0, 0x20000, 22), Err(NOSPACE));
}
#[test]
fn serial_ack_rules_at_half_space() {
    let mut c = established(0xffff_ff00, 1, 64, false, false);
    let una = c.snd_una;
    let rn = c.rcv_next;
    assert_eq!(input(&mut c, &seg(16, rn, una.wrapping_sub(0x8000_0000), 64, &[]), 20).0, DROP);
    assert_eq!(input(&mut c, &seg(16, rn, una.wrapping_sub(0x7fff_ffff), 64, &[]), 20).0, ACKED); // old: ignored
    assert_eq!(input(&mut c, &seg(16, rn, una.wrapping_add(1), 64, &[]), 20).0, DROP); // beyond snd_next
}
#[test]
fn out_of_window_data_elicits_ack_only() {
    let mut c = established(5, 9, 64, false, false);
    let rn = c.rcv_next;
    let una = c.snd_una;
    let (r, s) = input(&mut c, &seg(24, rn.wrapping_add(1), una, 64, &[1, 2]), 20);
    assert_eq!(r, ACK);
    assert_eq!(get32(&s.frame, 42), rn);
    assert_eq!(c.rx_used, 0);
}
#[test]
fn send_retransmit_timer_and_budget() {
    let mut c = established(100, 200, 64, true, false);
    let mut s = sink();
    assert_eq!(tcp_send(&mut c, Some(&[7u8; 10]), 20, &mut s), TX_DATA);
    assert!(c.tx_used == 10 && c.retry_after == RETRY_TICKS && s.len == 64 && s.frame[47] == 24);
    let mut s = sink();
    assert_eq!(tcp_send(&mut c, Some(&[1]), 20, &mut s), BUSY);
    for (k, t) in [(0u8, 69u64), (0, 70), (1, 120), (2, 170), (3, 220)] {
        let mut s = sink();
        let r = tcp_retransmit(&mut c, t, &mut s);
        if t == 69 {
            assert_eq!(r, NOT_DUE);
        } else if t == 220 {
            assert_eq!(r, EXPIRED); // absolute TX TTL (200) reached first
            assert_eq!(c.state, LISTEN);
        } else {
            assert_eq!(r, RETRANSMITTED);
            assert_eq!(c.retry_count, k + 1);
        }
    }
}
#[test]
fn windows_bound_send_and_retransmit() {
    let mut c = established(100, 200, 4, true, false);
    let mut s = sink();
    assert_eq!(tcp_send(&mut c, Some(&[1u8; 5]), 20, &mut s), WINDOW);
    assert_eq!(tcp_send(&mut c, Some(&[1u8; 4]), 20, &mut s), TX_DATA);
    let rn = c.rcv_next;
    let una = c.snd_una;
    assert_eq!(input(&mut c, &seg(16, rn, una, 0, &[]), 21).0, ACKED);
    let mut s = sink();
    assert_eq!(tcp_retransmit(&mut c, 90, &mut s), WINDOW);
}
#[test]
fn active_close_through_time_wait() {
    let mut c = established(1000, 2000, 64, true, true);
    let mut s = sink();
    assert_eq!(tcp_finish(&mut c, 20, &mut s), FIN_SENT);
    assert!(c.state == FIN_WAIT_1 && s.frame[47] == 17 && transport_checksum(&GI, &PI, 6, &s.frame[34..54]) == 0);
    let (rn, sn) = (c.rcv_next, c.snd_next);
    assert_eq!(input(&mut c, &seg(16, rn, sn, 64, &[]), 21).0, ACKED);
    assert_eq!(c.state, FIN_WAIT_2);
    assert_eq!(input(&mut c, &seg(17, rn, sn, 64, &[]), 22).0, FIN_ACK);
    assert_eq!(c.state, TIME_WAIT);
    assert_eq!(tcp_expire(&mut c, 22 + TIME_WAIT_TTL - 1), 0);
    assert_eq!(tcp_expire(&mut c, 22 + TIME_WAIT_TTL), DONE);
    assert_eq!(c.state, LISTEN);
}
#[test]
fn passive_close_eof_and_last_ack() {
    let mut c = established(1000, 2000, 64, false, true);
    let (rn, sn) = (c.rcv_next, c.snd_next);
    assert_eq!(input(&mut c, &seg(25, rn, sn, 64, &[9, 8, 7]), 30).0, FIN_ACK);
    assert_eq!(c.state, CLOSE_WAIT);
    let mut s = sink();
    assert_eq!(tcp_finish(&mut c, 31, &mut s), BUSY); // unread data
    assert_eq!(tcp_receive_begin(&c, 0x10000, 64, 0x20000, 31), Ok(3));
    tcp_receive_finish(&mut c, 3, 31);
    assert_eq!(tcp_receive_begin(&c, 0x10000, 64, 0x20000, 31), Err(EOF));
    assert_eq!(tcp_finish(&mut c, 32, &mut s), FIN_SENT);
    assert_eq!(c.state, LAST_ACK);
    let (rn, sn) = (c.rcv_next, c.snd_next);
    assert_eq!(input(&mut c, &seg(16, rn, sn, 64, &[]), 33).0, DONE);
    assert_eq!(c.state, LISTEN);
}
#[test]
fn simultaneous_close_and_duplicate_fin() {
    let mut c = established(1000, 2000, 64, false, true);
    let mut s = sink();
    assert_eq!(tcp_finish(&mut c, 20, &mut s), FIN_SENT);
    let (rn, una) = (c.rcv_next, c.snd_una);
    assert_eq!(input(&mut c, &seg(17, rn, una, 64, &[]), 21).0, FIN_ACK);
    assert_eq!(c.state, CLOSING);
    let (rn, sn) = (c.rcv_next, c.snd_next);
    assert_eq!(input(&mut c, &seg(16, rn, sn, 64, &[]), 22).0, ACKED);
    assert_eq!(c.state, TIME_WAIT);
    assert_eq!(input(&mut c, &seg(17, rn.wrapping_sub(1), sn, 64, &[]), 40).0, ACK);
    assert_eq!(c.tx_since, 40);
}
#[test]
fn reset_rules() {
    let mut c = established(1000, 2000, 64, false, true);
    let rn = c.rcv_next;
    assert_eq!(input(&mut c, &seg(4, rn.wrapping_add(1), 0, 0, &[]), 20).0, ACK); // challenge
    assert_eq!(input(&mut c, &seg(4, rn.wrapping_add(64), 0, 0, &[]), 20).0, DROP);
    assert_eq!(input(&mut c, &seg(4, rn, 0, 0, &[]), 20).0, RESET);
    assert!(c.state == LISTEN && c.port == 4001 && c.closure_enabled == 1);
}
#[test]
fn abort_preserves_endpoint_only() {
    let mut c = established(0x0101_0101, 7, 64, true, true);
    let mut s = sink();
    tcp_send(&mut c, Some(&[3u8; 9]), 15, &mut s);
    let (ident, isn) = (c.ip.identification, c.next_isn);
    c.pad0 = [1, 2, 3];
    tcp_abort(&mut c);
    assert!(c.state == LISTEN && c.tx == [0u8; 64] && c.tx_used == 0 && c.snd_next == 0 && c.peer_port == 0 && c.peer == [0; 4]);
    assert!(c.ip.identification == ident && c.next_isn == isn && c.port == 4001 && c.payload_enabled == 1 && c.retry_enabled == 1);
    assert!(c.closure_enabled == 1 && c.pad0 == [1, 2, 3]);
    tcp_close(&mut c);
    assert!(c.port == 0 && c.pad0 == [0; 3] && c.ip.identification == 0);
}
#[test]
fn non_tcp_frames_are_delegated() {
    let mut c = endpoint(1, false, false, false);
    let mut f = seg(2, 1, 0, 1, &[]);
    f.0[23] = 17;
    let s = checksum(&{
        let mut h = [0u8; 20];
        h.copy_from_slice(&f.0[14..34]);
        h[10] = 0;
        h[11] = 0;
        h
    });
    f.0[24..26].copy_from_slice(&s.to_be_bytes());
    assert_eq!(input(&mut c, &f, 1).0, 1000);
}
#[test]
fn constants_match_net_tcp_h() {
    assert_eq!((HANDSHAKE_TTL, RX_MAX, TX_MAX, TX_TTL, IDLE_TTL, RETRY_TICKS, RETRY_MAX), (200, 64, 64, 200, 400, 50, 3));
    assert_eq!((MSL_TICKS, TIME_WAIT_TTL, CLOSE_TTL, CLOSE_HARD_TTL), (12000, 24000, 400, 48000));
    assert_eq!([LISTEN, SYN_RECEIVED, ESTABLISHED, CLOSE_WAIT, FIN_WAIT_1, FIN_WAIT_2, CLOSING, LAST_ACK, TIME_WAIT], [1, 2, 3, 4, 5, 6, 7, 8, 9]);
    assert_eq!([SYNACK, CONNECTED, EXPIRED, DATA_ACK, ACK, TX_DATA, ACKED, EMPTY, WINDOW, RETRANSMITTED, NOT_DUE], [9, 10, -8, 11, 12, 13, 14, -9, -10, 15, -11]);
    assert_eq!([FIN_SENT, FIN_ACK, RESET, DONE, EOF], [16, 17, 18, 19, -12]);
    assert_eq!(core::mem::size_of::<Tcp>(), 504);
    let _ = BUSY;
}
