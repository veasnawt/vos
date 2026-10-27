//! Compile-only fixture: force generic Controller methods through bare-metal
//! code generation. Never linked to VOS; the large Fake is host fixture storage.
#![no_std]
#![forbid(unsafe_code)]
use vos_xhci::xhci::Error;
use vos_xhci::xhci::capabilities::Facts;
use vos_xhci::xhci::controller::{Controller, State};
use vos_xhci::xhci::ring::CommandId;

#[path = "xhci_fake.rs"]
pub mod fixture;
use fixture::Fake;

pub fn start(
    c: &mut Controller,
    b: &mut Fake,
    bytes: &[u8],
    facts: Facts,
    epoch: u64,
    now: u64,
) -> Result<(), Error> {
    c.qualify(bytes, facts)?;
    c.start(b, epoch, now)
}
pub fn step(c: &mut Controller, b: &mut Fake, now: u64) -> Result<State, Error> {
    c.step(b, now)
}
pub fn submit(c: &mut Controller, b: &mut Fake, now: u64) -> Result<CommandId, Error> {
    c.submit_noop(b, now)
}
pub fn shutdown(c: &mut Controller, b: &mut Fake, now: u64) -> Result<(), Error> {
    c.shutdown(b, now)
}
pub fn cancel(c: &mut Controller, b: &mut Fake, now: u64) -> Result<(), Error> {
    c.cancel(b, now)
}
