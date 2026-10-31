//! USB.3-A: hosted EHCI qualification and firmware-handoff model.
//!
//! A pure state machine over typed operations and typed facts. It performs no I/O of any kind: it never reads PCI
//! configuration space, never touches MMIO, ports or DMA memory, and never dereferences an address. A register write
//! exists only as an `Event` value returned from an accepted request, and nothing in this crate can perform it. This
//! crate is not referenced by the kernel build, the Rust staticlib root or any ISO.
#![no_std]
#![forbid(unsafe_code)]

pub mod dell;
pub mod facts;
pub mod machine;
pub mod types;

pub use facts::*;
pub use machine::*;
pub use types::*;
