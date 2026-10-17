# VOS

Minimal freestanding x86 kernel booted by GRUB.
On boot it prints `Hello from VOS!` to the VGA text buffer and COM1 serial.

## Requirements
gcc (with `gcc-multilib`), binutils, make, grub-mkrescue (+ grub-pc-bin), xorriso,
qemu-system-i386 and qemu-system-x86_64. The host gcc is used for both targets.

## Architectures
Select the target with `ARCH=<name>`; the default is `i386`.

| ARCH     | Boot protocol | Status |
|----------|---------------|--------|
| `i386`   | Multiboot 1   | 32-bit protected mode (default) |
| `x86_64` | Multiboot2    | 32-bit stub enters long mode; kernel runs in the higher half at `0xFFFFFFFF80000000` |

Build output goes to `build/<ARCH>/`; the x86_64 self-checking test image goes to
`build/x86_64-test/`.

## Build and test
    make                     # build/i386/vos.bin (same as ARCH=i386)
    make ARCH=x86_64         # build/x86_64/vos.bin
    make ARCH=x86_64 verify  # grub-file Multiboot2 header check
    make ARCH=x86_64 iso     # build/x86_64/vos.iso
    make ARCH=x86_64 run     # QEMU with a window
    make test                # headless QEMU tests for the selected ARCH
    make ARCH=x86_64 test    # x86_64 tests (see below)
    make test-all            # tests for every architecture
    make clean               # remove build/ (all architectures)

### What the tests check
- `i386`: boots the production image and requires the expected serial lines
  (`scripts/test.sh`).
- `x86_64`:
  1. boots the production image (which halts) and requires `boot: checks passed`
     on serial with QEMU still running afterwards (`scripts/test.sh`);
  2. builds a test image with `VOS_TEST=1`, which reports its result through
     QEMU's `isa-debug-exit` device, and runs it with 128 MiB and 2 GiB;
  3. runs the test image with `-cpu qemu32` (no long mode), which must stop
     with `CPU lacks long mode`.

  `scripts/exit-test.sh` passes only if QEMU exits with the expected status
  (33 = pass, 35 = expected failure) AND the expected serial marker is present.
  Timeouts, other exit statuses, missing markers and triple faults fail; QEMU
  runs with `-no-reboot`, so a reset loop also fails.

## Layout
- `Makefile` – shared build; includes `arch/<ARCH>/arch.mk`
- `arch/i386/` – Multiboot 1 kernel: `boot.S`, `kernel.c`, `linker.ld`, `grub.cfg`, `arch.mk`
- `arch/x86_64/boot.S` – Multiboot2 header, 32-bit stub (CPU checks, boot page
  tables, long-mode switch), higher-half entry, `.bss` zeroing
- `arch/x86_64/kernel.c` – `kernel_main`: serial + VGA output, Multiboot2 parsing,
  boot-time physical reservations (kernel image, Multiboot2 info), self-checks
- `arch/x86_64/multiboot2.h` – Multiboot2 boot information structures
- `arch/x86_64/linker.ld` – low boot area at 1 MiB, higher-half kernel, no RWX segments
- `arch/x86_64/grub.cfg`, `arch/x86_64/arch.mk`
- `scripts/test.sh` – headless smoke test for images that halt
- `scripts/exit-test.sh` – strict test for `VOS_TEST=1` images (exit status + marker)

The kernel is freestanding (no libc, no host OS dependency at runtime).
The `m1-32bit` tag marks the original known-good 32-bit baseline.

## Documentation
- `docs/architecture/native-software-platform.md` – proposed native application model,
  SDK/ABI, security, packaging and compatibility architecture (design only, not implemented)

## History
VOS was developed under the working name MyOS until 2026-10-17. Commits up
to `5fd25cc` (including the `m1-32bit` tag) use the old name, for example in
boot messages, artifact names (`myos.bin`, `myos.iso`) and the `MYOS_TEST`
build variable, which is now `VOS_TEST`.
