use vos_xhci_init::*;
fn f<B: Backend>(mut s: Session<B>) { s.fresh = [true; 5]; }
fn main() {}
