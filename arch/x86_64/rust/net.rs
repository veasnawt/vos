//! R3-N1 FFI boundary for the Rust ARP/IPv4/UDP core. C keeps the public ABI in
//! net_arp.c/net_ipv4.c/net_udp.c and forwards here; virtio-net, DMA, TCP and
//! boot glue stay C. Contract (net_arp.h): inputs may alias each other and the
//! packet output, but not the context; the length output is separate from the
//! packet output. Inputs that the C copies first are copied first; generated
//! frames are built in locals sized like the C locals and copied out last.
//! Rust-only refusals (no access, no state change) cover cases the accepted C
//! contract leaves undefined: a misaligned context, or a frame/data range that
//! overlaps a context the call mutates or borrows.

// ---------------- stage 1: checksums ----------------
#[path = "../../../kernel/rust/net/checksum.rs"]
pub mod checksum;
#[path = "../../../kernel/rust/net/context.rs"]
pub mod context;
use core::slice;

/// # Safety
/// `bytes` is readable for `length` bytes (caller bounds length <= 1500).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_checksum(bytes: *const u8, length: usize) -> u16 {
    if length == 0 {
        return checksum::checksum(&[]);
    }
    if bytes.is_null() {
        return 0xffff; // contract violation: fail verification safely
    }
    checksum::checksum(unsafe { slice::from_raw_parts(bytes, length) })
}
/// # Safety
/// `source`/`dest` are 4 readable bytes; `bytes` is readable for `n` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_rust_net_transport_checksum(source: *const u8, dest: *const u8, protocol: u8, bytes: *const u8, n: usize) -> u16 {
    if source.is_null() || dest.is_null() || (bytes.is_null() && n != 0) {
        return 0xffff;
    }
    // SAFETY: pure reads of caller-owned 4-byte addresses and n payload bytes.
    let (s, d) = unsafe { (&*source.cast::<[u8; 4]>(), &*dest.cast::<[u8; 4]>()) };
    let b: &[u8] = if n == 0 { &[] } else { unsafe { slice::from_raw_parts(bytes, n) } };
    checksum::transport_checksum(s, d, protocol, b)
}
