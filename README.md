# VOS

VOS is an experimental, independently written operating-system kernel for x86 PCs. It is
**not** a Linux distribution and does not use Linux, a C library or any host operating system
at run time; GRUB loads it directly.

Today VOS can boot, protect itself from the programs it runs, and run one separately built
native program (`hello`) in user mode. It has **no** graphical desktop, interactive shell,
keyboard input, file system, networking, timer, scheduler or multitasking. It has only been
tested in the QEMU emulator.

## What happens when x86_64 VOS boots

1. GRUB loads the kernel (`vos.bin`) and the program `hello.elf` as a boot module.
2. The kernel switches to 64-bit mode, sets up its own memory management and CPU protections,
   and checks the boot information.
3. It validates `hello.elf`, copies it into a fresh, isolated user address space and runs it in
   ring 3 (user mode).
4. `hello` prints through the kernel's `debug_write` system call and exits with status 0.
5. The kernel takes back every page the program used, reports the result, and halts.

## Requirements

gcc (with `gcc-multilib`), binutils, make, grub-mkrescue (+ grub-pc-bin), xorriso,
qemu-system-i386 and qemu-system-x86_64. The host gcc builds both kernels and the user
programs, with flags that keep them freestanding.

## Architectures

Select the target with `ARCH=<name>`; the default is `x86_64` (since N2). The 32-bit `i386`
kernel is kept, still built and tested with `ARCH=i386`.

| ARCH | Boot protocol | Status |
|---|---|---|
| `i386` | Multiboot 1 | 32-bit protected mode; prints a greeting and halts (legacy, `ARCH=i386`) |
| `x86_64` | Multiboot2 | 64-bit higher-half kernel with memory management, ring-3 programs and system calls (default) |

## Build

    make                          # x86_64 kernel: build/x86_64/vos.bin (same as ARCH=x86_64)
    make ARCH=i386                # legacy i386 kernel: build/i386/vos.bin
    make ARCH=x86_64 user         # the native program: build/x86_64/user/hello.elf
    make ARCH=x86_64 user-check   # validate hello.elf against the VOS executable profile
    make ARCH=x86_64 iso          # bootable image with kernel and hello: build/x86_64/vos.iso
    make clean                    # remove build/ (all architectures)

`hello.elf` is built from `user/hello/hello.c` with the VOS runtime in `user/lib/`, completely
separately from the kernel. `user-check` runs `scripts/check-user-elf.sh`, which reads the ELF
headers directly and prints, for example:

    check-user-elf: build/x86_64/user/hello.elf: OK (3 PT_LOAD, entry 0x400000, 3 pages, VOS ABI 0)

## Boot VOS in QEMU

All program output goes to the serial port. To see it in your terminal (the kernel halts at the
end, so `timeout` stops QEMU after a few seconds):

    make ARCH=x86_64 iso
    timeout 10 qemu-system-x86_64 -cdrom build/x86_64/vos.iso -display none -serial stdio -no-reboot

The last lines should be:

    app: module "hello" at 0x12c000-0x12f348, 13128 bytes
    elf: hello: ET_EXEC entry 0x400000, 3 PT_LOAD, 3 pages, VOS ABI 0
    user[hello]: Hello from VOS!
    proc: exit status 0 (cs=0x23 rip=0x40002a)
    proc[hello]: exited status 0, all frames reclaimed
    boot: checks passed

(Addresses can differ.) When `timeout` stops QEMU it prints
`qemu-system-x86_64: terminating on signal 15 ... (timeout)` and exits with status 124; that
is expected, because the kernel halts instead of powering off. How to read the lines:

- `user[hello]: Hello from VOS!` was written by the program itself. The kernel adds the
  `user[hello]:` prefix to every line a program prints, so programs cannot imitate kernel
  messages.
- `cs=0x23` shows the program ran in ring 3; `rip=0x40002a` is the instruction right after its
  `exit` system call.
- `proc[hello]: exited status 0, all frames reclaimed` is the kernel's own report: the program
  exited with status 0 and every page it used was returned.

`make ARCH=x86_64 run` boots the same image in a QEMU window, which shows the VGA screen
(`Hello from VOS!` from the kernel); the program's output is only on the serial port.

## Tests

    make test                  # the default architecture (x86_64, a few minutes)
    make ARCH=i386 test        # the legacy i386 kernel
    make test-all              # every architecture
    make ARCH=x86_64 user-test # only the static ELF tests (no QEMU)

Every QEMU run is headless, sequential and has a timeout. The x86_64 tests include:

- smoke tests of the production images (`scripts/test.sh`), which must show the lines above;
- test images built with `VOS_TEST=1`, which report pass (exit status 33) or an expected
  failure (35) through QEMU's `isa-debug-exit` device (`scripts/exit-test.sh`); a test passes
  only with the right exit status **and** the expected kernel messages;
- kernel self-tests (boot information, memory managers, page tables, user mode, ELF loading);
- deliberate kernel faults, which must panic with the expected diagnostics;
- an isolation suite of 41 small programs that try forbidden things (write kernel memory,
  execute data, run privileged instructions, pass bad pointers, ...) and must each end exactly
  as their test configuration says, with `hello` run again after each one; it runs with 128 MiB,
  2 GiB and `-cpu max` (SMEP, SMAP, UMIP);
- static tests of the executable format, including about 60 deliberately corrupted files.

## Layout

- `Makefile` – shared build; includes `arch/<ARCH>/arch.mk` and, for x86_64, `user/user.mk`
- `arch/i386/` – the 32-bit kernel
- `arch/x86_64/` – the 64-bit kernel: boot (`boot.S`), CPU and interrupts (`cpu.c`, `gdt.c`,
  `idt.c`, `entry.S`, `exception.c`), memory (`pmm.c`, `vmm.c`, `resv.c`, `bootinfo.c`),
  processes and system calls (`process.c`, `usermode.S`, `syscall.c`, `usercopy.c`), executables
  (`elf.c`, `loader.c`, `app.c`), and `selftest*.c` for test builds
- `user/` – user space: `include/vos/syscall.h`, `lib/` (runtime, ABI note, linker script),
  `hello/`, `suite/` (isolation-test programs), `tests/` (fixtures)
- `tests/x86_64/` – GRUB configurations for special test images
- `scripts/` – test harnesses and ELF tools

The `m1-32bit` tag marks the original known-good 32-bit baseline.

## Documentation

- `docs/abi/syscall-v0.md` – the experimental application ABI: entry state, system calls,
  memory protection, executable format
- `docs/plans/n1-user-space.md` – the completed N1 milestone: design, stages and verified results
- `docs/architecture/native-software-platform.md` – the long-term native software architecture
  (proposal), with the current state and security boundaries in its section 3
- `docs/plans/n2-foundation.md` – proposed next milestone (planning only)
- `docs/vision/product-direction.md` – provisional long-term product direction (not a specification)

## History

VOS was developed under the working name MyOS until 2026-10-17. Commits up
to `5fd25cc` (including the `m1-32bit` tag) use the old name, for example in
boot messages, artifact names (`myos.bin`, `myos.iso`) and the `MYOS_TEST`
build variable, which is now `VOS_TEST`.
