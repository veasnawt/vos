use vos_xhci::xhci::survey::*;
fn f(m: &mut impl Mmio) { m.write32(0, 1); }
fn main() {}
