# VOS

**VOS (Veasna OS) is an open-source, AI-assisted, from-scratch operating-system project for x86 PCs.**

VOS is being developed with human architectural direction, review, and testing supported by AI coding assistance. The goal is to grow an independent kernel into a usable general-purpose operating system. It is currently **pre-alpha kernel research**, not a desktop OS or a production-ready release. See the [verified status](docs/STATUS.md) and [roadmap](docs/ROADMAP.md) before interpreting capability claims.

[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue)](LICENSE)

VOS is written primarily in freestanding C and x86 assembly. It is **not a Linux distribution**, and it does not rely on Linux, a host operating system, or a hosted C library at runtime. GRUB currently loads the kernel through Multiboot2 on x86-64. A small legacy i386 build remains available.

**Status: pre-alpha kernel research.** VOS is not an installable or everyday-use operating system. The documented checkpoint has been tested in **QEMU with SeaBIOS and OVMF**, not verified as a functioning physical-machine installation. No native graphical desktop, keyboard/mouse input, interactive shell, writable storage, or network stack is available yet.

### What works today

- A 64-bit higher-half kernel with memory isolation, W^X mappings, and protected stacks.
- Separately built ELF user programs running in ring 3, system calls, kernel threads, timer preemption, and IPC channels.
- Read-only virtio-blk access, block caching, and a **kernel-internal** ext2 reader (not a userspace file API).
- A basic framebuffer boot/status/panic console where supported, plus serial diagnostics.
- BIOS and UEFI boot paths **in QEMU** via GRUB; extensive emulator-based regression tests.

The separate [Veasna OS](https://github.com/veasnawt/veasna-os) React/Electron desktop prototype is **not** a graphical environment running on this kernel.

**Start here:** [Current status](docs/STATUS.md) · [Roadmap](docs/ROADMAP.md) · [Development handoff](docs/HANDOFF.md) · [Contribution guide](CONTRIBUTING.md) · [Security policy](SECURITY.md).

### Quick start (BIOS / QEMU)

On Ubuntu, Debian, or WSL2, install the toolchain and emulator prerequisites listed below and in [docs/HANDOFF.md](docs/HANDOFF.md), then run:

~~~sh
make iso
make run
~~~

For an automated check, run `make test-all`. A UEFI ISO and additional OVMF tests are available using `make uefi-iso` and `make test-uefi` after installing the additional UEFI build/test tools. VOS is intended for **emulation first**; do not flash experimental images onto a disk holding important data.

## What happens when x86_64 VOS boots

1. GRUB loads the kernel (`vos.bin`) and the program `hello.elf` as a boot module.
2. The kernel switches to 64-bit mode, sets up its own memory management and CPU protections,
   and checks the boot information. It then maps itself W^X: only its code is executable, its
   code and constants are read-only, and its stacks have unmapped guard pages below them.
   It sets up the interrupt controller and the timer (100 Hz); interrupts stay off inside the
   kernel and are on while the program runs.
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

    app: module "hello" at 0x1c5000-0x1c8348, 13128 bytes
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

`make ARCH=x86_64 run` boots the same image in a QEMU window, which can show the
framebuffer boot diagnostics and status display; the user program's detailed output is on the serial port.
`make ARCH=x86_64 debug` starts the same image halted, waiting for a debugger on TCP port 1234
(GDB is not part of the requirements; see `docs/plans/n2-foundation.md`, section 8).

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
- kernel self-tests (boot information, memory managers, page tables, the W^X kernel map and its
  checker, user mode, ELF loading);
- deliberate kernel faults, which must panic with the expected diagnostics;
- stress tests (since N2.5): mixed concurrent workloads of the programs above with kills,
  faults, full tables and recovery, while the kernel audits its scheduler, process and channel
  state;
- IPC tests (since N2.4): separately built programs exchanging messages over channels in
  scenarios set up by the test configuration (ordering, full queues, closing, killed receivers,
  rights, ABI v0 compatibility);
- multi-process tests (since N2.3): separately built programs running at the same time —
  memory isolation, fair sharing, endless loops beside normal programs, faults, limits;
- kernel-thread tests (since N2.2): threads alternating, registers and stacks surviving switches,
  blocking and waking, thread exit and stack reclamation, the idle thread taking timer interrupts,
  and failure cases (thread stack overflow, deadlock);
- timer-interrupt tests (since N2.1): separately built programs that loop forever, check their
  registers or try to switch interrupts off, run while the timer interrupts them; a test-only
  tick budget in the kernel ends them;
- kernel memory-protection cases (since N2.0): the kernel writing its own code or constants,
  executing data or a free page (through a 2 MiB alias page and, since N3.0, through the 4 KiB
  pages above the image), reading a stack guard page, overflowing its stack, and NMI and
  machine-check entry (one NMI injected through the QEMU monitor); `scripts/check-fault.sh`
  compares the CPU-reported fault with what each case expected;
- an isolation suite of 41 small programs that try forbidden things (write kernel memory,
  execute data, run privileged instructions, pass bad pointers, ...) and must each end exactly
  as their test configuration says, with `hello` run again after each one; it runs with 128 MiB,
  2 GiB and `-cpu max` (SMEP, SMAP, UMIP);
- static tests of the executable format, including about 60 deliberately corrupted files;
- virtio-blk tests (since N3.1): a generated read-only test disk (`build/vblk/disk.img`) is
  attached through QEMU's `virtio-blk-pci` and also delivered by GRUB as a module. Every
  sector read through the driver must equal that independent copy. The tests also cover
  range, buffer and queue limits, injected device errors (QEMU `blkdebug`), timeouts with
  reset and recovery, forged completions, modern and transitional devices, devices the driver
  must refuse, and reads beside running programs. The disk's SHA-256 is checked after every
  run;
- block-layer tests (since N3.2): a test-only RAM device in every image, and the virtio-blk
  disk in the disk images. They cover the device registry, request validation, the read-only
  cache (counted hits, misses and evictions, LRU, exhaustion, failed loads, invalidation after
  resets, corruption detection), concurrent readers, and shutdown and reinitialization with
  frame accounting;
- ext2 tests (since N3.3): six deterministic ext2 images built with `mke2fs -d`
  (`scripts/mkext2fixtures.sh`; 1, 2 and 4 KiB blocks, htree directories, no features, empty).
  Every file, directory and link is compared with a manifest written on the host from the
  source tree (SHA-256 per file). The tests also cover path errors, 46 corrupted or failing
  images refused for the intended reason, device reset and removal, concurrent readers, and
  the 1 KiB image read through virtio-blk beside running programs.

## Layout

- `Makefile` – shared build; includes `arch/<ARCH>/arch.mk` and, for x86_64, `user/user.mk`
- `arch/i386/` – the 32-bit kernel
- `arch/x86_64/` – the 64-bit kernel: boot (`boot.S`), CPU and interrupts (`cpu.c`, `gdt.c`,
  `idt.c`, `entry.S`, `exception.c`, `irq.c`, `pic.c`, `pit.c`), kernel threads (`thread.c`,
  `thread_switch.S`), memory (`pmm.c`, `vmm.c`, `kmap.c`, `resv.c`, `bootinfo.c`),
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
- `docs/abi/syscall-v1.md` – ABI v1 (N2.4): yield and message channels between processes
- `docs/plans/n1-user-space.md` – the completed N1 milestone: design, stages and verified results
- `docs/architecture/native-software-platform.md` – the long-term native software architecture
  (proposal), with the current state and security boundaries in its section 3
- `docs/plans/n2-foundation.md` – the N2 roadmap; N2.0 (kernel W^X, protected stacks), N2.1
  (interrupt controller, timer), N2.2 (kernel threads, scheduler), N2.3 (several processes,
  timer preemption), N2.4 (channels, ABI v1) and N2.5 (stress, audits, closure) are done: N2
  is complete; its closure (section 3.7) lists guarantees, limits and risks
- `docs/plans/n3-storage.md` – the N3 roadmap (storage, proposal); N3.0 (storage design, kernel
  map extended to 4 MiB of 4 KiB pages), N3.1 (read-only, polled virtio-blk driver), N3.2
  (block layer and read-only cache) and N3.3 (read-only ext2 reader) are done
- `docs/architecture/storage.md` – the storage architecture proposal: layers, virtio-blk, ext2
  (read-only first), layout, naming, permissions, crash behaviour, open decisions
- `docs/vision/product-direction.md` – provisional long-term product direction (not a specification)

## Community, license, and support

VOS is licensed under the [GNU General Public License version 3 only](LICENSE)
(§GPL-3.0-only§). You can study and contribute to the project under that license's terms.
See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution expectations and attribution,
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community guidelines, and
[SECURITY.md](SECURITY.md) for private security reporting.

This is an experimental research kernel. **No warranty or production security guarantees**
are provided. The [status report](docs/STATUS.md) is the source of truth for tested
capabilities and unsupported hardware. Please do not mistake a checkpoint tag for
an end-user operating-system release.

## History

VOS was developed under the working name MyOS until 2026-10-17. Commits up
to `5fd25cc` (including the `m1-32bit` tag) use the old name, for example in
boot messages, artifact names (`myos.bin`, `myos.iso`) and the `MYOS_TEST`
build variable, which is now `VOS_TEST`.
