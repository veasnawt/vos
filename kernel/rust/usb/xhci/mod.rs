//! Kernel-private protocol models; backend effects are simulated in USB.2-A.
#![forbid(unsafe_code)]

pub mod capabilities;
pub mod containment;
pub mod controller;
pub mod discovery;
pub mod dma;
pub mod header;
pub mod lifecycle;
pub mod qualify;
pub mod residue;
pub mod resources;
pub mod ring;

mod types;
pub use types::*;
