use vos_xhci_init::*;
fn main() { let mut m = Machine::new(ReleasePolicy::Never); m.phase = Phase::Running; }
