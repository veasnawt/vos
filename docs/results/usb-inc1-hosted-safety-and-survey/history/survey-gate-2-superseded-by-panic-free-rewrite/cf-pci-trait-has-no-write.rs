use vos_xhci::xhci::survey::*;
fn f(p: &mut impl Pci) { p.write32(0, 0, 0, 0, 1); }
fn main() {}
