# UEFI boot (U0 feasibility result, U1 proposal)

| | |
|---|---|
| Status | **U0 complete (2026-10-19). U1 complete (2026-10-19, section 8): UEFI and BIOS boots of one kernel with a screen console; final regression and mutation results in section 8.8 and `docs/STATUS.md`.** |
| Starting point | `6769157` (N3.4 complete); no kernel, ABI or legacy build change in U0 |
| Architecture decision | UEFI x86_64 is the primary future boot target; Legacy BIOS remains a supported compatibility path; the Dell OptiPlex 9020 is an early test machine, not the standard |

## 1. Tools

**Packages.** `apt-get -s install grub-efi-amd64-bin mtools` would also install six recommended
packages:
- `grub-efi-amd64` and `grub-efi-amd64-signed`, whose scripts run `grub-install`;
- `secureboot-db`, which updates EFI variables;
- `efibootmgr`, `sbsigntool` and `grub-efi-amd64-unsigned`.

With `--no-install-recommends` only three packages remain: `grub-efi-amd64-bin`,
`grub-efi-amd64-unsigned` (a hard dependency that carries `/usr/lib/grub/x86_64-efi`) and
`mtools`.

**Safety check.** All three have no maintainer scripts (no `preinst`, `postinst`, `prerm`,
`postrm` or triggers). Their files go only to `/usr/lib/grub/x86_64-efi`, `/usr/bin` (the
`m*` tools), `/etc/mtools.conf`, man pages and docs. The WSL system has no `/boot/grub`,
`/boot/efi` or `/sys/firmware/efi`.

**How U0 used them.** `sudo` needs a password, so the packages were not installed system-wide.
The three verified `.deb` files were unpacked with `dpkg-deb -x` into a scratch directory, and
only that copy was used. The same files install system-wide with:

    sudo apt-get install --no-install-recommends grub-efi-amd64-bin mtools

| Tool | Version |
|---|---|
| GRUB x86_64-efi (`grub-efi-amd64-bin`, `-unsigned`) | 2.14-2ubuntu1 (the system i386-pc GRUB is 2.14-2ubuntu2.1) |
| mtools | 4.0.49-1 |
| xorriso | 1.5.6 (installed) |
| QEMU | 10.2.1 |
| OVMF | 2025.11-3ubuntu7.3: `OVMF_CODE_4M.fd` (Secure Boot not enabled); a fresh copy of `OVMF_VARS_4M.fd` per run |

## 2. Image

`scripts/uefi-u0-image.sh` (experimental, not wired into `make`) copies the existing ISO trees
`build/x86_64/isodir` and `build/x86_64-test/isodir`:
- same `vos.bin`, same `hello.elf`;
- hashes checked: production `a480c611…`, test `62f99cb3…`, `hello.elf` `c6c64161…`.

It adds GRUB serial and diagnostic lines to a copy of `grub.cfg` (`serial`, `terminal_output`,
`lsefisystab`, `lsmmap`, `echo`), then runs:

    grub-mkrescue -d <x86_64-efi dir> -o vos-uefi-<image>.iso <isodir copy>

Output goes to `build/x86_64-uefi-u0/`.

**Structure** (xorriso and mtools reports):
- **El Torito:** a single UEFI entry, `/efi.img`. There is no BIOS entry: the image is
  UEFI-only.
- **Partitioning:** a protective MBR (`0xEE`) plus a GPT. Partition 2, "EFI boot partition",
  has type GUID `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` (EFI System Partition) and is mapped
  onto `/efi.img` (5760 sectors).
- **`/efi.img`:** a 2.9 MB FAT image containing `\EFI\BOOT\BOOTX64.EFI`, a 221,184-byte PE32+
  x86-64 EFI application (GRUB's core).
- **ISO 9660 files:** `/boot/vos.bin`, `/boot/hello.elf`, `/boot/grub/grub.cfg`, and GRUB's
  modules and font.

## 3. OVMF results (QEMU q35, Haswell-noTSX CPU model, Secure Boot not enabled)

**How a run is judged.** Every run ends with the QEMU monitor's `info cpus` and
`info registers`. A legitimate end of boot is the CPU halted (`HLT=1`) at
`0xffffffff80107041`, the instruction after the `hlt` that follows `kernel_main`'s return. A
panic halts in `halt_forever` (`0xffffffff80109491`). A timeout alone is never counted.

| Run | Media, RAM | Result |
|---|---|---|
| production | CD, 1 GiB | GRUB EFI starts and loads the kernel and module. VOS gets control ("Hello from VOS!" on serial) and initializes through the boot-information checks, then **`panic: module below 1 MiB`** (halted in `halt_forever`) |
| production | USB stick (xHCI), 8 GiB | same: module at `0x64000`, info at `0x68000`, the same panic |
| production without `module2` (diagnostic) | CD, 1 GiB | full initialization: memory manager, VMM, W^X map, 8259/PIT, scheduler, no virtio-blk, "no module named hello", **`boot: checks passed`**, legitimate halt |
| production without `module2` | USB stick, 8 GiB | same; 7.5 GB above 1 GiB ignored as designed; legitimate halt |
| production without `module2` + virtio-blk disk | CD, 1 GiB | driver initializes in **polling mode** ("interrupt line not level triggered (ELCR)"), legitimate halt |
| main test image without `module2` | CD, 1 GiB | sched 45/0, user 184/0, ipc 30/0, bootinfo, pmm, kmap, blk, ext2 pass. Three failures, all explained below. Exit status 35 (FAIL) |
| current BIOS ISO | USB stick | not booted by OVMF ("Not Found": no EFI boot path), as expected |

**The three test failures** (main test image, no module):
1. `vmm`: "VGA text memory writable through the alias". The legacy VGA text buffer does not
   exist under UEFI.
2. `irq`: "a free slave edge line and a free level line exist". OVMF leaves ELCR = 0, so no
   line is level-triggered.
3. `elf`: "exactly one module named hello". The module was removed for this diagnostic run.

**Answers to the U0 questions:**

1. GRUB EFI starts: yes (`grub: platform efi cpu x86_64`).
2. GRUB recognizes and loads the Multiboot2 kernel: yes, at its fixed physical address
   (segments at `0x100000`–`0x1a0000`). No load error.
3. Control reaches VOS: yes. The 32-bit entry runs and enters long mode in the higher half
   (`rip=0xffffffff801072a8`).
4. The kernel initializes: **no** with the module (U0-B1). Yes without it.
5. CPU state at `_start` (QEMU `-d cpu`), compared with SeaBIOS + GRUB i386-pc. Each column
   comes from the production kernel **with** the `hello` module (UEFI values re-measured in
   the U1 preparation; see the correction note below):

   | | UEFI (GRUB x86_64-efi) | BIOS (GRUB i386-pc) |
   |---|---|---|
   | EAX / EBX | `0x36d76289` / `0x68000` | `0x36d76289` / `0x1fa290` |
   | GDTR base | `0x6c0d0` (GRUB's) | `0x10b0` (GRUB's) |
   | Mode | 32-bit protected, flat CS `0x10` / DS `0x18`, paging off, IF = 0 | the same |
   | CR0 | `0x00010033` (PE, MP, ET, NE, WP) | `0x00000011` |
   | CR4 | `0x00000648` (DE, MCE, OSFXSR, OSXMMEXCPT) | `0` |
   | EFER | `0x800` (NXE) | `0` |
   | IDT | the firmware's (`0x3f493018`, limit `0xfff`) | limit 0 |
   | TR | firmware TSS loaded (`0x48`) | none |
   | Stack | `0x7ff00` | `0x7ff00` |

   The kernel installs its own GDT, IDT, TSS, stack and page tables. It does **not** reset
   CR0/CR4: under UEFI the kernel runs with NE, MP, DE, MCE, OSFXSR and OSXMMEXCPT set
   (`cr0=0x80010035 cr4=0x100668`, BIOS `cr0=0x80010015 cr4=0x100020`). This was harmless in
   every test: CR0.EM stays set, so no x87/SSE instruction runs.
6. **Placement.** The kernel is at `0x100000`–`0x1a0000` as linked. With the module present,
   the module is at `0x64000`–`0x67348` and the boot information at `0x68000`–`0x6a140`.
   These ranges are disjoint (the gap is `0x67348`–`0x68000`). Without the module (the
   diagnostic variant) the boot information is at `0x64000`–`0x66128`. All of them lie
   **below 1 MiB**, inside a region the UEFI map marks available. Under BIOS the boot
   information and module were at `0x1fa290` and `0x1fb000`.

   **Correction (U1 preparation).** The first version of this document combined two runs. It
   gave the boot information as `0x64000`–`0x6a140` (the no-module start with the
   with-module end), so it seemed to overlap the module. It also showed EBX = `0x64000`,
   which was measured on the no-module image. The values above were re-established by
   dumping guest memory `0x60000`–`0x80000` after the panic and decoding the Multiboot2
   structure at EBX:
   - tags 21, 1, 2, 3, 6, 9, 4, 12, 14, 15, 17 and the end tag, 8512 bytes in all;
   - the module bytes have the SHA-256 of `hello.elf`.

   The EFI memory map shows `0x1000`–`0x6c000` as LoaderData (GRUB's allocation, holding both
   objects) and `0x100000`–`0x1a0000` as LoaderData (the kernel).
7. **Memory assumptions:**
   - Still valid:
     - everything VOS needs is below 1 GiB;
     - the converted map (20–21 entries, with ACPI NVS and reclaim, firmware code, and the
       ECAM window `0xe0000000`) is handled;
     - reserved types are protected;
     - RAM above 1 GiB is ignored.
   - **Broken:** "modules are never below 1 MiB" (U0-B1).
8. Serial output: works throughout (COM1, 38400 8N1), both the kernel's and GRUB's own (each
   GRUB line appears twice, through the firmware console and GRUB's serial terminal).
9. **Framebuffer and interrupts:**
   - **Interrupts:** no interrupt problem. Timer IRQs from ring 3 and in the idle halt, 8259
     masks and PIT all worked, and no stray vector from firmware-left APIC state arrived in
     about 2 minutes of tests.
   - **ELCR:** reads `0x0` under OVMF, so virtio-blk interrupt mode is refused and the driver
     polls.
   - **GRUB video:** "error: no suitable video mode found" (non-fatal: the kernel requests no
     framebuffer, and EFI has no text mode). GRUB also warns "no console will be available
     to OS".
   - **Screen:** **VOS's own output never appears on screen under UEFI.** Its only screen
     output is the VGA text buffer at `0xB8000`, which the screenshot shows absent: the last
     visible lines are GRUB's.

## 4. Blockers

| # | Blocker | Severity | Evidence |
|---|---|---|---|
| U0-B1 | GRUB EFI places the boot information and modules below 1 MiB. `boot_reserve` refuses "module below 1 MiB", and the kernel panics | **critical**: no UEFI boot with any module | panics in two runs (CD 1 GiB, USB 8 GiB) |
| U0-B2 | No screen output under UEFI (VGA text only; no framebuffer requested) | **high** for hardware, none for QEMU (serial works) | screenshot |
| U0-B3 | ELCR all-edge under OVMF: no level-triggered INTx, so virtio-blk interrupt mode is unavailable | **medium**: polling fallback works; real firmware may differ | `vblk` log under OVMF |
| U0-B4 | CR0/CR4 inherited from the loader (NE, MP, DE, MCE, OSFXSR, OSXMMEXCPT under UEFI) | **low**: no effect observed; nondeterminism between boot paths | `-d cpu`, kernel log |
| U0-B5 | Self-tests assume BIOS hardware (VGA text buffer, a free level line) | **low**: test portability | main test image under OVMF |
| U0-B6 | Boot information or modules above 1 GiB would be refused | **not observed**; risk on other firmware | code (`module extends above 1 GiB`, info limit) |

## 5. Proposed minimal U1 [Proposal]

1. **Accept loader-placed data below 1 MiB (B1).** The low 1 MiB is already reserved and the
   PMM never allocates there, and the W^X map already covers phys 0–4 MiB.
   - Allow modules and the information block anywhere in available memory below 1 GiB.
   - Keep the overlap checks (kernel, information block, modules, each other) and refuse
     anything inside a non-available entry.
   - Synthetic bootinfo tests for low placements.
2. **Kernel-defined CPU state (B4).** Set CR0 and CR4 to explicit values at boot (keep only the
   bits VOS uses). Log them, identically under BIOS and UEFI.
3. **Boot-environment-aware self-tests (B5).** The VGA and level-line tests check what the boot
   provides and say "not applicable" instead of failing. Under BIOS they stay mandatory.
4. **A separate UEFI build and test target:**
   - `make uefi-iso` (only when `x86_64-efi` is present) and `make test-uefi`;
   - test-image ISOs under OVMF, with a per-run variable copy and the halt-address check;
   - runs: boot, main test image, vblk (polling expected), ext2.
   - The BIOS targets and their ISOs stay unchanged. A combined BIOS+UEFI ISO only after both
     GRUB platforms are installed system-wide.
5. **Screen output under UEFI (B2), in U1 if approved, otherwise U2:**
   - a Multiboot2 framebuffer request (optional, so BIOS text mode stays);
   - a framebuffer text console with a built-in font;
   - a kernel facility to map device memory uncached or write-combining (OVMF's GOP buffer
     lies above 1 GiB). This is the first change to the VMM's "only PMM frames" rule and needs
     its own review.

**Not in U1:** ACPI, local APIC and IOAPIC, MSI-X (needed for interrupt-driven virtio under
UEFI, B3), VOS's own EFI loader, Secure Boot, hardware tests.

## 6. Legacy BIOS regression and compatibility

U0 changed no file under `arch/`, `user/`, `docs/abi/` or `tests/`, nor the Makefile. The
system GRUB stays i386-pc only (`/usr/lib/grub` unchanged).

**Hashes before the experiment and after the regression** (identical):

| Artifact | SHA-256 |
|---|---|
| production `build/x86_64/vos.bin` | `a480c611d09b502bd67874b3029fab98d75f2d20be6a0367d0e02d3f96a911b8` |
| test `build/x86_64-test/vos.bin` | `62f99cb3c97d29ad4a48ae6277e22a6284471a6c8e892a886be157ab8e319976` |
| `hello.elf` | `c6c6416195a0f18d680d7f8f19be52ce02ff02912ef3bac45324fbe88006cd06` |
| i386 `vos.bin` | `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd` |

**Legacy run:** `make test-all` exit 0: 66 runs passed, 0 failed, 11 fault comparisons,
`test-user-elf` 292/0 (BIOS x86_64 suite plus i386).

**Reproduction from the repository:**
- `scripts/uefi-u0-image.sh`, with `GRUB_EFI_DIR` pointing at the unpacked `x86_64-efi`
  directory and mtools on `PATH`, rebuilt the four images. The kernels inside are
  byte-identical to the BIOS builds.
- `scripts/uefi-u0-ovmf.sh` judged:
  - the production image (CD) "halted elsewhere (panic?) at `ffffffff80109491`",
    `panic: module below 1 MiB`;
  - the no-module image (USB) "kernel returned and halted", `boot: checks passed`.

## 7. Files

| File | Purpose |
|---|---|
| `scripts/uefi-u0-image.sh` | Builds the experimental UEFI ISOs into `build/x86_64-uefi-u0/` (ignored by Git) |
| `scripts/uefi-u0-ovmf.sh` | Boots one image under OVMF with a fresh variable store and judges the end state |
| this document | U0 result, blockers and U1 proposal |

Neither script is called by `make`. No USB media, disk, partition, EFI variable or firmware
setting was touched; the Dell was not used.

## 8. U1 result: UEFI readiness with BIOS compatibility (complete)

One x86_64 kernel now boots and passes its tests through both GRUB
x86_64-efi under UEFI firmware and GRUB i386-pc under BIOS firmware. It uses
the same Multiboot2 handoff on both paths. Secure Boot is not supported.

**Commits (on top of `ac4bb52`):**

| Commit | Stage |
|---|---|
| `41e2c41` | 1: loader objects below 1 MiB under explicit rules |
| `d3f17ca` | 2: defined CR0/CR4/EFER state on every boot path |
| `14f9a72` | 3: kernel-only device-memory window |
| `1bac85a` | 4: framebuffer information validated |
| `ec8c749` | 5: screen console, original 8×16 font |
| `b0f7d99` | 6: boot-aware self-tests |
| `c7225a5` | 7: `make uefi-iso`, `make test-uefi` |

### 8.1 Supported boot paths

| Firmware | Loader | Media | Screen |
|---|---|---|---|
| UEFI (OVMF tested) | GRUB x86_64-efi (`make uefi-iso`) | CD or USB stick: `\EFI\BOOT\BOOTX64.EFI` on an EFI System Partition (GPT) | GOP framebuffer |
| BIOS (SeaBIOS tested) | GRUB i386-pc (`make iso`) | CD or USB stick (hybrid MBR) | VBE linear framebuffer |

**Common to both paths:**
- Multiboot2 with the module-alignment tag and an optional 32-bpp framebuffer request.
- Serial log on COM1, 38400 8N1.
- Without a usable framebuffer: serial only. VGA text is used only when the loader reports
  80×25 EGA text, or on a BIOS boot without framebuffer information whose BIOS data area
  reports 80-column text mode.

### 8.2 Multiboot2 loader data and low memory

**What the Multiboot2 specification guarantees:**
- The boot information is at a 32-bit physical address and 8-byte aligned.
- Modules are page-aligned (we request this).
- The kernel's segments are loaded at their addresses.
- Framebuffer information arrives only if requested, and may be absent.

**What it does not guarantee:** any placement relative to 1 MiB or 1 GiB.

**Observed placement:**
- GRUB x86_64-efi under OVMF: the boot information and modules below 1 MiB, in memory the
  map calls available (UEFI LoaderData). With a tiny kernel, as low as `0x4000`/`0x8000`.
- GRUB i386-pc under SeaBIOS: directly above the kernel.

**Rules for every loader object** (`bootinfo.c`):
1. Non-empty and at or below 1 GiB.
2. Never in physical page 0 or the legacy hole `[0xA0000, 1 MiB)`, even if a map calls them
   RAM.
3. No page shared with the kernel image.
4. Whole pages inside one available map entry, touching no other entry type: the EBDA,
   ACPI, reserved and firmware entries are refused by page rounding.
5. No byte overlap with another loader object. Sharing a page is allowed, because
   reservations are a union of pages.
6. The kernel image itself starts at or above 1 MiB.

All objects are reserved before the PMM exists; the low 1 MiB stays wholly reserved; the
W^X map already covers phys 0–4 MiB. The 1 GiB support limit is unchanged.

### 8.3 CPU state

`cpu_init_protections` records what the loader left and then establishes one documented
state (each bit is documented in `cpu.c`):

| Register | Policy |
|---|---|
| CR0 | PE, PG, WP, EM, NE set; MP, TS, AM, CD, NW clear |
| CR4 | PAE set; DE and MCE when present; SMEP, SMAP, UMIP when present. VME, PVI, TSD, PSE, PGE, PCE, OSFXSR, OSXMMEXCPT, FSGSBASE, PCIDE, OSXSAVE, PKE, PKS, CET clear. LA57, VMXE, SMXE and reserved bits preserved |
| EFER | NXE set; SCE cleared (SYSCALL is not configured) |

**Compatibility changes for BIOS boots:**
- CR0.NE is now set.
- CR4.DE and CR4.MCE are now set: a machine check panics with a message instead of
  shutting the machine down.
- EFER.SCE was already clear.

**Measured** with the same CPU model: SeaBIOS (inherited cr0 `0x80000011`, cr4 `0x20`) and
OVMF (`0x80010033`, `0x668`) both end at cr0 `0x80010035`, cr4 `0x100068`, efer `0xd00`.

**Ring-3 tests:** SYSCALL → #UD, RDPMC → #GP(0), WRFSBASE → #UD, XGETBV → #UD.

### 8.4 Device-memory window (`devmap.h`)

- **Location:** kernel PDPT entry 509 (VA `0xFFFFFFFF40000000`). It is shared by every address
  space, and every entry in it is supervisor-only.
- **Pages:** 4 KiB, present, writable, NX, never user or global; an unmapped guard page after
  each mapping; at most 8 mappings and 64 MiB in all; permanent.
- **What may be mapped:** ranges outside every RAM-type map entry (available, ACPI
  reclaimable, ACPI NVS, bad, unknown types). Firmware "reserved" entries and unlisted MMIO
  holes may be mapped.
- **What is refused:** zero length, overflow, addresses beyond CPUID's physical width, and
  physical overlap with another device mapping (no attribute aliases).
- **Memory types:**
  - **UC:** PCD|PWT, PAT entry 3.
  - **WC:** the PAT bit; PAT entry 4 is set to WC only when the CPU has a PAT, after
    checking that entry 3 is UC. Entries 0–3 are verified unchanged, and the PAT bit is
    refused everywhere else by the VMM audit.
  - **MTRRs:** read, never written. PAT WC combined with any MTRR type is WC or UC, never
    cacheable.
- **Audits:** `devmap_audit` and the VMM audit match every device leaf to the registry.

### 8.5 Framebuffer and screen console

**Accepted framebuffer information** (`boot_framebuffer`):
- RGB, 32 bpp, BGRX (red 16, green 8, blue 0, 8 bits each): the verified format on OVMF GOP
  and SeaBIOS VBE;
- 320×200 to 8192×8192, pitch a multiple of 4, at least width × 4 and at most 64 KiB;
- size computed in 64 bits, at most 32 MiB, address aligned and not wrapping;
- or 80×25 EGA text at `0xB8000`.

Anything else is refused with a reason, and the boot continues.

**Mapping:** a legitimate framebuffer is device memory. On OVMF it is the VGA BAR at
`0x80000000`, on SeaBIOS `0xfd000000`, absent from the memory map; firmware may also list it
as "reserved". It is mapped write-combining through the window. A framebuffer inside RAM is
refused (`devmap_check`).

**The screen console** (`screen.c`):
- It mirrors the serial log with an original 8×16 font (`scripts/font8x16.py` generates
  `font8x16.c`; the test target checks they match).
- A status bar shows BOOTING, PASS (green) or FAIL (red), plus a red panic banner.
- Lines wrap; when the area is full, output continues at the top with a blank line after the
  newest one.
- It never reads the framebuffer.
- Nothing is drawn before `screen_init`; earlier output is replayed.
- **Before U1 the kernel wrote "Hello from VOS!" to `0xB8000` unconditionally.** It no longer
  writes VGA memory unless text mode is positively known.
- **BIOS change:** because the kernel requests a framebuffer, BIOS GRUB now switches to VBE
  graphics. Neither `gfxpayload=text` nor an EGA-text console tag keeps text mode (verified
  with probe kernels). BIOS boots therefore also use the pixel console.

### 8.6 Self-tests by boot environment

| Former U0 failure | Classification | Now |
|---|---|---|
| VGA text writable | invalid where no text mode exists (UEFI, and VBE graphics on BIOS) | `vmm` checks only the alias mapping (writable, NX, supervisor); the write test runs in `selftest_screen` when VGA text is the active console, else NOT APPLICABLE |
| free level-triggered line | firmware-dependent (OVMF: ELCR 0) | edge-line and shared-line tests mandatory everywhere (a second edge line stands in); level-specific checks NOT APPLICABLE when no level line exists |
| `elf`: one module named `hello` | an artifact of the U0 diagnostic | passes unchanged (Stage 1) |
| virtio-blk interrupt mode under OVMF | genuine portability limit: INTx needs a level line | the driver must choose polling for exactly that reason; the full polling battery runs; interrupt-mode tests NOT APPLICABLE |

### 8.7 Build and test

**Packages** (once, run by you; three packages, no maintainer scripts, no boot changes):

    sudo apt-get install --no-install-recommends grub-efi-amd64-bin mtools

**Targets:**
- `make uefi-iso` builds `build/x86_64/vos-uefi.iso` from the same tree as `make iso`.
- `make test-uefi` runs the UEFI matrix: q35 + `OVMF_CODE_4M.fd` (Secure Boot not enabled)
  and a fresh `OVMF_VARS_4M.fd` copy per run.
- The BIOS targets (`make iso`, `make test`, `make test-all`) are unchanged in function.

**Without a system install:** point `GRUB_EFI_DIR` at an unpacked `x86_64-efi` directory and
put mtools on `PATH`. Nothing writes outside `build/`.

**Reproducibility:**
- Kernel and ELF binaries are byte-reproducible.
- ISO images are not: xorriso and GRUB embed timestamps and UUIDs. Compare their payloads
  instead.

### 8.8 Evidence and final verification

**Measured (QEMU 10.2.1, OVMF 2025.11, SeaBIOS; Haswell-noTSX, `max`, default CPUs):**

| Run | Result |
|---|---|
| BIOS `make ARCH=x86_64 test` after stage 1 | 65 passed, 0 failed, `test-user-elf` 292/0 |
| BIOS `make ARCH=x86_64 test` after stage 2 | 65 passed, 0 failed, `test-user-elf` 292/0 |
| `make test-uefi` (first full run) | 15 of 16 runs passed (first published as "17 of 18", a miscount) |
| — the one failure: `uefi-ext2` | a check that needs interrupt mode, while the driver polls under OVMF; reclassified in `b0f7d99`; rerun: PASS (94 cases, 0 failed, 1 not applicable) |
| UEFI production boot | `user[hello]: Hello from VOS!`, `boot: checks passed`, halted after `kernel_main`, PASS badge in the screenshot; also via USB at 8 GiB, without serial, without framebuffer, at 640×480 and at 1920×1080 |
| UEFI test kernel | 1 GiB and 8 GiB: `VOS-TEST: PASS` |
| UEFI suite, ipc, mp, stress, irq images | `VOS-TEST: PASS` |
| UEFI vblk | `VOS-TEST: PASS`: polling with its reason, 102 cases, 0 failed, 1 not applicable |
| BIOS vblk on the final tree | interrupt mode on IRQ 11, 149 cases, 0 failed, 0 not applicable |
| BIOS main test kernel at `ec8c749` | PASS |
| Every U1 commit | builds on its own |

**Final verification (at the checkpoint, after `3e9d1c2`):**
- **Reproducibility:** two clean builds gave identical production and self-test kernels.
- **`make test-all`:** 68 passed, 0 failed, `test-user-elf` 292/0.
- **`make test-uefi`:** 16 passed, 0 failed, with the EFI boot path verified.
- **Compatibility:** `hello.elf` and i386 `vos.bin` hashes unchanged; `git diff --check`
  clean.
- **Mutations:** 15, all caught; one only after the test added in `fa37ccd`.

**Corrections to the table above:**
- The `make test-uefi` matrix has 16 runs, so the earlier partial result was 15 of 16, not
  17 of 18.
- The x86_64 kernel changes by design in U1; `hello.elf`, the i386 kernel and the ABI did not.

### 8.9 Known limitations and hardware risks

- **Secure Boot:** not supported. GRUB's lockdown refuses unsigned Multiboot2 kernels. U3
  (VOS's own signed EFI loader) is the planned path.
- **Interrupts under UEFI:** INTx is edge-only under OVMF, so virtio-blk polls; MSI-X or the
  IOAPIC are needed.
- **Physical memory:** only RAM below 1 GiB is used. The device window holds at most
  8 mappings and 64 MiB.
- **Framebuffer format:** only BGRX at 32 bpp is accepted. Other GOP and VBE formats get
  serial only.
- **VGA text fallback:** verified by unit tests only. No available loader boots this kernel
  without a framebuffer on BIOS.
- **Write-combining:** functional only in QEMU (TCG ignores memory types). The PAT and MTRR
  behaviour on real hardware is unverified.
- **Not tested on the Dell OptiPlex 9020 or any physical machine.**
