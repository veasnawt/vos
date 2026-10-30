#!/bin/sh
# Build the model rlib and the test binary into build/usb2c41-1/scratch (overwritten in place).
set -e
cd /home/veasnawt/src/vos-usb2c41
RUSTC=${RUSTC:-/home/veasnawt/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc}
S=build/usb2c41-1/scratch
$RUSTC --edition=2024 -Dwarnings --crate-name vos_xhci_init --crate-type=rlib -Coverflow-checks=yes -Cdebug-assertions=yes kernel/rust/usb/xhci_init/lib.rs -o $S/libvos_xhci_init.rlib
$RUSTC --edition=2024 -Dwarnings --test -Copt-level=1 -Coverflow-checks=yes --extern vos_xhci_init=$S/libvos_xhci_init.rlib tests/rust/xhci_init_model.rs -o $S/init-tests
