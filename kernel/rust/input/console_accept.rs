//! Bounded console edit/echo flow. The adapter owns raw storage and C callbacks.
#![forbid(unsafe_code)]
pub const MAX: u32 = 128;
/// Synchronous operations; no editor reference may survive an echo callback.
/// write indices are always in 0..=128. No allocation or dynamic dispatch.
pub trait Editor {
    fn ready(&self) -> bool;
    fn length(&self) -> u32;
    fn set_length(&mut self, length: u32);
    fn write(&mut self, index: u32, byte: u8);
    fn seal(&mut self);
    fn overflow(&mut self);
    fn newline(&mut self);
    fn erase(&mut self);
    fn echo(&mut self, byte: u8);
}
#[inline(always)]
pub fn accept<S: Editor>(s: &mut S, byte: u8) {
    if s.ready() { return; }
    match byte {
        b'\n' => { s.newline(); s.seal(); }
        8 => {
            if s.length() != 0 {
                s.erase();
                // C reloads length after erase. Preserve this observable order;
                // malformed callback-produced lengths never form raw indices.
                if let Some(index) = s.length().checked_sub(1).filter(|n| *n <= MAX) {
                    s.set_length(index);
                    s.write(index, 0);
                }
            }
        }
        b' '..=b'~' => {
            let length = s.length();
            if length == MAX { s.overflow(); }
            else if length < MAX {
                s.set_length(length + 1);
                s.write(length, byte);
                s.write(length + 1, 0);
                s.echo(byte);
            }
        }
        _ => {}
    }
}
