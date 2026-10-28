#[path = "../../kernel/rust/input/ps2_pointer.rs"]
mod decoder;
use decoder::{Decoder, Step};

#[test]
fn layout_and_boundaries() {
    assert_eq!(core::mem::size_of::<Decoder>(), 5);
    let mut d = Decoder::default();
    assert_eq!(d.push(0), Step::Rejected);
    assert_eq!(d.push(0x39), Step::Pending);
    assert_eq!(d.push(0), Step::Pending);
    assert_eq!(d.push(255), Step::Complete(decoder::Packet { dx:-256, dy:-1, buttons:1 }));
    assert_eq!(d.push(0xc8), Step::Pending);
    assert_eq!(d.push(8), Step::Pending);
    assert_eq!(d.push(8), Step::Rejected);
    assert_eq!(d.used, 0);
}

#[test]
fn exact_idle_and_corrupt_state() {
    for used in 1..=2 {
        let mut d = Decoder { used, ..Decoder::default() };
        for i in 1..16 { assert!(!d.poll_idle()); assert_eq!(d.idle, i); }
        assert!(d.poll_idle()); assert_eq!((d.used,d.idle), (0,0));
    }
    for used in 3..=255 {
        let mut d = Decoder { used, ..Decoder::default() };
        assert_eq!(d.push(8), Step::Rejected);
        assert_eq!((d.used,d.idle), (0,0));
    }
    for idle in 16..=255 {
        let mut d = Decoder { used:1,idle,..Decoder::default() };
        assert!(d.poll_idle()); assert_eq!((d.used,d.idle),(0,0));
    }
}
