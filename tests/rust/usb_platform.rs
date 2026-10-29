//! Host test root for the private USB ownership adapter: the canonical
//! USB.2 tree plus resource_ownership, exactly as the kernel crate mounts them.
//! Only the adapter's own unit tests run here; the model has its own suite.
#![allow(dead_code)]
#[path = "../../kernel/rust/usb/xhci/mod.rs"]
pub mod xhci;
#[path = "../../arch/x86_64/rust/resource_ownership.rs"]
mod resource_ownership;
