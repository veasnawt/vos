//! Existing pointer.h ABI. Storage and hardware/session ownership remain in C.
#[path = "../../../kernel/rust/input/ps2_pointer.rs"]
mod core;
use self::core::{Decoder, Packet, Step};

const _: () = {
    assert!(::core::mem::size_of::<Decoder>() == 5);
    assert!(::core::mem::align_of::<Decoder>() == 1);
    assert!(::core::mem::offset_of!(Decoder, bytes) == 0);
    assert!(::core::mem::offset_of!(Decoder, used) == 3);
    assert!(::core::mem::offset_of!(Decoder, idle) == 4);
    assert!(::core::mem::size_of::<Packet>() == 6);
    assert!(::core::mem::align_of::<Packet>() == 2);
    assert!(::core::mem::offset_of!(Packet, dx) == 0);
    assert!(::core::mem::offset_of!(Packet, dy) == 2);
    assert!(::core::mem::offset_of!(Packet, buttons) == 4);
};

fn overlap(state: *mut Decoder, out: *mut Packet) -> bool {
    let a = state as usize;
    let b = out as usize;
    match (a.checked_add(5), b.checked_add(6)) {
        (Some(end_a), Some(end_b)) => a < end_b && b < end_a,
        _ => true,
    }
}

/// # Safety
/// state must point to initialized, exclusively borrowed five-byte C decoder
/// storage. out must point to six writable bytes, aligned for pointer_packet;
/// it need not be initialized. Regions must be live, disjoint kernel storage.
/// Null/alignment/overlap checks cannot establish arbitrary pointer validity.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pointer_decode(state: *mut Decoder, byte: u8, out: *mut Packet) -> i32 {
    if state.is_null() || out.is_null() || !(out as usize).is_multiple_of(2) || overlap(state, out) {
        return -1;
    }
    // SAFETY: the C caller owns initialized decoder storage exclusively for this
    // synchronous call. All bit patterns are valid u8 fields; push checks used.
    let step = unsafe { (&mut *state).push(byte) };
    match step {
        Step::Rejected => -1,
        Step::Pending => 0,
        Step::Complete(packet) => {
            // SAFETY: caller supplies writable, aligned, nonaliasing output.
            // Write individual fields: never borrow/read uninitialized output
            // or copy unspecified Rust padding into the C packet's padding.
            unsafe {
                ::core::ptr::addr_of_mut!((*out).dx).write(packet.dx);
                ::core::ptr::addr_of_mut!((*out).dy).write(packet.dy);
                ::core::ptr::addr_of_mut!((*out).buttons).write(packet.buttons);
            }
            1
        }
    }
}

/// # Safety
/// state points to initialized, live, exclusively borrowed C decoder storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pointer_idle(state: *mut Decoder) -> i32 {
    if state.is_null() {
        return 0;
    }
    // SAFETY: same five-byte state contract, without a packet output parameter.
    unsafe { (&mut *state).poll_idle() as i32 }
}
