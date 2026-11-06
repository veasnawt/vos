//! Ordinary (no pointer lease) bounded PS/2 keyboard polling policy.
//! No hardware, allocation, persistent state or unsafe code. C keeps the public
//! dispatcher, storage, controller ownership and all controller writes.
#![forbid(unsafe_code)]

/// Accepted KEYBOARD_DRAIN_LIMIT: at most 32 status reads and 32 data reads.
pub const LIMIT: u32 = 32;
const STATUS: u16 = 0x64;
const DATA: u16 = 0x60;
const OBF: u8 = 0x01;
const AUX: u8 = 0x20;
const ERR: u8 = 0xC0;

/// Synchronous adapter over one live keyboard. Only `read` reaches the controller
/// backend and it reloads the callback on every call; no borrow spans it.
/// `decode` is the accepted pure set-1 decoder; `reset_decoder` clears all seven
/// decoder bytes. `ready` is read once, before any port access.
pub trait Ordinary {
    fn ready(&self) -> bool;
    fn read(&mut self, port: u16) -> u8;
    fn reset_decoder(&mut self);
    fn decode(&mut self, byte: u8) -> u8;
}

/// Accepted ordinary keyboard_poll contract: test ready first; each iteration
/// reads status before data and never reads data while OBF is clear; auxiliary
/// bytes are consumed undecoded; ERR, 0x00 and 0xFF reset the decoder; the first
/// nonzero decoded character is returned, otherwise 0 after at most LIMIT bytes.
#[inline(always)]
pub fn poll<S: Ordinary>(s: &mut S) -> u8 {
    if !s.ready() {
        return 0;
    }
    for _ in 0..LIMIT {
        let status = s.read(STATUS);
        if status & OBF == 0 {
            break;
        }
        let byte = s.read(DATA);
        if status & AUX != 0 {
            continue;
        }
        if status & ERR != 0 || byte == 0 || byte == 0xFF {
            s.reset_decoder();
            continue;
        }
        let c = s.decode(byte);
        if c != 0 {
            return c;
        }
    }
    0
}
