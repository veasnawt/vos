#!/bin/bash
# USB.2-C3b.2 regression sequence against the production kernel-4, in priority order (the suites that exercise code
# this milestone touched first). Each step is admitted by guard.sh against the 566,231,040 B stop line; a refused
# step exits 99 and the sequence stops there.
set -u
cd /home/veasnawt/src/vos-usb2c3b2
B=build/usb2c3b2-1; F=$B/final; G=$F/guard.sh
RUSTC=/home/veasnawt/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc
K=$B/kernel-4; A=$K/rust/libvos_kernel.a
C3A=/home/veasnawt/src/vos-usb2c3a/build/usb2c3a-1/final
step() { name=$1; est=$2; shift 2; $G "$name" "$est" -- "$@" > $F/$name-run.txt 2>&1; rc=$?; echo "$name exit $rc" | tee -a $F/regressions.txt; [ $rc -eq 99 ] && exit 99; }
step c3a-host   70000000 python3 -I -B scripts/test-usb2c3a.py --archive $A --kernel-map $K/kernel.map --rustc $RUSTC --out $F/c3a-host
step c1-host    28000000 python3 -I -B scripts/test-usb2c1.py --archive $A --rustc $RUSTC --out $F/c1-host
step c3b1-host  30000000 python3 -I -B scripts/test-usb2c3b1.py --kernel-build $K --rustc $RUSTC --out $F/c3b1-host --prune-mutants
step c3a2-san    6000000 bash -c "mkdir $F/c3a2-san && gcc -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections -fno-pie -no-pie -Iarch/x86_64 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer tests/host/usb2c3a2-efi.c $A -o $F/c3a2-san/sanitizer > $F/c3a2-san/sanitizer-build.txt 2>&1 && $F/c3a2-san/sanitizer tests/rust/fixtures > $F/c3a2-san/sanitizer.txt 2>&1"
step c2-host    36000000 python3 -I -B scripts/test-usb2c2.py --archive $A --rustc $RUSTC --out $F/c2-host
step usb2b-host  9000000 python3 -I -B scripts/test-usb-resources.py --archive $A --rustc $RUSTC --out $F/usb2b-host
step usb1-host   2000000 python3 -I -B scripts/test-usb-probe.py --host-only --archive $A --rustc $RUSTC --output $F/usb1-host
step r2b-host    4000000 python3 -I -B scripts/test-ps2-pointer-rust.py --archive $A --rustc $RUSTC --out $F/r2b-host
step r1-bounds   4000000 python3 -I -B scripts/test-elf-bounds.py --archive $A --elfcheck $C3A/r1/elfcheck --user $C3A/r1/user --out $F/r1-bounds
echo done
