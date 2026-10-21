# VOS Status

**Latest completed checkpoint:** 2026-10-19, U1 complete (historical results below).
The checkpoint tag is `checkpoint-2026-10-19-u1`. Development verification on
2026-10-21 is [READY FOR N3.5 CLOSURE REVIEW](results/n3.5-stage5.md);
N3.5 remains **INCOMPLETE** pending final architectural approval.

VOS is an experimental, independently written x86_64 kernel (with a legacy i386 kernel). It
runs only in QEMU so far: no physical machine has booted it. This is a development checkpoint,
not a release.

## Completed milestones

N1 (user mode, system calls), N2 (scheduling, preemption of user programs, IPC), N3.0–N3.4
(W^X higher-half memory, virtio-blk, block cache, read-only ext2, interrupt-driven block I/O),
U0 (UEFI feasibility) and U1 (UEFI readiness with BIOS compatibility).
See `docs/ROADMAP.md`.

## Boot paths (all verified in QEMU only)

| Firmware | Loader | Media | Screen |
|---|---|---|---|
| UEFI: OVMF 2025.11, q35, Secure Boot not enabled | GRUB 2.14 x86_64-efi (`make uefi-iso`) | CD; USB stick (xHCI) | GOP framebuffer console |
| BIOS: SeaBIOS (QEMU pc and q35) | GRUB 2.14 i386-pc (`make iso`) | CD; USB stick (hybrid MBR) | VBE framebuffer console |

On both paths:
- Multiboot2 handoff, with loader data accepted below 1 MiB under explicit rules.
- Serial log on COM1 (38400 8N1).
- Boots without a serial port, or without a framebuffer (then serial only).

## Kernel capabilities

**Memory and protection:**
- Higher-half kernel with a W^X map, NX, and SMEP/SMAP/UMIP when the CPU has them.
- A defined CR0/CR4/EFER state on every boot path: SYSCALL disabled, no RDPMC, WRFSBASE or
  XSAVE in ring 3.
- The PMM tracks RAM below 1 GiB.
- An audited, supervisor-only device-memory window.

**Execution:**
- Kernel threads.
- Ring-3 programs in separate address spaces, preempted by the timer.
- IPC channels.
- System-call ABI v0/v1, plus local N3.5 ABI v2 file operations;
  [Stage 3 implementation](results/n3.5-stage3.md) and [Stage 4 security verification](results/n3.5-stage4.md).
  Stage 4 closure is approved. [Stage 5 final verification](results/n3.5-stage5.md)
  passes 74 BIOS and 38 fresh UEFI runs, 845/0 file assertions, 302/0 static checks
  and five mutation controls. Frozen hashes and resource budgets hold.
  N3.5 awaits architectural closure approval; stop before N3.6.

**Interrupts and timing:**
- 8259 PIC and PIT.
- Device IRQ lines with top and bottom halves, and a storm guard.
- Deadlines for blocking waits.

**Storage (read-only):**
- virtio-blk, interrupt-driven where a level-triggered INTx line exists, otherwise polling.
- Block cache.
- ext2 reader and capability-scoped read-only ABI v2 file calls on the development
  branch. Only explicit trusted launches receive non-root directory grants; normal
  hello still requires no disk. No writable operations or authenticated-media claim.

**Display:**
- Framebuffer console with an original 8×16 font, status bar (PASS/FAIL) and panic banner.
- VGA text only when text mode is positively known.

## U1 verification (at this checkpoint)

| Check | Result |
|---|---|
| Reproducibility | two clean builds: production `vos.bin` identical; self-test `vos.bin` identical. ISO images differ by design (embedded timestamps and UUIDs) |
| `make test-all` (BIOS: i386 + x86_64; a superset of `make test` and `make ARCH=x86_64 test`) | exit 0: 68 runs passed, 0 failed, 11 fault comparisons, `test-user-elf` 292 passed / 0 failed |
| `make test-uefi` | exit 0: EFI boot path verified (UEFI El Torito entry, ESP type GUID, `BOOTX64.EFI`); 16 runs passed, 0 failed |
| Compatibility hashes | i386 `vos.bin` `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd` and `hello.elf` `c6c6416195a0f18d680d7f8f19be52ce02ff02912ef3bac45324fbe88006cd06`, both unchanged. ABI v0/v1 unchanged: no change under `user/`, `docs/abi/` or `arch/i386/` |
| `git diff --check` | clean |
| Mutations of the U1 security code | 15 mutations, each a valid build in the main test image; 15 caught by a concrete failing assertion or boot panic (one, F1, only after the test added in `fa37ccd`). Details below |

**Mutations.** The 15 mutations, by area:

| Area | Mutations |
|---|---|
| Loader objects | page 0 accepted; legacy hole accepted; kernel pages shared; module/info byte overlap accepted |
| Device window | overflow check removed; RAM accepted; physical width ignored; user-accessible pages; cacheable "UC" pages; executable pages |
| CPU state | EFER.SCE set; CR4.PCE set |
| Framebuffer | channel layout not checked (F1); size limit removed |
| Screen selection | UEFI without a framebuffer falls back to VGA text |

**Not mutation-tested** (no QEMU configuration produces the state):
- MTRR/PAT behaviour on real hardware;
- a loader that hands this kernel no framebuffer on BIOS (the VGA text fallback);
- EFER.SCE inherited set. QEMU firmware never sets it, so the "not cleared" mutation would be
  equivalent; the "set" mutation is caught.

After the F1 fix, the self-test kernel passed again under SeaBIOS and OVMF (bootinfo 183
cases). The production kernel is unaffected by that test-only change.

**The UEFI runs:**
- **Production:** CD at 1 GiB; USB stick at 8 GiB; no serial port; no framebuffer; 640×480;
  1920×1080. Each is judged by the CPU halted right after `kernel_main` returned, plus a PASS
  badge in the screenshot.
- **Test kernel:** at 1 GiB and 8 GiB.
- **Panic banner.**
- **Test images:** suite (memory isolation), ipc, mp, stress, irq, vblk (polling) and ext2.

**NOT APPLICABLE checks** (reported with reasons, never counted as passed):

| Configuration | Check | Reason |
|---|---|---|
| BIOS and UEFI | VGA text write | a framebuffer console is active |
| UEFI | level-triggered IRQ registration | OVMF's ELCR is 0 |
| UEFI | virtio-blk interrupt-mode tests | the driver polls |
| UEFI | ext2 readers interrupted during I/O | polling waits cannot be interrupted |

On BIOS (SeaBIOS: ELCR `0xc00`), virtio-blk interrupt mode is fully tested: 149 cases, 0 failed.

## Known defects and limitations

- **Hardware:** QEMU only. Untested on any physical machine, including the Dell OptiPlex 9020.
- **Secure Boot:** not supported. GRUB's lockdown refuses unsigned Multiboot2 kernels.
- **UEFI interrupts:** no IOAPIC, MSI or MSI-X. INTx under OVMF is edge only, so virtio-blk
  polls and its waits cannot be interrupted.
- **Memory:**
  - RAM above 1 GiB is ignored.
  - Loader objects above 1 GiB are refused (not observed).
  - The device window holds at most 8 mappings and 64 MiB.
- **Framebuffer:**
  - Only 32-bpp BGRX is accepted; other formats give serial only.
  - The console's write-combining is unverified on hardware (QEMU ignores memory types).
- **VGA text fallback:** only unit-tested. GRUB always hands this kernel a framebuffer, because
  the kernel requests one.
- **BIOS display change:** BIOS boots now switch to VBE graphics.
- **Missing features:** no keyboard input, shell, writable storage, networking,
  SMP or ACPI power management. The local read-only file API awaits N3.5 closure.
  The production kernel ends by halting.

## Unsupported (by design, for now)

- Physical-disk installation.
- `grub-install` / `efibootmgr` use.
- Writes of any kind to storage.
- Secure Boot.
- Hardware other than QEMU's emulated PC.
