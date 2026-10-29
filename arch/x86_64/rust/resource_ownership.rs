//! Private non-device adapter. Uses the canonical logical Lease as the unique
//! handle; it does NOT implement the live xHCI Backend or assert DMA quiescence.
#![allow(dead_code)] // Shared model types retain their existing complete API.

use crate::xhci::resources::{Kind, Lease};

#[repr(C)]
#[derive(Default)]
struct Raw {
    token: u64,
    address: u64,
}
const _: () = assert!(core::mem::size_of::<Raw>() == 16);
const _: () = assert!(core::mem::align_of::<Raw>() == 8);

unsafe extern "C" {
    fn usb_resource_acquire(kind: u32, out: *mut Raw) -> i32;
    fn usb_resource_release(kind: u32, token: u64) -> i32;
    fn usb_resource_retain(kind: u32, token: u64);
}

/// Non-Copy and !Send/!Sync through the existing Lease marker. No address is
/// exposed for dereference; a control page is ordinary CPU RAM, never DMA.
pub(crate) struct Owned {
    lease: Option<Lease>,
    kind: u32,
}
impl Owned {
    fn acquire(kind: Kind) -> Result<Self, i32> {
        let abi_kind = match kind {
            Kind::Control => 1,
            Kind::Mapping => 2,
            _ => return Err(1), // no symbolic DMA/worker kind becomes real
        };
        let mut raw = Raw::default();
        // SAFETY: exact C prototype; exclusive initialized stack output;
        // private synchronous service checks kernel CR3/IF/non-IRQ context.
        let result = unsafe { usb_resource_acquire(abi_kind, &mut raw) };
        if result != 0 {
            // C retains any rollback-failed resources even on failed acquire.
            return Err(result);
        }
        match Lease::new(kind, raw.token, raw.address) {
            Ok(lease) => Ok(Self { lease: Some(lease), kind: abi_kind }),
            Err(_) => {
                // SAFETY: same private service/owner; never free on bad output.
                unsafe { usb_resource_retain(abi_kind, raw.token) };
                Err(8)
            }
        }
    }
    fn release(&mut self) -> Result<(), i32> {
        let lease = match &self.lease {
            Some(lease) => lease,
            None => return Err(8),
        };
        // SAFETY: C verifies the live generation and actual owner record before
        // mutation. Failure leaves both this handle and C identifiers intact.
        let result = unsafe { usb_resource_release(self.kind, lease.token()) };
        if result == 0 {
            self.lease = None;
            Ok(())
        } else {
            Err(result)
        }
    }
}
impl Drop for Owned {
    fn drop(&mut self) {
        if let Some(lease) = &self.lease {
            // SAFETY: diagnostic only, no fallible/context-sensitive teardown.
            // The C registry remains authoritative and blocks reacquisition.
            unsafe { usb_resource_retain(self.kind, lease.token()) };
        }
    }
}

// SAFETY: unique private scalar C ABI; called only by opt-in native fixtures.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_resource_cycle() -> i32 {
    let mut control = match Owned::acquire(Kind::Control) {
        Ok(lease) => lease,
        Err(_) => return 1,
    };
    let mut mapping = match Owned::acquire(Kind::Mapping) {
        Ok(lease) => lease,
        Err(_) => return if control.release().is_ok() { 2 } else { -1 },
    };
    // Dependency order: mapping/PTs before CPU storage. A failed unmap keeps
    // both resources retained. There is no worker/device exposure in this call.
    if mapping.release().is_err() || control.release().is_err() {
        return -1;
    }
    if mapping.release().is_ok() || control.release().is_ok() {
        return -2; // a second explicit release must fail without backend effects
    }
    0
}

// SAFETY: opt-in fixture-only proof that dropping a Rust value retains the
// actual C-owned page. Recovery is an explicit owner action in the C verifier.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_resource_abandon() -> i32 {
    match Owned::acquire(Kind::Control) {
        Ok(lease) => { drop(lease); 0 }
        Err(error) => error,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn layout_and_budget() {
        assert!(core::mem::size_of::<Owned>() <= 40);
        assert_eq!(Kind::Control.charge().physical_bytes(), 4096);
        assert_eq!(Kind::Mapping.charge().physical_bytes(), 8192);
        assert!(Kind::Control.charge().add(Kind::Mapping.charge()).is_ok());
    }
}
