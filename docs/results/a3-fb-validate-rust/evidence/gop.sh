#!/bin/sh
OVMF=1 COLOR=2ea043 scripts/halt-test.sh uefi-640x480 build/a3/kernel/vos-uefi.iso build/a3/kernel/vos.bin 35 main 'screen: framebuffer 640x480 BGRX' 'boot: checks passed' -- -vga none -device VGA,edid=on,xres=640,yres=480 > build/a3/r2a/uefi-640x480.txt 2>&1
echo "uefi-640x480 exit=$?" >> build/a3/r2a/uefi-640x480.txt
OVMF=1 COLOR=2ea043 scripts/halt-test.sh uefi-1920x1080 build/a3/kernel/vos-uefi.iso build/a3/kernel/vos.bin 35 main 'screen: framebuffer 1920x1080 BGRX' 'boot: checks passed' -- -vga none -device VGA,edid=on,xres=1920,yres=1080 > build/a3/r2a/uefi-1920x1080.txt 2>&1
echo "uefi-1920x1080 exit=$?" >> build/a3/r2a/uefi-1920x1080.txt
