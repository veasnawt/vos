//! Existing translated Set-1 US text decoder. No controller or ownership policy.
//! All seven fields are bytes, so every encoding is valid Rust data. Preserve
//! the C's bytewise behavior even for noncanonical modifier/Pause values.
#![forbid(unsafe_code)]

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Decoder {
    pub shift: u8,
    pub ctrl: u8,
    pub alt: u8,
    pub caps: u8,
    pub caps_down: u8,
    pub extended: u8,
    pub pause_left: u8,
}

impl Decoder {
    /// One byte, one bounded transition; no polling, callbacks or allocation.
    pub fn decode(&mut self, scan: u8) -> u8 {
        if self.pause_left != 0 {
            self.pause_left -= 1;
            return 0;
        }
        if scan == 0xe1 {
            self.extended = 0;
            self.pause_left = 5;
            return 0;
        }
        if scan == 0xe0 {
            self.extended = 1;
            return 0;
        }
        let ext = self.extended != 0;
        let up = scan & 0x80 != 0;
        let code = scan & 0x7f;
        let mut bit = if ext { 2 } else { 1 };
        self.extended = 0;
        if code == 0x1d || code == 0x38 {
            let mask = if code == 0x1d { &mut self.ctrl } else { &mut self.alt };
            *mask = if up { *mask & !bit } else { *mask | bit };
            return 0;
        }
        if ext {
            return 0;
        }
        if code == 0x2a || code == 0x36 {
            bit = if code == 0x2a { 1 } else { 2 };
            self.shift = if up { self.shift & !bit } else { self.shift | bit };
            return 0;
        }
        if code == 0x3a {
            if !up && self.caps_down == 0 {
                self.caps ^= 1;
            }
            self.caps_down = u8::from(!up);
            return 0;
        }
        if up || self.ctrl != 0 || self.alt != 0 {
            return 0;
        }
        // code is masked to seven bits, hence both indices are in 0..128.
        let c = PLAIN[usize::from(code)];
        if c >= b'a' && c <= b'z' {
            return if (u8::from(self.shift != 0) ^ self.caps) != 0 {
                c - b'a' + b'A'
            } else { c };
        }
        if self.shift != 0 && SHIFTED[usize::from(code)] != 0 {
            SHIFTED[usize::from(code)]
        } else { c }
    }
}

const PLAIN: [u8; 128] = {
    let mut bytes = [0; 128];
    bytes[0x02] = 49;
    bytes[0x03] = 50;
    bytes[0x04] = 51;
    bytes[0x05] = 52;
    bytes[0x06] = 53;
    bytes[0x07] = 54;
    bytes[0x08] = 55;
    bytes[0x09] = 56;
    bytes[0x0A] = 57;
    bytes[0x0B] = 48;
    bytes[0x0C] = 45;
    bytes[0x0D] = 61;
    bytes[0x0E] = 8;
    bytes[0x10] = 113;
    bytes[0x11] = 119;
    bytes[0x12] = 101;
    bytes[0x13] = 114;
    bytes[0x14] = 116;
    bytes[0x15] = 121;
    bytes[0x16] = 117;
    bytes[0x17] = 105;
    bytes[0x18] = 111;
    bytes[0x19] = 112;
    bytes[0x1A] = 91;
    bytes[0x1B] = 93;
    bytes[0x1C] = 10;
    bytes[0x1E] = 97;
    bytes[0x1F] = 115;
    bytes[0x20] = 100;
    bytes[0x21] = 102;
    bytes[0x22] = 103;
    bytes[0x23] = 104;
    bytes[0x24] = 106;
    bytes[0x25] = 107;
    bytes[0x26] = 108;
    bytes[0x27] = 59;
    bytes[0x28] = 39;
    bytes[0x29] = 96;
    bytes[0x2B] = 92;
    bytes[0x2C] = 122;
    bytes[0x2D] = 120;
    bytes[0x2E] = 99;
    bytes[0x2F] = 118;
    bytes[0x30] = 98;
    bytes[0x31] = 110;
    bytes[0x32] = 109;
    bytes[0x33] = 44;
    bytes[0x34] = 46;
    bytes[0x35] = 47;
    bytes[0x39] = 32;
    bytes
};

const SHIFTED: [u8; 128] = {
    let mut bytes = [0; 128];
    bytes[0x02] = 33;
    bytes[0x03] = 64;
    bytes[0x04] = 35;
    bytes[0x05] = 36;
    bytes[0x06] = 37;
    bytes[0x07] = 94;
    bytes[0x08] = 38;
    bytes[0x09] = 42;
    bytes[0x0A] = 40;
    bytes[0x0B] = 41;
    bytes[0x0C] = 95;
    bytes[0x0D] = 43;
    bytes[0x1A] = 123;
    bytes[0x1B] = 125;
    bytes[0x27] = 58;
    bytes[0x28] = 34;
    bytes[0x29] = 126;
    bytes[0x2B] = 124;
    bytes[0x33] = 60;
    bytes[0x34] = 62;
    bytes[0x35] = 63;
    bytes
};
