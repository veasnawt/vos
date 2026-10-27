//! Kernel-private protocol models; backend effects are simulated in USB.2-A.
#![forbid(unsafe_code)]

pub mod capabilities;
pub mod controller;
pub mod resources;
pub mod ring;

pub const PAGE: u64 = 4096;
pub const BAR_BYTES: u32 = 16384;
pub const TRBS: u16 = 256;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Truncated,
    Unsupported,
    InvalidCapability,
    Overflow,
    Bounds,
    Alignment,
    IllegalState,
    RingFull,
    UnexpectedEvent,
    Stale,
    Budget,
    Allocation,
    Backend,
    Timeout,
    Clock,
    Cancelled,
    Quarantined,
}
