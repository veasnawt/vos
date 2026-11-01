//! Bounded console extraction flow; storage adapter owns raw memory access.
#![forbid(unsafe_code)]
pub const MAX: u32 = 128;
pub const PENDING: i32 = -1;
pub const NOSPACE: i32 = -2;
pub const TOOLONG: i32 = -3;

/// Synchronous storage operations, in C forward-copy order. No dynamic dispatch
/// is required: production monomorphizes a private raw-memory adapter. Read/write
/// indices are bounded to 0..=MAX after admission; clear resets the whole editor.
pub trait Storage {
    fn ready(&self) -> bool;
    fn overflow(&self) -> bool;
    fn length(&self) -> u32;
    fn capacity(&self) -> u32;
    fn output_valid(&self, count: u32) -> bool;
    fn read(&self, index: u32) -> u8;
    fn write(&mut self, index: u32, byte: u8);
    fn clear(&mut self);
}

#[inline(always)]
pub fn take_line<S: Storage>(s: &mut S) -> i32 {
    if !s.ready() { return PENDING; }
    if s.overflow() { s.clear(); return TOOLONG; }
    let length = s.length();
    // Invalid length on the old C copy path was outside the 129-byte object.
    // Refuse it without touching either buffer; valid-call precedence is intact.
    if length >= s.capacity().min(MAX + 1) || !s.output_valid(length + 1) { return NOSPACE; }
    if length == 0 {
        let byte = s.read(0);
        s.write(0, byte);
        s.clear();
        return 0;
    }
    for index in 0..=length {
        let byte = s.read(index);
        s.write(index, byte);
    }
    s.clear();
    length as i32
}
