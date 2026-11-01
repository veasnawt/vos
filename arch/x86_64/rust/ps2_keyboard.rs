//! Private C ABI for the pure Set-1 decoder; hardware stays in keyboard.c.
#[path = "../../../kernel/rust/input/ps2_keyboard.rs"]
pub mod decoder;
use decoder::Decoder;
use core::mem::{align_of, offset_of, size_of};

const _: () = {
    assert!(size_of::<Decoder>() == 7 && align_of::<Decoder>() == 1);
    assert!(offset_of!(Decoder, shift) == 0);
    assert!(offset_of!(Decoder, ctrl) == 1);
    assert!(offset_of!(Decoder, alt) == 2);
    assert!(offset_of!(Decoder, caps) == 3);
    assert!(offset_of!(Decoder, caps_down) == 4);
    assert!(offset_of!(Decoder, extended) == 5);
    assert!(offset_of!(Decoder, pause_left) == 6);
};

/// # Safety
/// d must identify seven mapped, readable/writable bytes of exclusively owned
/// decoder storage for this synchronous call. No callback, yield or retained
/// borrow occurs. All byte encodings are valid; the type has no padding. Null
/// and wrapping ranges return no character without accessing storage. Mapping
/// validity cannot be proved from an arbitrary raw address and is the caller's
/// responsibility, as for the unchanged C interface in keyboard.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn keyboard_decode(d: *mut Decoder, scan: u8) -> core::ffi::c_char {
    if d.is_null() || (d as usize).checked_add(size_of::<Decoder>()).is_none() {
        return 0;
    }
    // SAFETY: caller guarantees mapped exclusive storage; align=1, no padding,
    // and every possible field byte is valid. Snapshot before updating fields.
    let mut state = unsafe { core::ptr::read(d) };
    let c = state.decode(scan);
    // SAFETY: same caller-owned seven-byte object, no aliases or callbacks used.
    unsafe { core::ptr::write(d, state) };
    c as core::ffi::c_char
}
