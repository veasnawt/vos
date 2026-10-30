#!/bin/bash
# R2-B native pointer matrix against the production kernel-4, admitted by guard-cap.sh (600 MiB cap).
set -u
cd /home/veasnawt/src/vos-usb2c3b2
B=build/usb2c3b2-1; F=$B/final; G=$F/guard-cap.sh
RUSTC=/home/veasnawt/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc
E=/home/veasnawt/vos-evidence/n36-stage3-gatea-cont-20261021T220119Z/efi-tools/usr/lib/grub/x86_64-efi
D=/home/veasnawt/src/myos/build/app-console-20261025T051241Z/attempt-1/disk.img
M=/home/veasnawt/src/vos-r2b/build/r2b-1/apps.json
step() { name=$1; est=$2; shift 2; $G "$name" "$est" -- "$@" > $F/$name-run.txt 2>&1; rc=$?; echo "$name exit $rc" | tee -a $F/regressions.txt; return $rc; }
step r2b-native-normal 30000000 python3 -I -B scripts/test-ps2-pointer-native.py --mode normal --normal-kernel $B/kernel-4/vos.bin --verify-kernel $B/kernel-4/vos.bin --apps-manifest $M --efi $E --disk $D --out $F/r2b-native-normal
step kernel-4-verify 20000000 make ARCH=x86_64 BUILD=$B/kernel-4-verify RUSTC=$RUSTC TEST_DEFS=-DVOS_GUI_VERIFY verify || exit $?
step r2b-native-verify 30000000 python3 -I -B scripts/test-ps2-pointer-native.py --mode verify --normal-kernel $B/kernel-4/vos.bin --verify-kernel $B/kernel-4-verify/vos.bin --apps-manifest $M --efi $E --disk $D --out $F/r2b-native-verify
echo done
