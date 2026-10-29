#!/bin/bash
# USB.2-C3b.1 regression sequence against kernel-3; every step is admitted by
# guard.sh (566,231,040 B stop line, 20 GiB C: reserve) before it starts.
set -u
cd /home/veasnawt/src/vos-usb2c3b1
B=build/usb2c3b1-1; F=$B/final; G=$F/guard.sh
RUSTC=/home/veasnawt/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc
K=$B/kernel-3; A=$K/rust/libvos_kernel.a
C3A=/home/veasnawt/src/vos-usb2c3a/build/usb2c3a-1/final
step() { name=$1; est=$2; shift 2; $G "$name" "$est" -- "$@" > $F/$name-run.txt 2>&1; rc=$?; echo "$name exit $rc" | tee -a $F/regressions.txt; [ $rc -eq 99 ] && exit 99; }
c3a2_san() {  # the C3a.2 sanitizer harness on the new archive (Rust inputs unchanged; see regressions notes)
  mkdir $F/c3a2-san && gcc -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fno-pie -no-pie -Iarch/x86_64 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    tests/host/usb2c3a2-efi.c $A -o $F/c3a2-san/sanitizer > $F/c3a2-san/sanitizer-build.txt 2>&1 &&
  $F/c3a2-san/sanitizer tests/rust/fixtures > $F/c3a2-san/sanitizer.txt 2>&1
}
step c3a-host 70000000 python3 -I -B scripts/test-usb2c3a.py --archive $A --kernel-map $K/kernel.map --rustc $RUSTC --out $F/c3a-host
step c3a2-san 6000000 c3a2_san
step c2-host 36000000 python3 -I -B scripts/test-usb2c2.py --archive $A --rustc $RUSTC --out $F/c2-host
step c1-host 28000000 python3 -I -B scripts/test-usb2c1.py --archive $A --rustc $RUSTC --out $F/c1-host
step usb2b-host 9000000 python3 -I -B scripts/test-usb-resources.py --archive $A --rustc $RUSTC --out $F/usb2b-host
step usb1-host 2000000 python3 -I -B scripts/test-usb-probe.py --host-only --archive $A --rustc $RUSTC --output $F/usb1-host
step r2b-host 4000000 python3 -I -B scripts/test-ps2-pointer-rust.py --archive $A --rustc $RUSTC --out $F/r2b-host
step r1-bounds 4000000 python3 -I -B scripts/test-elf-bounds.py --archive $A --elfcheck $C3A/r1/elfcheck --user $C3A/r1/user --out $F/r1-bounds
step kernel-3-verify 16000000 make ARCH=x86_64 BUILD=$B/kernel-3-verify RUSTC=$RUSTC TEST_DEFS=-DVOS_GUI_VERIFY verify
step r2b-native 45000000 python3 -I -B scripts/test-ps2-pointer-native.py --normal-kernel $K/vos.bin --verify-kernel $B/kernel-3-verify/vos.bin --apps-manifest /home/veasnawt/src/vos-r2b/build/r2b-1/apps.json --efi /home/veasnawt/vos-evidence/n36-stage3-gatea-cont-20261021T220119Z/efi-tools/usr/lib/grub/x86_64-efi --disk /home/veasnawt/src/myos/build/app-console-20261025T051241Z/attempt-1/disk.img --out $F/r2b-native
step usb2a-core 25000000 python3 -I -B scripts/test-xhci-core.py --rustc $RUSTC --out $F/usb2a-core
echo done
