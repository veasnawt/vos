//! R2-L PS/2 controller lifecycle: the accepted C `keyboard_init()`,
//! `keyboard_suspend()`, `keyboard_resume()`, `keyboard_pointer_start()` and
//! `keyboard_pointer_stop()` sequences, behind an adapter with no unsafe code.
//! Every controller access goes through the accepted bounded Rust read/write
//! helpers (or, for the drain loops, a single status/data read). Only the
//! commands of accepted main ed3b496 are issued: no output-port, A20, reset or
//! PIC operations exist here. State changes happen at the same points relative
//! to controller callbacks as in the accepted C.
#![forbid(unsafe_code)]

use super::controller_write::Port;

/// Bytes drained per bounded drain loop; equals the accepted `KEYBOARD_DRAIN_LIMIT`.
pub const DRAIN_LIMIT: u32 = 32;
const STATUS: u16 = 0x64;
const DATA: u16 = 0x60;
const OBF: u8 = 0x01;
const ACK: u8 = 0xFA;
const BAT_OK: u8 = 0xAA;
const MOUSE_ID: u8 = 0x00;
const MODE_SET: u8 = 0x60;
const MODE_CLEAR: u8 = 0x13;

/// The live keyboard object and its controller, as seen by the lifecycle.
pub trait Lifecycle {
    /// Accepted bounded helpers (status-polled write; channel-filtered read).
    fn write_port(&mut self, port: Port, value: u8) -> bool;
    fn command(&mut self, value: u8) -> bool;
    fn aux_command(&mut self, value: u8) -> bool;
    fn read_keyboard(&mut self) -> Option<u8>;
    fn read_aux(&mut self) -> Option<u8>;
    /// One controller read through the object's current io table.
    fn read(&mut self, port: u16) -> u8;
    /// The out-of-line suspend/resume entries (as C calls its public functions).
    fn suspend(&mut self) -> bool;
    fn resume(&mut self) -> bool;
    fn io_present(&self) -> bool;
    fn ready(&self) -> bool;
    fn set_ready(&mut self, ready: bool);
    fn pointer_ready(&self) -> bool;
    fn set_pointer_ready(&mut self, ready: bool);
    fn clear_decoder(&mut self);
    fn clear_pointer_decoder(&mut self);
    fn clear_pointer_pending(&mut self);
    fn clear_pointer_lost(&mut self);
    fn clear_key_head(&mut self);
    fn clear_key_count(&mut self);
}

/// Initialization additionally sees the caller's io table, which the accepted
/// C uses directly (not the object's) for its validity check and drain loop.
pub trait Init: Lifecycle {
    /// `*k = (struct keyboard){ .io = io }`: every byte zero, then io.
    fn reset_object(&mut self);
    /// io, io->in and io->out are all non-null.
    fn io_complete(&self) -> bool;
    /// One read through the caller's io table.
    fn read_original(&mut self, port: u16) -> u8;
}

#[inline(always)]
pub fn init<S: Init>(s: &mut S) -> bool {
    s.reset_object();
    if !s.io_complete() {
        return false;
    }
    if !s.write_port(Port::Status, 0xAD) || !s.write_port(Port::Status, 0xA7) {
        return false;
    }
    let mut drained = 0;
    while drained < DRAIN_LIMIT && s.read_original(STATUS) & OBF != 0 {
        s.read_original(DATA);
        drained += 1;
    }
    if s.read_original(STATUS) & OBF != 0 {
        return false;
    }
    if !s.write_port(Port::Status, 0x20) {
        return false;
    }
    let Some(mode) = s.read_keyboard() else {
        return false;
    };
    let mode = (mode | MODE_SET) & !MODE_CLEAR;
    if !s.write_port(Port::Status, 0x60)
        || !s.write_port(Port::Data, mode)
        || !s.write_port(Port::Status, 0xAE)
        || !s.command(0xF5)
        || !s.command(0xF0)
        || !s.command(0x02)
        || !s.command(0xF4)
    {
        return false;
    }
    s.set_ready(true);
    true
}

#[inline(always)]
pub fn suspend<S: Lifecycle>(s: &mut S) -> bool {
    if !s.io_present() {
        return false;
    }
    if !s.ready() {
        return true;
    }
    s.set_ready(false);
    if !s.write_port(Port::Data, 0xF5) {
        return false;
    }
    let mut ack = false;
    for _ in 0..DRAIN_LIMIT {
        match s.read_keyboard() {
            None => break,
            Some(ACK) => {
                ack = true;
                break;
            }
            Some(_) => {}
        }
    }
    if !ack {
        return false;
    }
    for _ in 0..DRAIN_LIMIT {
        if s.read(STATUS) & OBF == 0 {
            s.clear_decoder();
            return true;
        }
        s.read(DATA);
    }
    false
}

#[inline(always)]
pub fn resume<S: Lifecycle>(s: &mut S) -> bool {
    if !s.io_present() || s.ready() {
        return false;
    }
    s.clear_decoder();
    if !s.command(0xF4) {
        return false;
    }
    s.set_ready(true);
    true
}

#[inline(always)]
pub fn pointer_start<S: Lifecycle>(s: &mut S) -> bool {
    if !s.ready() || s.pointer_ready() {
        return false;
    }
    if !s.suspend() {
        return false;
    }
    let mut ok = s.write_port(Port::Status, 0xAD)
        && s.write_port(Port::Status, 0xA8)
        && s.aux_command(0xFF)
        && s.read_aux() == Some(BAT_OK)
        && s.read_aux() == Some(MOUSE_ID)
        && s.aux_command(0xF6)
        && s.aux_command(0xE6)
        && s.aux_command(0xF4);
    if !ok {
        s.write_port(Port::Status, 0xA7);
    }
    if !s.write_port(Port::Status, 0xAE) || !s.resume() {
        ok = false;
    }
    if !ok {
        s.write_port(Port::Status, 0xA7);
        return false;
    }
    s.clear_pointer_decoder();
    s.clear_pointer_pending();
    s.clear_pointer_lost();
    s.clear_key_count();
    s.clear_key_head();
    s.set_pointer_ready(true);
    true
}

/// Best effort: every step runs regardless of the previous one's result.
#[inline(always)]
pub fn pointer_stop<S: Lifecycle>(s: &mut S) {
    if !s.pointer_ready() {
        return;
    }
    s.write_port(Port::Status, 0xAD);
    s.aux_command(0xF5);
    s.write_port(Port::Status, 0xA7);
    s.write_port(Port::Status, 0xAE);
    s.clear_pointer_pending();
    s.set_pointer_ready(false);
    s.clear_pointer_decoder();
    s.clear_key_count();
    s.clear_key_head();
    s.clear_pointer_lost();
}
