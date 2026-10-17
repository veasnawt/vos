# MyOS

Minimal freestanding x86 kernel booted by GRUB.
On boot it prints `Hello from MyOS!` to the VGA text buffer and COM1 serial.

## Requirements
gcc (with `gcc-multilib`), binutils, make, grub-mkrescue (+ grub-pc-bin), xorriso, qemu-system-i386.

## Architectures
Select the target with `ARCH=<name>`; the default is `i386`.

| ARCH   | Boot protocol | Status |
|--------|---------------|--------|
| `i386` | Multiboot 1   | 32-bit protected mode, default |

Build output goes to `build/<ARCH>/`.

## Build and test
    make                  # build/i386/myos.bin (same as ARCH=i386)
    make verify           # grub-file Multiboot header check
    make iso              # build/<ARCH>/myos.iso
    make test             # headless QEMU boot test for the selected ARCH
    make run              # QEMU with a window
    make clean            # remove build/ (all architectures)
    make ARCH=i386 test   # explicit architecture selection

## Layout
- `Makefile` – shared build; includes `arch/<ARCH>/arch.mk`
- `arch/i386/arch.mk` – i386 flags, QEMU binary, test recipe
- `arch/i386/boot.S` – Multiboot header, stack, entry point
- `arch/i386/kernel.c` – `kernel_main`, serial + VGA output
- `arch/i386/linker.ld` – loads kernel at 1 MiB
- `arch/i386/grub.cfg` – GRUB menu entry
- `scripts/test.sh` – headless QEMU test (passes only on matching serial output)

The kernel is freestanding (no libc, no host OS dependency at runtime).
The `m1-32bit` tag marks the original known-good 32-bit baseline.
