# Resuming VOS development

A practical guide for picking VOS up again after a break. The current state and test results
are in `docs/STATUS.md`, and the plan is in `docs/ROADMAP.md`.

**Scope.** VOS here means the native, independently written x86_64 kernel in this repository.
The separate React/Electron "Veasna OS" desktop prototype is a different project: it is not
the native VOS kernel and not its desktop environment.

## 1. Tools (Ubuntu or Debian, also WSL2)

- **Build:** gcc with gcc-multilib (the i386 kernel and 32-bit stubs), binutils, make, python3.
- **BIOS images:** grub-mkrescue with grub-pc-bin, and xorriso.
- **Emulation:** qemu-system-x86_64 and qemu-system-i386.
- **Test fixtures:** e2fsprogs (the ext2 images; `scripts/mkext2fixtures.sh`).
- **UEFI images and tests:** GRUB's x86_64-efi files, mtools, and OVMF (package `ovmf`,
  providing `/usr/share/OVMF/OVMF_CODE_4M.fd` and `OVMF_VARS_4M.fd`). Install the GRUB EFI
  files and mtools once, without recommended packages:

  ```
  sudo apt-get install --no-install-recommends grub-efi-amd64-bin mtools
  ```

  This installs exactly three packages (`grub-efi-amd64-bin`, `grub-efi-amd64-unsigned`,
  `mtools`). They have no maintainer scripts and touch no boot loader, disk or EFI variable.
  A plain install, with recommends, would also pull in `grub-efi-amd64`, `grub-efi-amd64-signed`
  and `secureboot-db`, which run `grub-install` or update EFI variables: avoid them.
- **Without sudo:**
  1. Download the three packages with `apt-get download`.
  2. Unpack each into a scratch directory with `dpkg-deb -x <deb> <dir>`.
  3. Set `GRUB_EFI_DIR=<dir>/usr/lib/grub/x86_64-efi` and put `<dir>/usr/bin` on `PATH`.

  The checkpoint used grub-efi-amd64-bin 2.14-2ubuntu1, mtools 4.0.49-1, QEMU 10.2.1 and
  OVMF 2025.11.

## 2. Repository

```
git clone git@github.com:veasnawt/vos.git && cd vos
git checkout main            # or the checkpoint tag listed in docs/STATUS.md
```

The repository was private at the U1 checkpoint; check GitHub for its current visibility. Authorized users can clone via SSH, and public users may clone via HTTPS once the repository is published. `build/` is generated and ignored. An offline Git bundle of the
checkpoint may exist outside the repository; `docs/STATUS.md` names it.

## 3. Build

```
make                         # x86_64 kernel: build/x86_64/vos.bin (byte-reproducible)
make iso                     # BIOS ISO: build/x86_64/vos.iso
make uefi-iso                # UEFI ISO: build/x86_64/vos-uefi.iso (needs §1 UEFI tools)
make ARCH=i386               # the legacy i386 kernel
make VOS_TEST=1 iso          # the self-test kernel: build/x86_64-test/
```

**Reproducibility.** Kernel and ELF binaries are byte-reproducible. ISO images are not,
because GRUB and xorriso embed timestamps and UUIDs, so compare their contents instead.

## 4. Tests (headless QEMU, run one suite at a time)

- `make test-all`: the full BIOS regression for i386 and x86_64. It is a superset of
  `make test` and `make ARCH=x86_64 test`, which are the same thing. About 25 minutes.
- `make test-uefi`: the UEFI matrix on QEMU q35 with OVMF, Secure Boot not enabled, a fresh
  variable store per run. About 10 minutes.
- **How a run passes:**
  - Test kernels report `VOS-TEST: PASS` and exit through QEMU's isa-debug-exit device.
  - Production kernels halt and are judged by `scripts/halt-test.sh` from their final CPU
    state. A timeout never counts as a pass.
  - Self-tests report PASS/FAIL counts and, separately, NOT APPLICABLE checks that the
    machine cannot exercise; each states its reason.
- **Interactive runs:** `make run` (BIOS). For UEFI, run `qemu-system-x86_64 -machine q35`
  with the two OVMF pflash drives and `-cdrom build/x86_64/vos-uefi.iso`; see `exit-test.sh`
  `OVMF=1` for the exact options.

## 5. Architectural decisions in force

- **One x86_64 kernel for both firmware types,** loaded by GRUB through Multiboot2. UEFI is the
  primary future target; Legacy BIOS stays supported.
- **ABI:** v0 and v1 are frozen (`docs/abi/`). File ABI v2 is reserved for N3.5. `hello.elf`
  must stay byte-identical.
- **Kernel model:** non-preemptible, running with IF = 0. Interrupts are taken only in ring 3
  or in the idle halt.
- **Memory:** RAM below 1 GiB only. W^X kernel map. Device memory only through the audited
  device window (`devmap.h`), never RAM.
- **Loader data** (boot information, modules) may lie below 1 MiB, never in page 0 or the
  legacy hole (`docs/plans/uefi.md` §8.2).
- **Screen:** a framebuffer console with an original 8×16 font. VGA text only when text mode is
  positively known. Serial (COM1, 38400 8N1) is the primary log.
- **Storage:** read-only throughout (virtio-blk, block cache, ext2); no write path exists.
- **The i386 kernel is legacy:** `arch/i386/` is not modified, and its `vos.bin` hash is fixed.

## 6. Known limitations

The full list is in `docs/STATUS.md`. In short:
- QEMU only, no physical hardware yet.
- No Secure Boot.
- Under UEFI, virtio-blk polls: no IOAPIC/MSI.
- Only the 32-bpp BGRX framebuffer format.
- No keyboard, shell, writable file system or networking.

## 7. Suggested next milestone

N3.5, the read-only user-space file API, now has a **PROPOSED** design submitted for review
on 2026-10-20: [implementation plan and test matrix](plans/n3.5-file-api.md) and
[ABI v2 draft](abi/syscall-v2.md). Base: `c5f5acb`; local branch: `feat/n3.5-file-api`.
The overall direction is conditionally approved. Stage 1 bounded storage foundations are
implemented and tested locally; [results and exact verification limits](results/n3.5-stage1.md).
The refined design checkpoint is `f7b9efa`. The verified Stage 1 commit stops here: file
handles, new syscalls and later stages require the next review (plan section 13).
U1/N3.4 remain the latest completed milestones. U2 is the alternative future interrupt
controller milestone in `docs/ROADMAP.md`, also not started.

The design-phase unprivileged simulation of
`apt-get -s --no-install-recommends install grub-efi-amd64-bin mtools` selected the same
three packages as section 1. OVMF is present; EFI modules and mtools are missing from
standard paths. No sudo or installation was performed. Obtain separate explicit approval
before installing packages or running sudo; never change host bootloader settings.

## 8. Physical hardware: restrictions

**No physical machine has run VOS.** Before any hardware test (H2):
- get explicit approval for every step;
- never modify Windows partitions, internal disks or firmware settings without that approval;
- never run `grub-install`, `efibootmgr` or disk-writing commands on the host;
- write a USB stick only after identifying the device beyond doubt, then verify it by reading
  it back;
- back up the BitLocker recovery key before any boot-mode change.

**The H1 readiness audit of the Dell OptiPlex 9020** was reported but not written into the
repository. Its key facts:
- **CPU:** the i5-4570 (Haswell) has NX and SMEP, but no SMAP or UMIP.
- **Firmware:** BIOS A12. Windows 10 boots it in Legacy mode from an MBR disk.
- **UEFI boot:** needs the BIOS "Boot List Option" switched to UEFI. Windows does not start
  while it is set, and starts again once it is switched back; nothing is lost.
- **Serial port:** not verified. On the MT and SFF models, COM1 is likely.
- **Diagnostics:** the screen console (U1) now shows the whole boot log and PASS/FAIL without a
  serial cable.
