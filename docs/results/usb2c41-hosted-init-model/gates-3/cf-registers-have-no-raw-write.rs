use vos_xhci_init::*;
fn main() { let _ = Operation::WriteRegister { offset: 0x38, value: 1 }; }
