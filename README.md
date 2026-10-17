# MyOS

Minimal 32-bit x86 freestanding kernel, booted by GRUB via Multiboot 1.
On boot it prints `Hello from MyOS!` to the VGA text buffer and COM1 serial.

## Requirements
gcc (with `gcc-multilib`), binutils, make, grub-mkrescue (+ grub-pc-bin), xorriso, qemu-system-i386.

## Build and test
    make          # build/myos.bin
    make verify   # grub-file --is-x86-multiboot
    make iso      # build/myos.iso
    make test     # headless QEMU boot, serial captured to build/serial.log
    make run      # QEMU with a window
    make clean    # remove build/

## Layout
- `src/boot.S` – Multiboot header, stack, entry point
- `src/kernel.c` – `kernel_main`, serial + VGA output
- `src/linker.ld` – loads kernel at 1 MiB
- `scripts/test.sh` – headless QEMU test (passes only on matching serial output)
- `iso/boot/grub/grub.cfg` – GRUB menu entry

The kernel is freestanding (no libc, no host OS dependency at runtime).
