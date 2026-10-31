//! A3: the framebuffer validation of `boot_framebuffer`, ported from the C of `arch/x86_64/bootinfo.c` without any change of
//! behavior. Pure: no unsafe code, no allocation, no panic path, no I/O. The checks run in the C's order, because the order
//! decides which message wins.
//!
//! The walk of the loader's block already bounded the tag (`mb2_parse`); this code re-reads only what the C re-reads: the tag's
//! own `size`, then the fields of the 32-byte base structure, then, for RGB, the six channel bytes at offset 32. The caller hands
//! over exactly `min(size, 38)` bytes of the tag, so nothing here can read beyond the tag or beyond what the C reads.

/// `BOOT_FB_*` of `bootinfo.h`. `BOOT_FB_NONE` (0) is the zeroed output, so it needs no constant here.
pub const FB_BGRX32: i32 = 1;
pub const FB_EGA_TEXT: i32 = 2;

pub const FB_MIN_W: u32 = 320;
pub const FB_MIN_H: u32 = 200;
pub const FB_MAX_DIM: u32 = 8192;
pub const FB_MAX_PITCH: u32 = 65536;
pub const FB_MAX_BYTES: u64 = 32 << 20;

const TAG_BASE: u32 = 32; // sizeof(struct mb2_tag_framebuffer)
const TAG_RGB: u32 = 38; // plus sizeof(struct mb2_fb_rgb)
/// The most bytes of a tag this code ever reads.
pub const READ_MAX: u32 = TAG_RGB;
const TYPE_RGB: u8 = 1;
const TYPE_EGA_TEXT: u8 = 2;
const EGA_ADDR: u64 = 0xB8000;
const EGA_BYTES: u64 = 4000;

/// The error strings, byte for byte as in C, each NUL-terminated so a pointer to the first byte is a C string.
pub const E_NONE: &[u8] = b"no framebuffer information\0";
pub const E_MANY: &[u8] = b"more than one framebuffer tag\0";
pub const E_TRUNCATED: &[u8] = b"framebuffer tag truncated\0";
pub const E_EGA: &[u8] = b"EGA text framebuffer other than 80x25 at 0xB8000\0";
pub const E_NOT_RGB: &[u8] = b"framebuffer is not RGB (indexed colour is not supported)\0";
pub const E_NO_LAYOUT: &[u8] = b"RGB framebuffer tag without its channel layout\0";
pub const E_BPP: &[u8] = b"framebuffer is not 32 bits per pixel\0";
pub const E_LAYOUT: &[u8] = b"pixel layout is not BGRX (red 16, green 8, blue 0, 8 bits each)\0";
pub const E_DIMENSIONS: &[u8] = b"framebuffer dimensions outside 320x200 to 8192x8192\0";
pub const E_PITCH: &[u8] = b"framebuffer pitch below width x 4, unaligned or too large\0";
pub const E_BYTES: &[u8] = b"framebuffer larger than 32 MiB\0";
pub const E_ADDRESS: &[u8] = b"framebuffer address zero, unaligned or wrapping\0";

/// The validated framebuffer: the fields of `struct boot_fb` that the C sets on success.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fb {
    pub kind: i32,
    pub addr: u64,
    pub size: u64,
    pub pitch: u32,
    pub width: u32,
    pub height: u32,
}

/// The two checks that come before the tag is touched: some tag was recorded, and only one. `present` is whether the pointer to
/// it is non-null. Nothing about the tag itself has been read when these fail.
pub fn precheck(fb_tags: u32, present: bool) -> Result<(), &'static [u8]> {
    if fb_tags == 0 || !present {
        return Err(E_NONE);
    }
    if fb_tags > 1 {
        return Err(E_MANY);
    }
    Ok(())
}

fn byte(b: &[u8], at: usize) -> u8 {
    b.get(at).copied().unwrap_or(0)
}

fn le32(b: &[u8], at: usize) -> u32 {
    u32::from(byte(b, at)) | u32::from(byte(b, at + 1)) << 8 | u32::from(byte(b, at + 2)) << 16 | u32::from(byte(b, at + 3)) << 24
}

fn le64(b: &[u8], at: usize) -> u64 {
    u64::from(le32(b, at)) | u64::from(le32(b, at + 4)) << 32
}

/// Validate one framebuffer tag. `size` is the tag's own `size` field; `tag` is its first `min(size, 38)` bytes.
pub fn validate(size: u32, tag: &[u8]) -> Result<Fb, &'static [u8]> {
    if size < TAG_BASE {
        return Err(E_TRUNCATED);
    }
    let (addr, pitch, width, height) = (le64(tag, 8), le32(tag, 16), le32(tag, 20), le32(tag, 24));
    let (bpp, kind) = (byte(tag, 28), byte(tag, 29));
    if kind == TYPE_EGA_TEXT {
        if addr != EGA_ADDR || width != 80 || height != 25 || bpp != 16 || pitch != 160 {
            return Err(E_EGA);
        }
        return Ok(Fb { kind: FB_EGA_TEXT, addr, size: EGA_BYTES, pitch, width, height });
    }
    if kind != TYPE_RGB {
        return Err(E_NOT_RGB);
    }
    if size < TAG_RGB {
        return Err(E_NO_LAYOUT);
    }
    let bytes = u64::from(pitch) * u64::from(height);
    if bpp != 32 {
        return Err(E_BPP);
    }
    // red position and size, green, blue: the six bytes of struct mb2_fb_rgb right after the base structure
    if byte(tag, 32) != 16 || byte(tag, 33) != 8 || byte(tag, 34) != 8 || byte(tag, 35) != 8 || byte(tag, 36) != 0 || byte(tag, 37) != 8 {
        return Err(E_LAYOUT);
    }
    if width < FB_MIN_W || height < FB_MIN_H || width > FB_MAX_DIM || height > FB_MAX_DIM {
        return Err(E_DIMENSIONS);
    }
    if pitch % 4 != 0 || u64::from(pitch) < u64::from(width) * 4 || pitch > FB_MAX_PITCH {
        return Err(E_PITCH);
    }
    if bytes > FB_MAX_BYTES {
        return Err(E_BYTES);
    }
    if addr == 0 || addr % 4 != 0 || addr.checked_add(bytes).is_none() {
        return Err(E_ADDRESS);
    }
    Ok(Fb { kind: FB_BGRX32, addr, size: bytes, pitch, width, height })
}
