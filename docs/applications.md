# Common native application payload (Increment 1)

The accepted N0-N5 baseline is main `5b23fcbc09e81ee182ed3bece11d26b3f560edae`.
This increment packages separate `vsh.elf`, `hello.elf` and `rixie.elf` executables
with one kernel and a bounded private catalog. BIOS and UEFI firmware variants
contain the same kernel and application bytes and select the same shell bootstrap.
There is no application-selection boot menu. Older per-application demonstration
targets remain available for compatibility; the existing default `make iso` path
is unchanged.

VOS targets native physical x86_64 computers. QEMU is verification equipment;
this increment has not been tested on physical hardware.

## Build and boot

After the existing storage/isolation admission, choose a fresh output directory:

```sh
app_build="$PWD/build/app-payload-local"
make apps-uefi-iso APP_BUILD="$app_build" RUSTC=/path/to/accepted/rustc
```

This produces `$app_build/kernel/vos.iso` and `vos-uefi.iso`. They are firmware
packaging variants of one logical system payload, not separate operating systems
for different applications. The UEFI variant uses the existing GRUB EFI tools,
OVMF and mtools environment described in [HANDOFF](HANDOFF.md).
`APP_BUILD` must be nonempty. Do not target protected or previously verified roots.

`make apps-run APP_BUILD="$app_build"` boots the BIOS variant and the existing
Rust shell. The optional existing `VSH_DISK=/path/to/read-only-fixture.img` argument
attaches read-only storage; without an explicit filesystem grant, filesystem
commands correctly report unavailable. The shell supports its existing commands.
`apps` and `rixie` are **not implemented shell commands** in Increment 1.

To replay an already built BIOS image without rebuilding:

```sh
qemu-system-x86_64 -machine q35 -accel tcg -m 128M \
  -cdrom "$app_build/kernel/vos.iso" -boot d -serial stdio -nic none -no-reboot
```

UEFI replay additionally needs read-only OVMF code and a fresh, admitted writable
copy of OVMF variables. Do not reuse verification logs, QMP sockets or writable
variable files from preserved evidence. Recorded command JSON identifies the
tested configuration, not permission to overwrite its outputs. No host disk,
installation, firmware-variable change or physical-machine test is authorized here.

## Catalog contract (kernel-private)

- At most eight entries; names are 1-15 lowercase ASCII letters/digits, underscore
  or hyphen, with a NUL and zero padding. The current set is vsh, hello and rixie.
- Logical identities are nonzero and unique. Names are unique and cannot conflict
  with `help`, `clear`, `echo`, `pwd`, `ls`, `cd`, `cat`, `exit` or reserved `apps`.
- Source metadata is versioned. Unsupported versions/policies, invalid pointers'
  integer ranges, empty/oversized sources and invalid ELF images are rejected.
  Readable kernel memory and disjoint metadata/output buffers are caller contracts.
- The existing ELF validator retains its size, ABI, executable-format and memory
  permission limits. Shell/console policy descriptions require ABI v3; diagnostic
  policy does not grant access merely because an executable uses a newer ABI.
- Policies describe provisioned roles; they are not execution or console grants.
  The existing shell bootstrap installs its existing explicit grants.
- Validation allocates no frames and executes no application. It clears the
  entire output on rejection and publishes entries only after all validate.
- `app_catalog_at` and `app_catalog_find` are bounded kernel-only enumeration/
  lookup. No catalog structures, pointers, Multiboot ordinals or physical addresses
  are exposed to applications. Internal IDs are not a frozen public ABI.

The common boot profile requires exactly the three explicitly provisioned module
labels. It validates the complete catalog before the existing hello/shell path.
Missing, duplicate, unknown or invalid sources prevent payload applications from
running; there is no fallback to an unchecked executable.

Module bytes are borrowed through the existing supervisor-only kernel alias.
Every source page must be covered by boot reservations, whose entries are never
removed. No source bytes are copied into or linked with the kernel; catalog
metadata is 392 bytes. Kernel code remains responsible for not changing retained
sources. Future runtime loading must revalidate the selected executable.

Trusted means provisioned immutable system-image contents, not authenticated boot
or signed installation. Existing kernel/DMA trust limits remain.

## Verification and next boundary

[Focused results and screenshots](results/app-payload.md) cover catalog rejection,
capacity, ELF/application identities, ISO extraction and real BIOS/UEFI shell
interaction. A new optimized host-model baseline is kept separate from sanitizers.

Reproduction requires the commissioned `APP-BUDGET.json` and continuous original
isolation/reserve guards. The workload alone is not an admission controller:

```sh
PYTHONDONTWRITEBYTECODE=1 APP_PHASE_ROOT=/path/to/admitted/phase \
  RUSTC=/path/to/accepted/rustc \
  python3 -B scripts/test-app-payload.py \
  --output /path/to/admitted/phase/attempt-1
```

Use a fresh admitted attempt; do not repeat completed checks unnecessarily.

Increment 2 still needs separate authorization: execution authority, a bounded
kernel execution service, foreground child lifecycle and safe console restoration.
No spawn/exec syscall, process-lifecycle change, console handoff, new shell command,
network grant, filesystem executable loader, installer or desktop was introduced.
