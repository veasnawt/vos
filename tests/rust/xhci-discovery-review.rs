#![forbid(unsafe_code)]
use vos_xhci::xhci::{
    capabilities::Capabilities,
    discovery::{Report, Status, decode_caps},
};
#[path = "xhci_fake.rs"]
pub mod fixture;

fn words(bytes: &[u8]) -> [u32; 7] {
    core::array::from_fn(|n| u32::from_le_bytes(bytes[n * 4..n * 4 + 4].try_into().unwrap()))
}
#[test]
fn discovery_keeps_versions_that_activation_rejects() {
    for version in [0x110, 0x120] {
        let (mut b, f) = fixture::capabilities();
        fixture::put(&mut b, 0, (version << 16) | 0x40);
        assert_eq!(
            decode_caps(Some(words(&b)), &mut Report::default()),
            Status::Ok
        );
        assert!(Capabilities::parse(&b, f).is_err());
    }
}
#[test]
fn discovery_keeps_dword_alignment_and_larger_port_counts() {
    for n in 0..2 {
        let (mut b, f) = fixture::capabilities();
        if n == 0 {
            fixture::put(&mut b, 0, 0x1000024);
        } else {
            fixture::put(&mut b, 4, (17 << 24) | (16 << 8) | 64);
        }
        assert_eq!(
            decode_caps(Some(words(&b)), &mut Report::default()),
            Status::Ok
        );
        assert!(Capabilities::parse(&b, f).is_err());
    }
}
#[test]
fn discovery_does_not_pretend_to_qualify_an_operational_aperture() {
    let (mut b, f) = fixture::capabilities();
    fixture::put(&mut b, 20, 0x2000000);
    fixture::put(&mut b, 24, 0x1000000);
    assert_eq!(
        decode_caps(Some(words(&b)), &mut Report::default()),
        Status::Ok
    );
    assert!(Capabilities::parse(&b, f).is_err());
}
#[test]
fn malformed_shared_layout_is_rejected_by_both_profiles() {
    let (mut b, f) = fixture::capabilities();
    fixture::put(&mut b, 20, 0x1000);
    fixture::put(&mut b, 24, 0x1000);
    assert_eq!(
        decode_caps(Some(words(&b)), &mut Report::default()),
        Status::Invalid
    );
    assert!(Capabilities::parse(&b, f).is_err());
}
