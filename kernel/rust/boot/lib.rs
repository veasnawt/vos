//! USB.2-C3a.2 portable boot-memory crate root for hosted tests. The kernel
//! archive includes efi_memory.rs directly; no platform adapter lives here.
#![no_std]
#![forbid(unsafe_code)]

pub mod efi_memory;
