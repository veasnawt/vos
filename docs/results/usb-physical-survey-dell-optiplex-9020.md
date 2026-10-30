# First physical USB survey image, Dell OptiPlex 9020 (diagnostic only)

Target: Dell OptiPlex 9020, BIOS A12, Legacy BIOS boot, Intel USB 3.0 xHCI `8086:8C31` rev 04, Windows on the internal drive.
Baseline: Increment 1 merged as PR #36, merge commit `6d6a941a04fe31b318d184408e3c1a55e7d63f8d`.
**Diagnostic only: no controller initialization, no USB device takeover, no DMA activation, no HID.** No physical test has been run.
Nothing is committed or published; the physical boot needs the owner's approval. Owner procedure:
[usb-physical-survey-dell-optiplex-9020-procedure.md](usb-physical-survey-dell-optiplex-9020-procedure.md).

## 1. Source baseline (verified)
`origin/main` = `6d6a941` (fetched), a merge of PR #36 by the owner, parents `c909a7f` and `37b3aeb`. Its tree
`a30ad4d2b8730cdb2a09705ff058dd2e7f75288f` equals the tree of the reviewed commit `37b3aeb` exactly. The image was built in a fresh worktree
of `6d6a941`; the kernel's flat loaded image and stripped ELF are byte-identical to the Increment 1 kernel that passed its gates
(the unstripped file differs only through build paths).

## 2. Read-only safety review
Method: the call graph of the built kernel from `xhci_survey_boot` (47 functions) was walked with `objdump`, listing every port and
privileged instruction, then the C adapters and Rust were read.

| Question | Finding |
| --- | --- |
| PCI configuration writes | none. The only `out` in the graph is `pci_read32`'s write of the **address** register 0xCF8 (selecting what to read; inherent to configuration mechanism 1), followed by a read of 0xCFC. The data port is never written. `pci_write*` is not reachable and the source scans forbid it |
| MMIO writes | none. The only access to the leased window is `xhci_survey_bar_read32` (one aligned volatile load); there is no store path, and the hosted trait has no write method (compile-fail control) |
| Controller reset, DMA, run/stop | not present: no register of USBCMD/CRCR/DCBAAP/ERST/IMAN is written; BME is never touched; no DMA page is allocated for a device |
| Disk writes, firmware settings | none. The survey runs before the scheduler and every storage driver and halts afterwards; no BIOS/NVRAM/CMOS write exists in the graph |
| Other writes in the graph | COM1 UART output (`serial_putc`), the framebuffer text (`screen_*`), the kernel's own page tables and one PMM frame for the 64 KiB lease (`devmap_scope_*`, released afterwards), stack/static data. The `in` at port 0x64 reads the i8042 status register only |
| Works without USB keyboard or mouse after boot | yes. The survey needs no input; the keyboard is only used by GRUB to pick the menu entry (through the BIOS's USB emulation). The kernel never reads input in this path |
| Screen stays visible without interaction | yes: `vos.usb-survey=hold` halts the CPU in a `hlt` loop after printing; QEMU shows the serial log stops and the screenshot is unchanged. The log area is cleared first and the last lines are an 80-column summary |
| 64 KiB window, invalid ranges | Rust refuses any offset past 0xfffc before asking the platform; the C read re-checks token, kind, alignment and extent; `devmap_check` refuses RAM, existing mappings and beyond-MAXPHYADDR; a BAR that is I/O, unassigned, below 1 MiB, unaligned or with decoding off is not read and the reason is printed. Hostile register files (20,000) and PCI trees (300) never panic and stay within the read budget |
| Firmware independence | the survey code has no BIOS- or UEFI-specific logic (PCI mechanism 1, MMIO, text output); only the GRUB configuration differs per firmware |
| Default boot | unchanged: entry 0 of the image is `multiboot2 /boot/vos.bin` + `hello.elf` exactly as `grub.cfg`; `make iso` output is untouched |

No safety or correctness blocker was found. Accepted limits (unchanged from Increment 1): QEMU-only evidence; the BAR size is unknown so a
smaller-than-64-KiB BAR could expose a malformed controller's declared offsets to a neighbour; a read that never completes would hang the
machine (power off); ACPI DMAR/IVRS are not read, so an Intel IOMMU stays UNKNOWN.

## 3. Storage admission
Fresh admission for this build (owner-relayed Antigravity acceptance, 2026-10-30, separate from every earlier approval): forecast 90 MB,
stop line 120 MB, hard cap 135 MB, projection before and measurement after every step, STOP at 1.25x. Final tree **107.1 MB**: below the stop
line and cap, **17 MB above the forecast**. Cause: grub-mkrescue given two platform directories uses only the last one, so the dual image
had to be assembled from the BIOS and UEFI component images (two intermediate images and a merged tree, 45 MB) and one assembly attempt
failed (below) and is kept (13.7 MB). No step exceeded 1.25x its projection (worst 0.92x). Nothing deleted, moved or pruned.
Per-step log: `storage/storage-log.txt`; rules applied: `storage-estimation-rules.md` of Increment 1.

## 4. The images
Path `/home/veasnawt/src/vos-usb-survey-iso/build/sv-1/`.

| Image | Size | SHA-256 |
| --- | --- | --- |
| `vos-usb-survey-dual.iso` (primary, BIOS + UEFI) | 13,686,784 B | `736e5fdeb8b6b32c28374d6dee5b5be4bedef18d39cbede05517e69e0a887535` |
| `vos-usb-survey-bios-only.iso` (fallback) | 6,752,256 B | `a663ff295ead309c82e2c08dea382bb8e9ee549fd910374b2230ac60e587d982` |
| `vos-usb-survey-dual-attempt1-uefi-usb-boot-broken.iso` (kept evidence, do not use) | 13,684,736 B | `05717f2cc727cba1ceb7e2aaae22c7b261722be4d92bf10a3c4a4e5cb742b43e` |

Contents verified by extraction: `/boot/vos.bin` equals the kernel built from `6d6a941` (sha256 `a783dd3f8c627941...`), `/boot/hello.elf` is unchanged,
`/boot/grub/grub.cfg` equals the reviewed `grub-physical-survey.cfg`. The menu: entry 0 "VOS normal boot (default)" (timeout 30 s), entry 1
`>>> VOS USB SURVEY ... vos.usb-survey=hold`; both titles carry `build 6d6a941`. The accepted kernel does not print a build identifier on its own screen,
so the identification is in the GRUB menu entry titles and this report; printing it from the kernel would be a new source change.

Boot structure: the dual image has two El Torito entries (BIOS `eltorito.img`, UEFI `efi.img`), a GRUB2 hybrid MBR boot sector and a GPT with an
EFI System Partition, so it also boots when written raw to a USB drive. The BIOS-only image has an **active** MBR partition (0x80); the dual
image's MBR carries only the protective entry (0xEE, not active). That is how Ubuntu-style hybrid images look and they boot on most BIOSes, but a
BIOS that insists on an active partition could skip it, which is why the fallback exists. Rufus: **DD Image mode** (both are ISOHybrid; ISO mode
would rebuild the bootloader files and is what failed before).
Built with the steps in `build/assemble-dual.sh` (the reviewed, unchanged Makefile builds the kernel and the BIOS component image; the diagnostic
`grub.cfg` and the merge are build inputs recorded here, not source changes).

## 5. QEMU verification of the exact images (QEMU 10.2.1 q35; SeaBIOS and OVMF)
Each image is booted as an optical disc and as a raw USB mass-storage disk (what a DD-mode drive is), the GRUB menu driven by key events.

| Case | Result |
| --- | --- |
| dual, BIOS, disc, survey entry | pass: survey text, xHCI listed, summary, halts, screenshot |
| dual, BIOS, raw USB disk, survey | pass (the attached USB disk is seen as a connected port) |
| dual, UEFI (OVMF), disc, survey | pass |
| dual, UEFI, raw USB disk, survey | pass (after the fix below) |
| dual, BIOS and UEFI, disc, default entry (no keys) | pass: normal boot completes, no `usb:` line |
| BIOS-only, BIOS, disc and raw USB disk, survey | pass |
In every survey case the log stops after `usb: survey complete. Nothing was written...` and nothing prints for 4 s afterwards; the screenshot shows the
report with the last lines the 80-column summary (`native/qemu-2/*/screen.png`).

**Failed attempt, preserved:** the first dual image booted as a disc and as a BIOS USB disk but **not as a UEFI raw USB disk**: GRUB's EFI image locates its
modules through a `/.disk/<date>.uuid` file named in its embedded configuration, and the merge had taken the tree of the BIOS image, which has no such
file, so GRUB stopped at `grub rescue>` (`native/qemu-1`). Fix: carry the UEFI image's `.disk` directory into the merged tree (`assemble-dual.sh`);
the whole matrix was rerun (8/8, `native/qemu-2`).

## 6. What is NOT tested
Any physical hardware; Dell BIOS A12 behaviour with these images; real USB-stick controllers; GRUB keyboard handling on the Dell. UEFI on the Dell
is not applicable (Legacy mode). The older regression suites (C1-C3b.2, PS/2) were not re-run for this milestone: the kernel is byte-identical to the
one CI and Increment 1 already verified, and no source changed.

## 7. Remaining risks and required approvals
- **Approval needed:** the physical boot. Nothing has been run on the Dell.
- The survey does not print the Intel-specific xHCI port-routing registers (PCI config 0xD0-0xDC). On a Lynx Point machine USB 2.0 ports can be routed to the EHCI
  controllers, so the xHCI report may show **fewer connected ports than the devices plugged in**. The survey lists the EHCI controllers but not their ports.
  If that happens, a later read-only increment should print those config words (a source change, so it needs approval).
- Intel IOMMU state remains unknown (no ACPI table reading); the xHCI scratchpad request will be read for the first time on real hardware and may exceed the 64-buffer budget.
- The model refuses a controller with a device attached at boot (C4.1 rule), so the next increments need a design for the plugged-in keyboard before any activation.
- Firmware USB-to-PS/2 emulation will still be active during this survey (nothing is taken over), which is why GRUB sees your keyboard.
- Dual image MBR (above); BIOS-only fallback prepared.
- Documentation: `docs/plans/MASTER-PLAN.md` and `docs/ROADMAP.md` now state that UEFI x86-64 is the primary long-term boot target and Legacy BIOS a supported
  compatibility path, with firmware-independent USB/DMA/memory/HID logic (uncommitted).
