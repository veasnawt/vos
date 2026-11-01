//! Allocation-free USB boot-HID logic. No I/O, requests, timers, allocation or controller authority.
//! The trusted transport and input broker obligations are documented in docs/usb-boot-hid.md.
#![no_std]
#![forbid(unsafe_code)]
pub mod descriptor;
pub mod input;
pub use descriptor::{Binding, Device, Kind, MAX_CONFIGURATION, Selection, Speed};
pub use input::{Completion, Event, Focus, ProtocolRequest, Session};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Descriptor,
    Unsupported,
    Ambiguous,
    Bounds,
    Protocol,
    Stale,
    Ownership,
    Report,
    Overflow,
}
// Includes the caller's largest descriptor buffer; no DMA or heap is needed.
const _: () = assert!(core::mem::size_of::<Session>() * 2 + MAX_CONFIGURATION <= 8192);
