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

  At the recorded Ubuntu checkpoint, this selected three packages
  (`grub-efi-amd64-bin`, `grub-efi-amd64-unsigned`, `mtools`) without maintainer
  scripts. Dependency and script behavior is version-specific: inspect the proposed
  package transaction on another distribution/release rather than assuming the same
  result. Installing EFI build files is not an instruction to install GRUB on a disk.
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
git clone https://github.com/veasnawt/vos.git && cd vos
git checkout main            # or the checkpoint tag listed in docs/STATUS.md
```

The repository is public. At this documentation review, GitHub main is still the
`c5f5acb` baseline; the locally integrated N3.5 checkpoint has not been pushed.
A clone of GitHub main therefore does not yet contain N3.5. `build/` is generated
and ignored. See [STATUS](STATUS.md) for the local integration/publication state.

## 3. Build

```
make                         # x86_64 kernel: build/x86_64/vos.bin
make iso                     # BIOS ISO: build/x86_64/vos.iso
make uefi-iso                # UEFI ISO: build/x86_64/vos-uefi.iso (needs §1 UEFI tools)
make ARCH=i386               # the legacy i386 kernel
make VOS_TEST=1 iso          # the self-test kernel: build/x86_64-test/
```

**Reproducibility.** The N3.5 evidence proves byte equality between a fresh production
build and a forced rebuild in the **same checkout path**, with unchanged sources,
build flags and recorded toolchain/environment. The integration toolchain included
GCC 15.2.0 (Ubuntu 15.2.0-16ubuntu1), GNU ld 2.46 and GNU Make 4.4.1.
Keep the absolute checkout path, compiler/linker versions, inputs, flags and build
environment fixed when reproducing that comparison: debug ELF files contain source
paths and can differ between checkout locations. The 131 old and eleven ABI v2 ELF
comparisons apply to the retained verified builds, not arbitrary compiler versions.
Universal cross-host, cross-toolchain or cross-path bit identity is not established.
Loaded production content was checked independently of debug metadata. ISO container
bytes are not reproducible because GRUB/xorriso embed timestamps and identifiers;
compare their verified payloads instead. See the [integration summary](results/n3.5-integration.md).

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

### Mutation runner: current host restriction and bounds

The current `scripts/test-file-mutations.py` space guard requires **WSL2 with a
distribution named `Ubuntu`, its live virtual disk backed by Windows C:, and
Windows PowerShell reachable through the standard C: mount**. It fails closed if
that layout or a host-space measurement is unavailable. Native Linux and other WSL
layouts are not supported by this runner's present host guard. This is a test-harness
limitation, not a universal VOS build or runtime requirement.

The runner copies only 167 allowlisted file-test artifacts and required dependency
files from an already verified build. It excludes fixture source trees and unrelated
build outputs. GNU `cp --sparse=always --reflink=never` preserves sparse allocation
without shared copies; failed or dense sparse-file copies are rejected. Symlinks
are preserved and FIFOs recreated without reading them.

It requires a new output directory and matching source/build provenance, runs builds
with one job, and checks Linux free space and at least 10 GiB free on C: plus admission
headroom. Output allocation is limited to 1 GiB, each workspace to 128 MiB, and private
temporary output and individual child files to 64 MiB. Monitoring is sampled, not an
atomic quota. Budget other recovery outputs separately when enforcing an aggregate
phase limit; the runner's own allocation guard covers its output tree.

Repeated `--control` selects explicit controls; omitting it selects all five.
For an existing verified snapshot, use that unchanged snapshot for both source and
verified-build arguments, and invoke the current harness separately. Template only:

```sh
python3 /path/to/vos/scripts/test-file-mutations.py \
  /path/to/verified-source /path/to/verified-source /path/to/new-output \
  --control error-copy --control incarnation --control reserved-inode
```

The full mutation definitions, exact assertions, expected status and read-only disk
settings remain unchanged. Do not reuse an interrupted output directory.

## 5. Architectural decisions in force

- **One x86_64 kernel for both firmware types,** loaded by GRUB through Multiboot2. UEFI is the
  primary future target; Legacy BIOS stays supported.
- **ABI:** v0 and v1 are frozen (`docs/abi/`). File ABI v2 is integrated into local
  main for completed N3.5 at `0ddb201`; formal approval at `0109d9b` is historical
  (section 7). `hello.elf`
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

## 7. Current development checkpoint and review boundary

N3.5 is **COMPLETE** and integrated into local main at
`0ddb201ec1b13d65af7a22a56f35fafc02a3c9ab`; `feat/n3.5-file-api` points to
the same commit. GitHub publication of these changes remains a separate, unperformed
step. The [integration/recovery summary](results/n3.5-integration.md) records the
final checkpoint and evidence provenance.

Historical checkpoints are formal approval at `0109d9b`, documentation closure at
`398ba19`, and the pre-N3.5/resource-comparison baseline `c5f5acb`. The
[closure record](results/n3.5-closure.md) preserves the accepted evidence and
limitations. N3 and N3.6 remain incomplete.
[Plan and limits](plans/n3.5-file-api.md), [ABI v2](abi/syscall-v2.md), and
[Stage 3 evidence](results/n3.5-stage3.md) describe the implementation.
Stages 1/2 provide bounded I/O scopes, nodes, handles and trusted non-root grants.
The separately verified rights-width repair is `4384bb1`; the original Stage 3 draft
was preserved and its integration rehearsed in a separate worktree before resumption.
Stage 3 adds ABI note 2, file calls 6–10, typed close 5, validated wire copies,
versioned runtime and real ring-3 fixtures. No production launch receives a grant
automatically; ordinary hello still boots without storage.

[Stage 4 evidence](results/n3.5-stage4.md) preserves adversarial tests, the
reserved-inode repair, original GRUB failure, rejected experiments and captured
contiguous-allocation failure. Separate mandatory reader/file test images retain
the original 27-module union and every assertion. The user approved Stage 4 closure
at `38ae9e6` and authorized Stage 5.

Accepted [Stage 5 final verification](results/n3.5-stage5.md): 74 full BIOS passes,
19 full fresh `make test-uefi` passes and 19 additional
fresh UEFI configurations. These include default/max CPU at 128 MiB, 1 GiB and
sparse 8 GiB, with fresh VARS, plus production emulated USB/GPT and display cases.
File tests report 845/0; static checks 302/0; five deliberate mutations are detected.
All 139 frozen hashes and 131 old userspace ELFs match. Fresh equivalent c5 builds
confirm cumulative static/reserved growth of 43808/57344 B, within the original
65536/131072 B limits. Production bytes reproduce; no source change was needed.

The EFI tooling gap is resolved for this verification using authorized ordinary-user
download/extraction of `grub-efi-amd64-bin` and `grub-efi-amd64-unsigned`
2.14-2ubuntu1. Existing mtools and OVMF were reused. No sudo, system package
installation, host bootloader or firmware operation was performed. For another
checkout, use section 1's nonprivileged extraction procedure and substitute your
own extraction directory:

```sh
export GRUB_EFI_DIR="/path/to/extracted-tools/usr/lib/grub/x86_64-efi"
export PATH="/path/to/extracted-tools/usr/bin:$PATH"
make test-uefi
```

These are placeholders, not retained paths required by VOS. Install packages only
with separate explicit approval. Stage 4's selected-loader
limitation is historical: full fresh UEFI verification has now run. The final report
contains all configurations, hashes, measurements, host-audit failures/corrections,
artifact paths and remaining QEMU/polling/immutable-media/reset/stack-proof limits.

Closure accepts QEMU-only verification; physical hardware remains untested.
The filesystem interface remains read-only and requires trusted immutable media.
Full dynamic kernel-stack high-water proof remains incomplete; compiler frame sums
and guard tests are partial evidence. OVMF still polls without IOAPIC/MSI/MSI-X.
Finite work is not a real-time/fairness guarantee, and unconfirmed reset may retain
bounded DMA quarantine. No broader hardware or authenticated-media claim follows.

### N3.6 preparation — planning only

N3.6 is the next planned storage milestone, not started. [Preparation](plans/n3-storage.md#11-n36-preparation-planning-only)
records remaining stress/resource-closure objectives, baseline commits, unchanged
cumulative budgets, regression requirements and Stage 1 design questions.
First review deterministic long-run schedules, cold/deep workload availability,
orphan/reset outcomes and the required stack evidence. Keep both reader/file images
within the 128 MiB gates and preserve all frozen contracts.

Next steps are review of the publication documentation, a separately authorized
documentation commit/publication decision, and proposed N3.6 design preparation.
No push, release tag, N3.6 implementation, physical test or writable storage is
authorized by this handoff.

## 8. Physical hardware: restrictions

**No physical machine has run VOS.** Before any hardware test (H2):
- get explicit approval for every step;
- never modify Windows partitions, internal disks or firmware settings without that approval;
- never run `grub-install`, `efibootmgr` or disk-writing commands on the host;
- write a USB stick only after identifying the device beyond doubt, then verify it by reading
  it back;
- back up the BitLocker recovery key before any boot-mode change.

An H1 readiness audit was reported for the original development machine; it does
not establish that VOS boots on physical hardware. Before H2, prepare a
platform-specific checklist covering firmware support, the installed system's boot
mode, recovery and diagnostic output. Changing firmware mode can prevent the
installed system from booting until its original settings are restored; do not
assume a mode change is harmless. Keep individual machine inventories and recovery
material outside public setup examples.
