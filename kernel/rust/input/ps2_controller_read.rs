//! Bounded i8042 controller reads for one channel (keyboard or auxiliary).
//! No hardware, allocation, persistent state, controller writes or unsafe code.
//! C keeps the callers (command, aux_command, init, suspend/resume, pointer
//! start/stop), every controller write and all lifecycle decisions.
#![forbid(unsafe_code)]

/// Accepted KEYBOARD_WAIT_LIMIT: at most 100,000 status reads per call.
pub const WAIT_LIMIT: u32 = 100_000;
const STATUS: u16 = 0x64;
const DATA: u16 = 0x60;
const OBF: u8 = 0x01;
const AUX: u8 = 0x20;
const ERR: u8 = 0xC0;

/// Synchronous adapter over one live keyboard. `read` reaches the controller
/// backend and reloads the callback on every call; no borrow spans it.
pub trait ControllerRead {
    fn read(&mut self, port: u16) -> u8;
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Channel {
    Keyboard,
    Aux,
}

/// Accepted read_keyboard()/read_aux() contract: each iteration reads status
/// first and never reads data while OBF is clear; ERR fails the call before the
/// channel is examined; a byte from the other channel is consumed and waiting
/// continues; the first byte from the requested channel is returned. None after
/// WAIT_LIMIT status reads. The caller's output is written only on success.
#[inline(always)]
pub fn read_channel<S: ControllerRead>(s: &mut S, channel: Channel) -> Option<u8> {
    let want_aux = channel == Channel::Aux;
    for _ in 0..WAIT_LIMIT {
        let status = s.read(STATUS);
        if status & OBF == 0 {
            continue;
        }
        let data = s.read(DATA);
        if status & ERR != 0 {
            return None;
        }
        if (status & AUX != 0) == want_aux {
            return Some(data);
        }
    }
    None
}
