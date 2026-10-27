//! USB.2-A portable crate. No live platform adapter or boot registration.
#![no_std]
#![forbid(unsafe_code)]

#[path = "mod.rs"]
pub mod xhci;
