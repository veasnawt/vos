#!/bin/bash
# USB.2-C3b.2 native matrices: the QEMU test image with the execute option, then the production kernel
# (which must ignore it and behave as before). Each is admitted by guard.sh against the storage stop line.
set -u
cd /home/veasnawt/src/vos-usb2c3b2
B=build/usb2c3b2-1; F=$B/final
E=/home/veasnawt/vos-evidence/n36-stage3-gatea-cont-20261021T220119Z/efi-tools/usr/lib/grub/x86_64-efi
D=/home/veasnawt/src/myos/build/app-console-20261025T051241Z/attempt-1/disk.img
A=/home/veasnawt/src/vos-usb2c3a/build/usb2c3a-1/final/apps
$F/guard.sh native-test 45000000 -- python3 -I -B scripts/test-usb2c3a2-native.py --kernel $B/contain-test-7/vos.bin --flavour test \
  --apps $A --efi $E --disk $D --expect execution --compress-traces --out $F/native-test > $F/native-test-run.txt 2>&1
echo "native-test exit $?" | tee -a $F/regressions.txt
$F/guard.sh native-prod 45000000 -- python3 -I -B scripts/test-usb2c3a2-native.py --kernel $B/kernel-4/vos.bin --flavour production \
  --apps $A --efi $E --disk $D --expect containment --compress-traces --out $F/native-prod > $F/native-prod-run.txt 2>&1
echo "native-prod exit $?" | tee -a $F/regressions.txt
