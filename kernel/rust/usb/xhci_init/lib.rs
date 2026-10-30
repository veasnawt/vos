//! USB.2-C4.1: hosted xHCI initialization protocol model.
//!
//! A pure state machine over typed operations. It performs no I/O of any kind: it never reads PCI
//! configuration space, never touches MMIO and never dereferences a physical address. Register
//! writes exist only as `ModelEvent` values returned from accepted operations. This crate is not
//! referenced by the kernel build, the Rust staticlib root or any ISO.
//!
//! USB.2-C4.2a adds the hosted backend contract (`backend`), the session that is the only path for
//! observations to reach the machine (`session`) and a model-only DMA ownership ledger (`dma_model`).
#![no_std]
#![forbid(unsafe_code)]

pub mod backend;
pub mod dma_model;
pub mod machine;
pub mod session;
pub mod types;

pub use backend::*;
pub use dma_model::*;
pub use machine::*;
pub use session::*;
pub use types::*;
