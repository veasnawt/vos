//! R2-W bounded PS/2 controller writes: the accepted C `write_port()`,
//! `command()` and `aux_command()` contract, behind an adapter with no
//! unsafe code. Only the controller writes the accepted C driver already
//! performs are expressible: a byte to the data port (0x60) or the command
//! port (0x64). The keyboard and auxiliary acknowledgements use the accepted
//! bounded controller read.
#![forbid(unsafe_code)]

use super::controller_read::{read_channel, Channel, ControllerRead};

/// Status polls per write; equals the accepted `KEYBOARD_WAIT_LIMIT`.
pub const WAIT_LIMIT: u32 = 100_000;
const STATUS: u16 = 0x64;
const DATA: u16 = 0x60;
const IBF: u8 = 0x02;
const ACK: u8 = 0xFA;
const AUX_PREFIX: u8 = 0xD4;

/// A controller port this driver may write. No other port is representable.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Port {
    Data,
    Status,
}

impl Port {
    #[inline(always)]
    pub const fn address(self) -> u16 {
        match self {
            Port::Data => DATA,
            Port::Status => STATUS,
        }
    }

    /// Maps the accepted C port numbers; any other port is refused.
    #[inline(always)]
    pub const fn from_address(address: u16) -> Option<Port> {
        match address {
            DATA => Some(Port::Data),
            STATUS => Some(Port::Status),
            _ => None,
        }
    }
}

pub trait ControllerWrite: ControllerRead {
    /// Performs one port write; false only if no write callback exists.
    fn write(&mut self, port: u16, value: u8) -> bool;
}

/// Reads status on every iteration and writes exactly once after the input
/// buffer is clear. After the limit, returns false without writing.
#[inline(always)]
pub fn write_port<S: ControllerWrite>(s: &mut S, port: Port, value: u8) -> bool {
    for _ in 0..WAIT_LIMIT {
        if s.read(STATUS) & IBF == 0 {
            return s.write(port.address(), value);
        }
    }
    false
}

/// Data-port write, then keyboard-channel acknowledgement 0xFA; stops at the
/// first failed step.
#[inline(always)]
pub fn command<S: ControllerWrite>(s: &mut S, value: u8) -> bool {
    write_port(s, Port::Data, value) && read_channel(s, Channel::Keyboard) == Some(ACK)
}

/// 0xD4 to the command port, the byte to the data port, then auxiliary
/// acknowledgement 0xFA; stops at the first failed step.
#[inline(always)]
pub fn aux_command<S: ControllerWrite>(s: &mut S, value: u8) -> bool {
    write_port(s, Port::Status, AUX_PREFIX)
        && write_port(s, Port::Data, value)
        && read_channel(s, Channel::Aux) == Some(ACK)
}
