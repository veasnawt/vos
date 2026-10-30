//! USB.2-C4.1: hosted xHCI initialization protocol model.
//!
//! A pure state machine over typed operations. It performs no I/O of any kind: it never reads
//! PCI configuration space, never touches MMIO and never dereferences a physical address. Register
//! writes exist only as `ModelEvent` values returned from accepted operations. This crate is not
//! referenced by the kernel build, the Rust staticlib root or any ISO.
#![no_std]
#![forbid(unsafe_code)]

pub mod machine;
pub mod types;

pub use machine::*;
pub use types::*;
