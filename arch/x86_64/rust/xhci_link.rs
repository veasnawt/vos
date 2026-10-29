//! USB.2-C1 link anchor. It makes the canonical USB.2-A controller, ring and
//! capability code part of the production archive without activating it.
//! There is no C prototype and no kernel caller: the symbol exists only so the
//! generic lifecycle is generated and linked for size, symbol and map review.
//! Its backend refuses every effect; it owns no lease and touches no device.
use crate::xhci::Error;
use crate::xhci::capabilities::Facts;
use crate::xhci::controller::{Action, Backend, Controller, Observation, Sample, Timeouts};
use crate::xhci::resources::{Kind, Lease};
use crate::xhci::ring::Submission;
use core::hint::black_box;

/// Opaque refusals: the whole Result is hidden from the optimizer, so success
/// paths are not proven dead and the complete lifecycle is generated.
struct Refusing;
impl Backend for Refusing {
    fn identity(&self) -> u64 {
        black_box(0x5553_4232_4331_0001)
    }
    fn acquire(&mut self, _: Kind) -> Result<Lease, Error> {
        black_box(Err(Error::Unsupported))
    }
    fn release(&mut self, _: &Lease) -> Result<(), Error> {
        black_box(Err(Error::Unsupported))
    }
    fn action(&mut self, _: Action) -> Result<(), Error> {
        black_box(Err(Error::Unsupported))
    }
    fn observe(&mut self, _: Observation) -> Result<bool, Error> {
        black_box(Err(Error::Unsupported))
    }
    fn publish(&mut self, _: &Submission) -> Result<(), Error> {
        black_box(Err(Error::Unsupported))
    }
    fn peek(&mut self, _: u16) -> Result<Option<Sample>, Error> {
        black_box(Err(Error::Unsupported))
    }
    fn consumed(&mut self, _: u64) -> Result<(), Error> {
        black_box(Err(Error::Unsupported))
    }
}

// Each lifecycle entry is generated in its own frame; inlining them into the
// anchor would sum their locals with the Controller's and exceed the budget.
#[inline(never)]
fn qualify(c: &mut Controller) -> bool {
    let facts: Facts = black_box(Facts {
        bdf: 0,
        vendor: 0,
        device: 0,
        bar_base: 0,
        bar_bytes: 0,
        bar_address_bits: 0,
        physical_bits: 0,
        candidates: 0,
        complete_scan: false,
        memory_enabled: false,
        d0: false,
        identity_dma: false,
        coherent_dma: false,
        qualified_resource: false,
        ram_disjoint: false,
    });
    c.qualify(black_box(&[]), facts).is_err()
}
#[inline(never)]
fn start(c: &mut Controller, b: &mut Refusing, now: u64) -> bool {
    c.start(b, black_box(1), now).is_err()
}
#[inline(never)]
fn step(c: &mut Controller, b: &mut Refusing, now: u64) -> bool {
    c.step(b, now).is_err()
}
#[inline(never)]
fn submit(c: &mut Controller, b: &mut Refusing, now: u64) -> bool {
    c.submit_noop(b, now).is_err()
}
#[inline(never)]
fn stop(c: &mut Controller, b: &mut Refusing, now: u64) -> bool {
    let cancelled = c.cancel(b, now).is_err();
    let shut = c.shutdown(b, now).is_err();
    black_box(c.snapshot());
    cancelled && shut
}

// SAFETY: unique private symbol with scalar arguments. Never called by VOS;
// even if called, the refusing backend makes every lifecycle step fail closed.
#[unsafe(no_mangle)]
pub extern "C" fn vos_xhci_controller_link_v1(now: u64) -> i32 {
    let timeouts = Timeouts {
        ownership: 100,
        halt: 10,
        reset: 100,
        run: 100,
        command: 100,
        stop: 100,
        join: 100,
    };
    // The Controller stays inside the constructor's result: one stack copy.
    let mut created = Controller::new(timeouts);
    let Ok(owned) = created.as_mut() else {
        return -1;
    };
    let c: &mut Controller = black_box(owned);
    let mut refusing = Refusing;
    let b: &mut Refusing = black_box(&mut refusing);
    let failures = [qualify(c), start(c, b, now), step(c, b, now), submit(c, b, now), stop(c, b, now)];
    failures.iter().filter(|f| **f).count() as i32
}
