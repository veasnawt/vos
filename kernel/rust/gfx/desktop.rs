//! R3-G1: the pure decisions of the N6 desktop scene, exactly as the accepted C
//! (`arch/x86_64/desktop.c` at main 2c0890a3, frozen as tests/reference/desktop-r3g1.c).
//! Nothing here touches the framebuffer, a surface or any device; rendering
//! (`desktop_bar`, `desktop_paint`) and framebuffer ownership stay in C.
#![forbid(unsafe_code)]

/// The boot option that requests the desktop scene.
pub const TOKEN: &[u8] = b"vos.desktop=1";
/// `desktop_compute`'s minimum mode, from the accepted C.
pub const MIN_WIDTH: u32 = 640;
pub const MIN_HEIGHT: u32 = 480;
/// `BOOT_FB_MAX_DIM` (bootinfo.h).
pub const MAX_DIM: u32 = 8192;

/// `struct desktop_layout` field values, in declaration order.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Layout {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
    pub text_x: u32,
    pub text_y: u32,
    pub text_w: u32,
    pub text_h: u32,
}

/// The only field separators of the command line.
pub fn separator(byte: u8) -> bool {
    byte == b' ' || byte == b'\t'
}

/// `desktop_requested`: true when a field of the command line, separated only
/// by spaces and tabs, is exactly `vos.desktop=1`; a prefix, a suffix or a
/// substring never matches. `at(i)` returns byte `i` of the NUL-terminated
/// line. This is the accepted C's scan, so it calls `at(i)` only after it has
/// seen a non-NUL byte at every index below `i` (the reads the C makes), and
/// stops at the NUL or at the first matching field.
#[inline(always)]
pub fn requested_by(mut at: impl FnMut(usize) -> u8) -> bool {
    let mut p = 0usize;
    loop {
        let mut c = at(p);
        if c == 0 {
            return false;
        }
        while separator(c) {
            p = p.wrapping_add(1);
            c = at(p);
        }
        let mut n = 0usize;
        while n < TOKEN.len() && at(p.wrapping_add(n)) == TOKEN[n] {
            n += 1;
        }
        if n == TOKEN.len() {
            let end = at(p.wrapping_add(n));
            if end == 0 || separator(end) {
                return true;
            }
        }
        loop {
            let c = at(p);
            if c == 0 || separator(c) {
                break;
            }
            p = p.wrapping_add(1);
        }
    }
}

/// `desktop_compute`: the scene layout for a `width` x `height` framebuffer, or
/// `None` below 640x480 or above `BOOT_FB_MAX_DIM` in either dimension. The
/// guards make every subtraction exact (width >= 640, height >= 480).
pub fn compute(width: u32, height: u32) -> Option<Layout> {
    // Two unsigned range tests, as the C compiles: [640, 8192] and [480, 8192].
    if width.wrapping_sub(MIN_WIDTH) > MAX_DIM - MIN_WIDTH {
        return None;
    }
    if height.wrapping_sub(MIN_HEIGHT) > MAX_DIM - MIN_HEIGHT {
        return None;
    }
    Some(Layout {
        x: 24,
        y: 44,
        w: width.wrapping_sub(48),
        h: height.wrapping_sub(104),
        text_x: 36,
        text_y: 84,
        text_w: width.wrapping_sub(72),
        text_h: height.wrapping_sub(156),
    })
}
