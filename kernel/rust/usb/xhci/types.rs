//! Shared value contracts for the pure model and the narrow production subset.
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
