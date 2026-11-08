//! R3-G2: the pure geometry and bounds decisions of the early-boot screen console, exactly as the accepted C
//! (`arch/x86_64/screen.c` at main 0dbf404f, frozen as tests/reference/screen-r3g2.c). Every function takes and returns
//! scalars with the C's unsigned 32-bit (modulo 2^32) arithmetic. Framebuffer and VGA reads and writes, rendering,
//! the cursor lifecycle and its underlay, the character shadow and all device ownership stay in C.
#![forbid(unsafe_code)]

pub const MARGIN: u32 = 16; // SCREEN_MARGIN
pub const LINE_H: u32 = 18; // SCREEN_LINE_H
pub const GLYPH_W: u32 = 8;
pub const GLYPH_H: u32 = 16;
pub const FONT_FIRST: u32 = 0x20; // FONT8X16_FIRST
pub const FONT_GLYPHS: u32 = 95; // FONT8X16_GLYPHS
pub const CURSOR: u32 = 32; // CURSOR_W == CURSOR_H
pub const INPUT_COLS: u32 = 80;
pub const INPUT_ROWS: u32 = 25;
pub const GUI_MAX_W: u32 = 256;
pub const GUI_MAX_H: u32 = 160;
pub const INSIDE: u32 = 0x20000; // VOS_GUI_POINTER_INSIDE

// ---- selection (screen_choose) ----
pub const SCREEN_NONE: i32 = 0;
pub const SCREEN_VGA_TEXT: i32 = 1;
pub const SCREEN_FB: i32 = 2;
pub const BOOT_FB_BGRX32: i32 = 1;
pub const BOOT_FB_EGA_TEXT: i32 = 2;

/// Which reason string `screen_choose` reports.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Reason {
    Framebuffer,
    EgaText,
    /// The loader's own error, or "unusable framebuffer information" when it gave none.
    Refused,
    UefiNone,
    BdaText,
    NoText,
}

/// `screen_choose`, in the C's order: a valid BGRX32 tag, a valid EGA text tag, any tag at all (refused: a graphics
/// mode may be active), a UEFI boot, then the BIOS data area's 80-column text mode 3 or 7.
pub fn choose(fb_tags: u32, has_error: bool, fb_kind: i32, efi: bool, bda_mode: u8, bda_cols: u16) -> (i32, Reason) {
    if !has_error && fb_kind == BOOT_FB_BGRX32 {
        return (SCREEN_FB, Reason::Framebuffer);
    }
    if !has_error && fb_kind == BOOT_FB_EGA_TEXT {
        return (SCREEN_VGA_TEXT, Reason::EgaText);
    }
    if fb_tags != 0 {
        return (SCREEN_NONE, Reason::Refused);
    }
    if efi {
        return (SCREEN_NONE, Reason::UefiNone);
    }
    if (bda_mode == 3 || bda_mode == 7) && bda_cols == 80 {
        return (SCREEN_VGA_TEXT, Reason::BdaText);
    }
    (SCREEN_NONE, Reason::NoText)
}

// ---- rectangles and glyphs ----
/// `screen_fill`: the rectangle starts inside the surface.
pub fn visible(width: u32, height: u32, x: u32, y: u32) -> bool {
    x < width && y < height
}

/// `screen_fill`: a length clipped to the room left after `start` (callers ensure `start < limit`).
pub fn span(len: u32, limit: u32, start: u32) -> u32 {
    let room = limit.wrapping_sub(start);
    if len > room { room } else { len }
}

/// `screen_glyph`: the font index for `c`, or -1 when the 8x16 cell at (x, y) does not fit the surface.
pub fn glyph_index(width: u32, height: u32, x: u32, y: u32, c: u8) -> i32 {
    if x > width || width - x < GLYPH_W || y > height || height - y < GLYPH_H {
        return -1;
    }
    let c = c as u32;
    (if c >= FONT_FIRST && c < FONT_FIRST + FONT_GLYPHS { c - FONT_FIRST } else { b'?' as u32 - FONT_FIRST }) as i32
}

/// `cursor_damage`: the rectangle intersects the 32x32 cursor canvas at (cx, cy), in 64-bit arithmetic.
pub fn overlaps(x: u32, y: u32, w: u32, h: u32, cx: u32, cy: u32) -> bool {
    let (x, y, w, h, cx, cy) = (x as u64, y as u64, w as u64, h as u64, cx as u64, cy as u64);
    x < cx + CURSOR as u64 && x + w > cx && y < cy + CURSOR as u64 && y + h > cy
}

/// `cursor_hide`/`cursor_show`: rows (or columns) of the cursor canvas inside the surface at `pos`.
pub fn cursor_extent(limit: u32, pos: u32) -> u32 {
    let room = limit.wrapping_sub(pos);
    if room < CURSOR { room } else { CURSOR }
}

// ---- text cells ----
/// The left edge of text column `cx`.
pub fn cell_x(text_x: u32, cx: u32) -> u32 {
    text_x.wrapping_add(cx.wrapping_mul(GLYPH_W))
}

/// The top of text row `r` (the cleared line).
pub fn row_y(log_y: u32, r: u32) -> u32 {
    log_y.wrapping_add(r.wrapping_mul(LINE_H))
}

/// The glyph origin in text row `cy`: one pixel below the row top.
pub fn cell_y(log_y: u32, cy: u32) -> u32 {
    row_y(log_y, cy).wrapping_add(1)
}

pub const PUT_IGNORE: u32 = 0;
pub const PUT_NEWLINE: u32 = 1;
pub const PUT_DRAW: u32 = 2;
pub const PUT_WRAP_DRAW: u32 = 3;

/// `put`: a newline, an ignored non-printable byte, or a printable byte drawn at `cx` (after a newline when the
/// row is full).
pub fn put_action(c: u8, cx: u32, cols: u32) -> u32 {
    if c == b'\n' {
        PUT_NEWLINE
    } else if c < 0x20 || c > 0x7e {
        PUT_IGNORE
    } else if cx >= cols {
        PUT_WRAP_DRAW
    } else {
        PUT_DRAW
    }
}

/// `newline` in input mode: the next row, or `None` when the area is full and must scroll.
pub fn input_next_row(cy: u32, rows: u32) -> Option<u32> {
    let next = cy.wrapping_add(1);
    if next < rows { Some(next) } else { None }
}

/// `newline` in the circular log: the next row, wrapping to the top.
pub fn ring_next(cy: u32, rows: u32) -> u32 {
    let next = cy.wrapping_add(1);
    if next >= rows { 0 } else { next }
}

/// `newline` in the circular log: the blank gap row after the newest line. `rows` is at least 2 whenever a screen
/// exists (the C divides by it); 0 is answered with row 0 instead of faulting.
pub fn ring_gap(cy: u32, rows: u32) -> u32 {
    cy.wrapping_add(1).checked_rem(rows).unwrap_or(0)
}

/// `screen_erase_previous`: the new (cx, cy), or `None` at the very start of the input area. At column 0 it steps
/// back to the end of the previous row (circularly). `rows` 0 is answered as for `ring_gap`.
pub fn erase(input: bool, cx: u32, cy: u32, cols: u32, rows: u32) -> Option<(u32, u32)> {
    let (mut cx, mut cy) = (cx, cy);
    if cx == 0 {
        if input && cy == 0 {
            return None;
        }
        cy = cy.wrapping_add(rows).wrapping_sub(1).checked_rem(rows).unwrap_or(0);
        cx = cols;
    }
    Some((cx.wrapping_sub(1), cy))
}

// ---- console geometry ----
/// `screen_init`: text columns of a plain framebuffer console.
pub fn fb_cols(width: u32) -> u32 {
    width.wrapping_sub(2 * MARGIN) / GLYPH_W
}

/// `screen_init`: text rows of a plain framebuffer console below `log_y`.
pub fn fb_rows(height: u32, log_y: u32) -> u32 {
    height.wrapping_sub(log_y).wrapping_sub(MARGIN) / LINE_H
}

/// `screen_desktop_begin`: text columns and rows of the desktop's text area.
pub fn text_cols(text_w: u32) -> u32 {
    text_w / GLYPH_W
}
pub fn text_rows(text_h: u32) -> u32 {
    text_h / LINE_H
}

/// `screen_init`: a console needs at least 8 columns and 2 rows.
pub fn cells_ok(cols: u32, rows: u32) -> bool {
    !(rows < 2 || cols < 8)
}

/// `screen_begin_input`: the input area is at most 80x25 cells.
pub fn input_cols(cols: u32) -> u32 {
    if cols > INPUT_COLS { INPUT_COLS } else { cols }
}
pub fn input_rows(rows: u32) -> u32 {
    if rows > INPUT_ROWS { INPUT_ROWS } else { rows }
}

// ---- GUI sink and pointer ----
/// `screen_gui_available`: the desktop text area can hold a 256x160 surface.
pub fn gui_room(clear_w: u32, clear_bottom: u32, log_y: u32) -> bool {
    clear_w >= GUI_MAX_W && clear_bottom >= log_y.wrapping_add(GUI_MAX_H)
}

/// `screen_gui_begin`: a surface size the sink accepts.
pub fn gui_size_ok(w: u32, h: u32) -> bool {
    !(w == 0 || h == 0 || w > GUI_MAX_W || h > GUI_MAX_H)
}

/// `screen_gui_begin`: the surface is centred in the text area.
pub fn gui_x(clear_x: u32, clear_w: u32, w: u32) -> u32 {
    clear_x.wrapping_add(clear_w.wrapping_sub(w) / 2)
}
pub fn gui_y(log_y: u32, clear_bottom: u32, h: u32) -> u32 {
    log_y.wrapping_add(clear_bottom.wrapping_sub(log_y).wrapping_sub(h) / 2)
}

/// `screen_pointer_begin`: the cursor starts at the centre of the surface.
pub fn pointer_home(origin: u32, extent: u32) -> u32 {
    origin.wrapping_add(extent / 2)
}

/// `screen_pointer_move`: one axis moved by `delta` and clamped to [0, limit - 1], in 64-bit arithmetic.
pub fn pointer_axis(pos: u32, delta: i32, limit: u32) -> u32 {
    let v = pos as i64 + delta as i64;
    if v < 0 {
        0
    } else if v >= limit as i64 {
        limit.wrapping_sub(1)
    } else {
        v as u32
    }
}

/// `screen_pointer_move`, one axis: the coordinate relative to the surface clamped to [0, extent - 1], and whether
/// it lies inside the surface.
fn local(c: u32, origin: u32, extent: u32) -> (u32, bool) {
    if c < origin {
        return (0, false);
    }
    let d = c - origin;
    if d >= extent { (extent.wrapping_sub(1), false) } else { (d, true) }
}

/// `screen_pointer_move`: surface-local coordinates clamped to the surface (lx | ly << 8) and the inside flag.
/// Only this state, never the global cursor position, reaches the GUI owner.
pub fn pointer_state(cx: u32, cy: u32, gx: u32, gy: u32, gw: u32, gh: u32) -> u32 {
    let (lx, inside_x) = local(cx, gx, gw);
    let (ly, inside_y) = local(cy, gy, gh);
    lx | ly.wrapping_shl(8) | if inside_x && inside_y { INSIDE } else { 0 }
}
