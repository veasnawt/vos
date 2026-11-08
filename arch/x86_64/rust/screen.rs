//! R3-G2: the geometry and bounds decisions of screen.c are Rust (kernel/rust/gfx/screen.rs). `screen_choose` keeps its
//! screen.h C ABI; the other decisions are scalar `vos_rust_screen_*` entry points declared in rust_kernel.h. screen.c
//! keeps every framebuffer/VGA read and write, rendering, the cursor lifecycle and underlay, the character shadow and
//! device ownership. The accepted C is frozen as tests/reference/screen-r3g2.c and is not linked into the kernel.
//!
//! The only unsafe code is `screen_choose`'s write of its static reason string to the caller's `const char **`.
#[path = "../../../kernel/rust/gfx/screen.rs"]
mod screen_core;
use self::screen_core as s;

// The reason strings, byte for byte as in the accepted C, each NUL-terminated.
static R_FRAMEBUFFER: &[u8] = b"framebuffer\0";
static R_EGA_TEXT: &[u8] = b"VGA text (the loader reports 80x25 EGA text)\0";
static R_UNUSABLE: &[u8] = b"unusable framebuffer information\0";
static R_UEFI: &[u8] = b"UEFI boot without framebuffer information\0";
static R_BDA_TEXT: &[u8] = b"VGA text (BIOS data area: 80-column text mode)\0";
static R_NO_TEXT: &[u8] = b"no framebuffer information and the BIOS reports no text mode\0";

/// `int screen_choose(uint32_t fb_tags, const char *fb_err, int fb_kind, int efi, uint8_t bda_mode, uint16_t bda_cols,
/// const char **out)`: the screen kind; `*out` is the reason, which for a refused tag is the loader's own `fb_err`
/// pointer when it gave one, exactly as in the C.
///
/// # Safety
/// `out` is writable, aligned storage for one pointer (the C dereferences it unconditionally too). `fb_err` is only
/// compared with NULL and passed back, never read.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn screen_choose(fb_tags: u32, fb_err: *const u8, fb_kind: i32, efi: i32, bda_mode: u8, bda_cols: u16,
                                       out: *mut *const u8) -> i32 {
    let (kind, reason) = s::choose(fb_tags, !fb_err.is_null(), fb_kind, efi != 0, bda_mode, bda_cols);
    let text = match reason {
        s::Reason::Framebuffer => R_FRAMEBUFFER.as_ptr(),
        s::Reason::EgaText => R_EGA_TEXT.as_ptr(),
        s::Reason::Refused => if fb_err.is_null() { R_UNUSABLE.as_ptr() } else { fb_err },
        s::Reason::UefiNone => R_UEFI.as_ptr(),
        s::Reason::BdaText => R_BDA_TEXT.as_ptr(),
        s::Reason::NoText => R_NO_TEXT.as_ptr(),
    };
    // SAFETY: `out` is writable and aligned (contract above).
    unsafe { out.write(text) };
    kind
}

// Scalar decisions: no pointers cross these entry points.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_visible(width: u32, height: u32, x: u32, y: u32) -> i32 { s::visible(width, height, x, y) as i32 }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_span(len: u32, limit: u32, start: u32) -> u32 { s::span(len, limit, start) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_glyph_index(width: u32, height: u32, x: u32, y: u32, c: u8) -> i32 {
    s::glyph_index(width, height, x, y, c)
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_overlaps(x: u32, y: u32, w: u32, h: u32, cx: u32, cy: u32) -> i32 {
    s::overlaps(x, y, w, h, cx, cy) as i32
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_cursor_extent(limit: u32, pos: u32) -> u32 { s::cursor_extent(limit, pos) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_cell_x(text_x: u32, cx: u32) -> u32 { s::cell_x(text_x, cx) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_row_y(log_y: u32, r: u32) -> u32 { s::row_y(log_y, r) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_cell_y(log_y: u32, cy: u32) -> u32 { s::cell_y(log_y, cy) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_put_action(c: u8, cx: u32, cols: u32) -> u32 { s::put_action(c, cx, cols) }
/// The next input row, or -1 when the input area must scroll.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_input_next_row(cy: u32, rows: u32) -> i64 {
    match s::input_next_row(cy, rows) { Some(r) => r as i64, None => -1 }
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_ring_next(cy: u32, rows: u32) -> u32 { s::ring_next(cy, rows) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_ring_gap(cy: u32, rows: u32) -> u32 { s::ring_gap(cy, rows) }
/// `(cx << 32) | cy` after an erase, or `u64::MAX` when nothing is erased. For `rows >= 1` a real result never equals
/// `u64::MAX`: a new cx of `u32::MAX` arises only at column 0, where the new cy is below `rows`.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_erase(input: i32, cx: u32, cy: u32, cols: u32, rows: u32) -> u64 {
    match s::erase(input != 0, cx, cy, cols, rows) { Some((x, y)) => (x as u64) << 32 | y as u64, None => u64::MAX }
}
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_fb_cols(width: u32) -> u32 { s::fb_cols(width) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_fb_rows(height: u32, log_y: u32) -> u32 { s::fb_rows(height, log_y) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_text_cols(text_w: u32) -> u32 { s::text_cols(text_w) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_text_rows(text_h: u32) -> u32 { s::text_rows(text_h) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_cells_ok(cols: u32, rows: u32) -> i32 { s::cells_ok(cols, rows) as i32 }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_input_cols(cols: u32) -> u32 { s::input_cols(cols) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_input_rows(rows: u32) -> u32 { s::input_rows(rows) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_gui_room(clear_w: u32, clear_bottom: u32, log_y: u32) -> i32 { s::gui_room(clear_w, clear_bottom, log_y) as i32 }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_gui_size_ok(w: u32, h: u32) -> i32 { s::gui_size_ok(w, h) as i32 }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_gui_x(clear_x: u32, clear_w: u32, w: u32) -> u32 { s::gui_x(clear_x, clear_w, w) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_gui_y(log_y: u32, clear_bottom: u32, h: u32) -> u32 { s::gui_y(log_y, clear_bottom, h) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_pointer_home(origin: u32, extent: u32) -> u32 { s::pointer_home(origin, extent) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_pointer_axis(pos: u32, delta: i32, limit: u32) -> u32 { s::pointer_axis(pos, delta, limit) }
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_screen_pointer_state(cx: u32, cy: u32, gx: u32, gy: u32, gw: u32, gh: u32) -> u32 {
    s::pointer_state(cx, cy, gx, gy, gw, gh)
}
