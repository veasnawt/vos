extern crate boot_hid as hid;
use hid::*;
fn device() -> Device {
    Device::parse(
        &[
            18, 1, 0, 2, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 0, 0, 0, 1,
        ],
        Speed::Full,
    )
    .unwrap()
}
fn interface(n: u8, protocol: u8, endpoint: u8) -> Vec<u8> {
    vec![
        9, 4, n, 0, 1, 3, 1, protocol, 0, 9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0, 7, 5, endpoint, 3,
        8, 0, 10,
    ]
}
fn config(parts: &[Vec<u8>]) -> Vec<u8> {
    let mut v = vec![9, 2, 0, 0, parts.len() as u8, 1, 0, 0x80, 50];
    for p in parts {
        v.extend(p);
    }
    let len = v.len() as u16;
    v[2..4].copy_from_slice(&len.to_le_bytes());
    v
}
fn bindings() -> Selection {
    Selection::parse(
        device(),
        &config(&[interface(0, 1, 0x81), interface(1, 2, 0x82)]),
    )
    .unwrap()
}
const F: Focus = Focus { owner: 7, epoch: 1 };
fn session(kind: Kind) -> Session {
    let s = bindings();
    let b = match kind {
        Kind::Keyboard => s.keyboard,
        Kind::Mouse => s.mouse,
    };
    Session::new(b.unwrap(), 13, 1).unwrap()
}
fn ready(kind: Kind) -> Session {
    let mut s = session(kind);
    s.focus(F).unwrap();
    let r = s.request_protocol().unwrap();
    assert_eq!(
        r.setup(),
        [
            0x21,
            11,
            0,
            0,
            if kind == Kind::Keyboard { 0 } else { 1 },
            0,
            0,
            0
        ]
    );
    s.confirm_protocol(r, Completion::Success).unwrap();
    assert_eq!(s.take(F), Ok(Some(Event::Reset)));
    s.report(if kind == Kind::Keyboard {
        &[0; 8]
    } else {
        &[0; 3]
    })
    .unwrap();
    s
}
fn drain(s: &mut Session) -> Vec<Event> {
    let mut v = Vec::new();
    while let Some(e) = s.take(F).unwrap() {
        v.push(e);
    }
    v
}
fn key(k: u8) -> [u8; 8] {
    [0, 0, k, 0, 0, 0, 0, 0]
}
#[test]
fn valid_composite_and_unbound_interface() {
    let b = bindings();
    assert_eq!(b.keyboard.unwrap().interface(), 0);
    assert_eq!(b.mouse.unwrap().endpoint(), 0x82);
    let v = config(&[
        vec![9, 4, 0, 0, 1, 0xff, 0, 0, 0, 7, 5, 0x83, 2, 64, 0, 0],
        interface(1, 1, 0x81),
    ]);
    assert!(Selection::parse(device(), &v).unwrap().keyboard.is_some());
    let mut v = config(&[interface(0, 1, 0x81), interface(1, 2, 0x82)]);
    v.splice(9..9, [8, 11, 0, 2, 3, 0, 0, 0]);
    let n = v.len() as u16;
    v[2..4].copy_from_slice(&n.to_le_bytes());
    assert!(Selection::parse(device(), &v).is_ok());
}
#[test]
fn devices_and_speeds() {
    let mut b = [18, 1, 0, 2, 0, 0, 0, 64, 1, 0, 2, 0, 0, 1, 0, 0, 0, 1];
    for len in 0..18 {
        assert!(Device::parse(&b[..len], Speed::Full).is_err());
    }
    for (at, value) in [(0, 17), (1, 2), (7, 9), (17, 0), (17, 2), (4, 9)] {
        let old = b[at];
        b[at] = value;
        assert!(Device::parse(&b, Speed::Full).is_err());
        b[at] = old;
    }
    assert!(Device::parse(&b, Speed::Low).is_err());
    b[7] = 8;
    assert!(Device::parse(&b, Speed::Low).is_ok());
    assert!(Device::parse(&b, Speed::High).is_err());
    b[7] = 64;
    let d = Device::parse(&b, Speed::High).unwrap();
    let mut v = config(&[interface(0, 1, 0x81)]);
    *v.last_mut().unwrap() = 17;
    assert!(Selection::parse(d, &v).is_err());
    b[7] = 8;
    let d = Device::parse(&b, Speed::Low).unwrap();
    *v.last_mut().unwrap() = 9;
    assert!(Selection::parse(d, &v).is_err());
}
#[test]
fn malformed_descriptor_fields() {
    let good = config(&[interface(0, 1, 0x81)]);
    for n in 0..good.len() {
        assert!(Selection::parse(device(), &good[..n]).is_err());
    }
    for (at, value) in [
        (0, 8),
        (1, 1),
        (2, 0),
        (4, 0),
        (4, 9),
        (4, 2),
        (5, 0),
        (7, 0),
        (7, 0x81),
        (8, 251),
        (9, 0),
        (9, 1),
        (9, 255),
        (11, 8),
        (12, 1),
        (13, 0),
        (16, 7),
        (18, 8),
        (23, 0),
        (24, 0x23),
        (25, 0),
        (26, 32),
        (29, 0),
        (29, 1),
        (29, 0x91),
        (29, 0x80),
        (30, 2),
        (30, 1),
        (30, 7),
        (31, 7),
        (32, 8),
        (33, 0),
    ] {
        let mut v = good.clone();
        v[at] = value;
        assert!(
            Selection::parse(device(), &v).is_err(),
            "field {at}={value}"
        );
    }
    let mut v = good.clone();
    v.extend([2, 0x99]);
    let n = v.len() as u16;
    v[2..4].copy_from_slice(&n.to_le_bytes());
    assert!(Selection::parse(device(), &v).is_err());
    let mut v = vec![0; 4097];
    v[..9].copy_from_slice(&good[..9]);
    assert!(Selection::parse(device(), &v).is_err());
}
#[test]
fn ambiguity_and_counts() {
    for parts in [
        vec![interface(0, 1, 0x81), interface(1, 1, 0x82)],
        vec![interface(0, 1, 0x81), interface(1, 2, 0x81)],
        vec![interface(0, 1, 0x81), interface(0, 2, 0x82)],
    ] {
        assert_eq!(
            Selection::parse(device(), &config(&parts)),
            Err(Error::Ambiguous)
        );
    }
    let mut p = interface(0, 1, 0x81);
    p[4] = 2;
    assert!(Selection::parse(device(), &config(&[p])).is_err());
    let mut p = interface(0, 1, 0x81);
    p.drain(9..18);
    assert!(Selection::parse(device(), &config(&[p])).is_err());
    let mut p = interface(0, 1, 0x81);
    p.splice(18..18, [9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0]);
    assert!(Selection::parse(device(), &config(&[p])).is_err());
}
#[test]
fn no_boot_data_before_transport_success() {
    let mut s = session(Kind::Keyboard);
    s.focus(F).unwrap();
    assert_eq!(s.report(&key(4)), Err(Error::Protocol));
    let r = s.request_protocol().unwrap();
    assert_eq!(s.report(&key(4)), Err(Error::Protocol));
    assert!(s.request_protocol().is_err());
    for outcome in [Completion::Stall, Completion::Timeout, Completion::Failed] {
        let mut x = session(Kind::Keyboard);
        x.focus(F).unwrap();
        let q = x.request_protocol().unwrap();
        assert_eq!(x.confirm_protocol(q, outcome), Err(Error::Protocol));
        assert!(x.report(&key(4)).is_err());
        assert!(x.request_protocol().is_err());
    }
    s.confirm_protocol(r, Completion::Success).unwrap();
    assert!(s.confirm_protocol(r, Completion::Success).is_err());
}
#[test]
fn stale_foreign_confirmations() {
    let mut s = session(Kind::Keyboard);
    let old = s.request_protocol().unwrap();
    s.reset(2).unwrap();
    let r = s.request_protocol().unwrap();
    assert_eq!(
        s.confirm_protocol(old, Completion::Success),
        Err(Error::Stale)
    );
    s.confirm_protocol(r, Completion::Success).unwrap();
    let mut other = Session::new(bindings().keyboard.unwrap(), 99, 1).unwrap();
    other.request_protocol().unwrap();
    assert_eq!(
        other.confirm_protocol(old, Completion::Success),
        Err(Error::Stale)
    );
    let mut mouse = session(Kind::Mouse);
    mouse.request_protocol().unwrap();
    assert_eq!(
        mouse.confirm_protocol(old, Completion::Success),
        Err(Error::Stale)
    );
    assert!(s.reset(2).is_err());
    s.disconnect();
    assert!(s.report(&[0; 8]).is_err());
}
#[test]
fn keyboard_transitions_and_duplicates() {
    let mut s = ready(Kind::Keyboard);
    let mut b = key(4);
    b[0] = 2;
    s.report(&b).unwrap();
    assert_eq!(
        drain(&mut s),
        vec![
            Event::Key {
                usage: 0xe1,
                down: true
            },
            Event::Key {
                usage: 4,
                down: true
            }
        ]
    );
    s.report(&b).unwrap();
    assert!(drain(&mut s).is_empty());
    b = key(5);
    s.report(&b).unwrap();
    assert_eq!(
        drain(&mut s),
        vec![
            Event::Key {
                usage: 0xe1,
                down: false
            },
            Event::Key {
                usage: 4,
                down: false
            },
            Event::Key {
                usage: 5,
                down: true
            }
        ]
    );
    s.report(&[0; 8]).unwrap();
    assert_eq!(
        drain(&mut s),
        vec![Event::Key {
            usage: 5,
            down: false
        }]
    );
    s.report(&[255, 0, 4, 5, 6, 7, 8, 9]).unwrap();
    drain(&mut s);
    s.report(&[0, 0, 10, 11, 12, 13, 14, 15]).unwrap();
    assert_eq!(drain(&mut s).len(), 20);
}
#[test]
fn rollover_bad_reports_release_and_rearm() {
    for bad in [
        vec![0; 7],
        vec![0; 9],
        vec![0, 1, 0, 0, 0, 0, 0, 0],
        key(1).to_vec(),
        key(2).to_vec(),
        key(3).to_vec(),
        key(0xe0).to_vec(),
        vec![0, 0, 4, 4, 0, 0, 0, 0],
    ] {
        let mut s = ready(Kind::Keyboard);
        s.report(&key(4)).unwrap();
        drain(&mut s);
        assert_eq!(s.report(&bad), Err(Error::Report));
        assert_eq!(drain(&mut s), vec![Event::Reset]);
        s.report(&key(5)).unwrap();
        assert!(drain(&mut s).is_empty());
        s.report(&[0; 8]).unwrap();
        s.report(&key(5)).unwrap();
        assert_eq!(
            drain(&mut s),
            vec![Event::Key {
                usage: 5,
                down: true
            }]
        );
    }
}
#[test]
fn keyboard_owner_epoch_and_disconnect() {
    let mut s = ready(Kind::Keyboard);
    s.report(&key(4)).unwrap();
    let next = Focus { owner: 8, epoch: 2 };
    assert!(s.take(next).is_err());
    s.focus(next).unwrap();
    assert!(s.take(F).is_err());
    assert_eq!(s.take(next), Ok(Some(Event::Reset)));
    assert_eq!(s.take(next), Ok(None));
    assert!(s.focus(F).is_err());
    s.report(&key(4)).unwrap();
    assert_eq!(s.take(next), Ok(None));
    s.disconnect();
    assert_eq!(s.take(next), Ok(Some(Event::Reset)));
    assert!(s.report(&[0; 8]).is_err());
    s.reset(2).unwrap();
    assert!(s.report(&[0; 8]).is_err());
    s.revoke(3).unwrap();
    assert!(s.take(next).is_err());
}
#[test]
fn queue_saturation_is_atomic_and_reset_cannot_be_lost() {
    let mut s = ready(Kind::Keyboard);
    for _ in 0..16 {
        s.report(&key(4)).unwrap();
        s.report(&[0; 8]).unwrap();
    }
    assert_eq!(s.report(&key(5)), Err(Error::Overflow));
    assert_eq!(drain(&mut s), vec![Event::Reset]);
    s.report(&key(5)).unwrap();
    assert!(drain(&mut s).is_empty());
    s.report(&[0; 8]).unwrap();
    s.report(&key(6)).unwrap();
    assert_eq!(
        drain(&mut s),
        vec![Event::Key {
            usage: 6,
            down: true
        }]
    );
}
#[test]
fn mouse_direction_buttons_and_relative_repetition() {
    let mut s = ready(Kind::Mouse);
    s.report(&[1, 128, 127]).unwrap();
    assert_eq!(
        drain(&mut s),
        vec![Event::Pointer {
            dx: -128,
            dy: 127,
            buttons: 1
        }]
    );
    s.report(&[1, 128, 127]).unwrap();
    assert_eq!(drain(&mut s).len(), 1);
    s.report(&[0, 0, 0, 255]).unwrap();
    assert_eq!(
        drain(&mut s),
        vec![Event::Pointer {
            dx: 0,
            dy: 0,
            buttons: 0
        }]
    );
    s.report(&[0, 0, 0]).unwrap();
    assert!(drain(&mut s).is_empty());
    for bad in [&[0, 0][..], &[8, 0, 0][..], &[0; 9][..]] {
        assert_eq!(s.report(bad), Err(Error::Report));
        assert_eq!(drain(&mut s), vec![Event::Reset]);
        s.report(&[0, 0, 0]).unwrap();
    }
    for _ in 0..32 {
        s.report(&[1, 1, 1]).unwrap();
    }
    assert_eq!(s.report(&[1, 1, 1]), Err(Error::Overflow));
    assert_eq!(drain(&mut s), vec![Event::Reset]);
    s.report(&[1, 1, 1]).unwrap();
    assert!(drain(&mut s).is_empty());
    s.report(&[0, 0, 0]).unwrap();
    s.report(&[1, 1, 1]).unwrap();
    assert_eq!(drain(&mut s).len(), 1);
    s.disconnect();
    assert_eq!(drain(&mut s), vec![Event::Reset]);
}
#[test]
fn bounded_hostile_inputs_and_independent_key_oracle() {
    let mut seed = 0x12345678u32;
    let good = config(&[interface(0, 1, 0x81)]);
    for _ in 0..50000 {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        let mut v = good.clone();
        let i = seed as usize % v.len();
        v[i] = (seed >> 16) as u8;
        let _ = Selection::parse(device(), &v);
        let _ = Device::parse(&v[..18], Speed::Full);
    }
    let mut s = ready(Kind::Keyboard);
    let mut old = 0;
    for i in 0..10000 {
        let k = if i % 3 == 0 { 0 } else { 4 + (i % 26) as u8 };
        s.report(&key(k)).unwrap();
        let mut want = vec![];
        if old != k {
            if old != 0 {
                want.push(Event::Key {
                    usage: old,
                    down: false,
                });
            }
            if k != 0 {
                want.push(Event::Key {
                    usage: k,
                    down: true,
                });
            }
        }
        assert_eq!(drain(&mut s), want);
        old = k;
    }
    println!("50000 hostile descriptor mutations; 10000 independent keyboard transitions PASS");
}
#[test]
fn resources() {
    println!(
        "Session={} Event={} descriptor_buffer={} two_sessions_plus_buffer={}",
        std::mem::size_of::<Session>(),
        std::mem::size_of::<Event>(),
        MAX_CONFIGURATION,
        2 * std::mem::size_of::<Session>() + MAX_CONFIGURATION
    );
    assert!(2 * std::mem::size_of::<Session>() + MAX_CONFIGURATION <= 8192);
}

#[test]
fn bounded_interface_numbers_and_mouse_oracle() {
    let mut v = config(&[interface(0, 1, 0x81)]);
    v[11] = 7;
    assert!(Selection::parse(device(), &v).is_err());
    let parts: Vec<_> = (0..8)
        .map(|i| {
            if i < 2 {
                interface(i, 1 + i, 0x81 + i)
            } else {
                vec![9, 4, i, 0, 0, 0xff, 0, 0, 0]
            }
        })
        .collect();
    assert!(Selection::parse(device(), &config(&parts)).is_ok());
    let mut s = ready(Kind::Mouse);
    for x in 0..=255u8 {
        for y in 0..=255u8 {
            s.report(&[0, x, y]).unwrap();
            let got = drain(&mut s);
            if x == 0 && y == 0 {
                assert!(got.is_empty());
            } else {
                let signed = |v: u8| {
                    if v < 128 {
                        i16::from(v)
                    } else {
                        i16::from(v) - 256
                    }
                };
                assert_eq!(
                    got,
                    vec![Event::Pointer {
                        dx: signed(x),
                        dy: signed(y),
                        buttons: 0
                    }]
                );
            }
        }
    }
    println!("65536 independent mouse motion cases PASS");
}
