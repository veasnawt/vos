//! Kernel-private protocol models; backend effects are simulated in USB.2-A.
#![forbid(unsafe_code)]

pub mod capabilities;
pub mod controller;
pub mod discovery;
pub mod header;
pub mod resources;
pub mod ring;

mod types;
pub use types::*;
