# USB input Increment 2: read-only survey v2 and hosted model for 21 ports

Status: **implemented and verified in QEMU and hosted tests; uncommitted; not booted on the Dell.** Base `496cc29`, branch
`feat/usb-inc2-survey-v2`, worktree `/home/veasnawt/src/vos-usb-inc2`. Evidence: `docs/results/usb-inc2-survey-v2/`
(manifest with SHA-256 of every file). Nothing in this increment writes to PCI configuration space, writes a controller
register, resets or initializes a controller, activates DMA, or touches a disk. No physical-test ISO was built, and Increment 3
was not started.

## What changed

| Area | Change |
| --- | --- |
| Hosted model | `MAX_PORTS` 16 to 31 (machine, session, both Python oracles), so the Dell's 21 root ports qualify and PORTSC is read for all of them. 32 and above are still refused before any port read. |
| Survey (Rust, `survey.rs`) | Raw PORTSC of up to 32 ports, ring pointers (DCBAAP, CRCR, interrupter 0), **Intel port routing registers** (PCI 0xD0 XUSB2PR, 0xD4 XUSB2PRM, 0xD8 USB3_PSSEN, 0xDC USB3PRM, read only, only for an Intel xHCI with a type-0 header), **EHCI inspection** (CAPLENGTH, HCSPARAMS, ports, PORTSC, owner bits, CONFIGFLAG, legacy support). A connected EHCI root port is printed with the caveat "a hub behind it? devices below it are not visible". |
| Firmware facts (Rust, new `firmware.rs`) | Multiboot2 memory map and RSDP (identical under BIOS and UEFI GRUB), bounded ACPI walk, DMAR and IVRS parsing, remapping-unit registers. **Four separate facts**: table present, hardware answers, translation enabled, xHCI covered. Unknown is never turned into no. The report always says "DMA isolation NOT ESTABLISHED". |
| Platform (C) | `devmap`: read-only, uncached, non-executable firmware-table lease for tables at or above 1 GiB (page aligned, at most 16 KiB, only memory the map calls reserved or ACPI). Tables below 1 GiB are read through the direct map after the same memory-map check. Remapping units are read only inside 0xFE000000-0xFEFFFFFF. |
| Tests/gates | `tests/rust/xhci_firmware.rs` (new), `xhci_survey.rs` extended, `scripts/test-usb-survey.py` (159 mutants, two suites), `scripts/test-usb-survey-native.py` (18 cases), `scripts/test-usb-inc2-regression.py` (new). |

Changed files: 19 modified, 3 new, +1,337 / -117 lines tracked, plus `firmware.rs` (742 lines), `xhci_firmware.rs` (716), the regression script (161).
Kernel image: `.text` 0x42000 (was 0x3c000), `.rodata` 0xe000 (was 0xd000); no `core::panicking` or `core::fmt` symbols; zero compiler warnings.

## Verification

| Check | Result |
| --- | --- |
| Hosted survey tests | 36 pass |
| Hosted firmware tests | 21 pass (hostile blocks, truncation sweep, 3000 corrupted worlds, bound-asserting fake memory) |
| Survey mutation gate | **158 of 159 killed in the first run; the gate failed.** The survivor `fw-entry-truncation-silent` showed that nothing asserted the "root list was cut" flag. A test was added; `survey-gate-2` killed it (1 of 1). The first run's output is preserved unchanged. The other 158 were not re-run, because adding a test cannot un-kill a mutant. |
| Compile-fail controls | 4 pass (no write method on `Pci`, `Mmio`, `Phys`; `scan` needs a `Pci`) |
| C4.1 model gate (port bound) | Full tests and oracle replay pass, **focused mutation run: 4 of 4** port-related mutants (not the full 104) |
| C4.2a backend gate (port bound) | Full tests and oracle replay pass, **focused mutation run: 5 of 5** (not the full 164) |
| Native QEMU matrix | **18 of 18** (the brief said 14: the script has 9 cases per firmware). SeaBIOS and OVMF: one xHCI, two xHCI behind a bridge plus EHCI, no USB, EHCI with keyboard, Intel IOMMU, AMD IOMMU, ACPI tables above 1 GiB (`-m 2048`, the lease path), default boot unchanged with xHCI and with a 2 GiB IOMMU machine. The first run was 14 of 18; see below. |
| Regression boots (BIOS) | 4 of 4: normal, C1 qualification, C2 DMA ownership exercise, survey+probe conflict refused. |
| Secret audit | 0 findings, 110 files without the manifest and 111 with it (Gitleaks generic-api-key rule reproduction plus credential shapes; `leak-audit.json`, `scripts/leak-scan.py`). An intermediate scan flagged three SHA-256 lines of the manifest only because two file names contained "key" or "secret"; those evidence files were renamed (`ehci-kbd`, `leak-audit.json`), content unchanged. |

### Failures preserved

1. **Survey gate 1**: 158/159, described above (`gates/survey-gate-1-*`).
2. **Native run 1**: 14/18. All four failures (`*-two-xhci-bridge-ehci`, `*-ehci-keyboard`) were defects in my test: the kernel prints two `usb: ehci` lines per controller and the check counted one. Run 2 fixed that and 3 of 4 passed; `uefi-ehci-keyboard` failed because OVMF leaves the port unreset so the survey correctly prints the guess `fs?` and my pattern rejected `?`. Run 3 passed. The kernel and ISOs were not changed between the runs.
3. **A real bug found by the new tests**: the Intel routing line exceeded the 118-character console line and silently lost `usb3prm`; the DMA-facts and signature lines had the same flaw. All are now split over lines and the tests check line length and that the last fact survives.
4. **UEFI ISO step**: first attempt refused because the default GRUB EFI directory does not exist on this host (`i2-uefi-isos`, rc=2, 1.5 KB); rerun with the recorded hash-verified EFI tree.
5. **Storage events**: the C4.2a re-run grew 34.8 MB against an 18 MB projection (1.93x) and tripped the 1.25x STOP (my projection omitted two fresh 10.4 MB worker directories); and the BIOS ISO step was refused because C: free space had fallen below the reserve. Both STOP files and the investigation are preserved in `storage/`. Antigravity released both.

## Storage

Guarded growth per step (tree bytes; admitted projection in brackets): model compile 16.5 MB, kernel verify 15.1 MB, hosted tests ~2.2 MB total, survey gate 13.4 + 7.2 MB, C4.1 focused 40.2 MB [38], C4.2a focused 34.8 MB [18, STOP], BIOS ISOs 16.1 MB [28], UEFI ISOs 25.2 MB [35], native 5.2 + 1.2 + 0.6 MB [8 in total], regression 6.8 MB [16]. The survey gate's second run (7.2 MB vs 6 MB) was within 1.25x. Worktree baseline before the evidence step: 208,081,697 B (stop line 280,000,000; cap 310,000,000). Evidence step: +0.6 MB [5 MB admitted]. Final worktree before the publication commit: 208,696,489 B; C: free 23,610,982,400 B (reserve 20 GiB + 1 GiB margin satisfied). Every step ran under `guard.sh` (`storage/storage-log.txt`, `storage/guard.sh`); no step other than the C4.2a re-run exceeded 1.25x its projection. Nothing outside this worktree was changed.

## Screenshots

`screenshots/` holds QEMU `screendump` images of the survey text on the emulated display (these are genuine running-VOS captures, but of QEMU, not of the Dell). The `native-1-*` images come from the first matrix run; `native-1-*-ehci-kbd` is from a case whose check failed because of my test regex, not because of the guest (the image is taken before the check). Prefer the `native-2-*` images for those topologies. Nothing in them is a mock-up.

## Dell hardware: observed facts versus unverified explanations

**Observed (owner's photographs of the physical survey, and Windows Device Manager):** xHCI 8086:8C31 at 00:14.0 with 21 ports, **0 connected**, 16 scratchpads, 32 slots, 32-byte contexts, running, BIOS-owned (SMI enabled, 0x40002001), OS not claimed; EHCI 8086:8C2D at 00:1A.0 and 8086:8C26 at 00:1D.0; IOMMU unknown (v1 did not read ACPI); the keyboard and mouse are plugged directly into the computer and Windows shows both under EHCI 8C2D, then USB Root Hub, then **Generic USB Hub**, then USB Input Device; the xHCI shows only the HP x750w mass-storage stick.

**Not verified:**
- That the "Generic USB Hub" is Intel's integrated rate-matching hub (8087:0024 or 8087:8000). It is the expected explanation for a hub the user cannot see, but Windows' Hardware Ids and Location Information of that hub and of the devices have not been obtained.
- Which XUSB2PR bits are set, which ports are routable, and which EHCI ports are connected. Survey v2 prints these, but **it has not been run on the Dell**. QEMU has no Intel PCH, so every routing path was tested only against hosted fakes.
- That routing the USB 2.0 ports to the xHCI would make the devices direct root-port devices and remove the hub from the path. That is the Linux behaviour on this chipset family, from secondary sources, and is not established here.
- Any DMAR/IVRS fact for this machine, including whether VT-d is enabled in the BIOS.

Hub support must **not** be assumed unnecessary because the devices are plugged in directly: the hub in the path is internal.

## Remaining blockers to a physical USB keyboard and mouse

1. Survey v2 has not been run on the Dell (needs a physical-test ISO and your approval): routing registers, EHCI port states, ring pointers and DMAR are unknown.
2. The devices sit behind EHCI #2's internal hub. Native xHCI input needs either the ports routed to the xHCI (a PCI configuration write: not done, not authorized) or an EHCI driver plus a hub driver. Nothing of these exists.
3. BIOS ownership handoff, halt and reset of a running, SMI-enabled controller have never been executed on real hardware.
4. The hosted model accepts only the QEMU profile; there is no Dell/Lynx Point profile, and attached devices at initialization are still refused.
5. DMA safety: with IOMMU state unknown, DMA ownership for the xHCI on this machine cannot be justified. DMA activation, rings, event handling, slot and endpoint management, HID boot keyboard/mouse drivers and the input-seam integration are not implemented.
6. Interrupt delivery (legacy IRQ 11 on the Dell, MSI off) is untested.

## Risks and limits of this evidence

- The committed evidence files contain maintainer-local absolute paths (worktree, toolchain), as the earlier `docs/results` evidence does. They carry no credentials; contributors do not need those paths.

- QEMU only. A physical controller, an Intel PCH and a BIOS-owned running controller can all behave differently.
- The C4.1 and C4.2a mutation runs were focused on the port-related mutants because only the port bound changed; the full oracle replays and test suites did run.
- **NOT RUN, so NOT TESTED for this change:** (a) the apps-based C2 and C3a native regression scripts (`test-usb2c2-native.py`, `test-usb2c3a-native.py`), which boot full application ISOs with the independent QEMU traces (about 21 MB of ISOs, over the admitted regression budget of 16 MB); (b) a UEFI regression ISO of the earlier USB modes. The four-case BIOS regression boot is a much narrower check (one application, BIOS only, console markers, no QEMU trace analysis) and is **not equivalent** to either. The UEFI native matrix boots the default image and the survey images only; it does not exercise the C1/C2 modes under UEFI. Because the change touches shared `devmap` code, run those scripts before relying on the C1/C2 paths under UEFI.
- On a 1080p console about 67 rows fit; the Dell output is expected to be longer than the 28 lines QEMU printed, so the top may scroll away. The three SUMMARY lines per area are printed last for that reason.
- The devmap change touches shared mapping code and the unsafe Rust glue has had no independent review yet.

## Recommended Increment 3 scope (not started)

1. **Physical read-only survey v2 on the Dell** (needs a physical-test ISO built from the merged tree and your approval): photograph the routing registers, EHCI port states, ring pointers and DMAR facts; collect the Windows Hardware Ids and Location Information of the hub, keyboard and mouse. No writes.
2. **Smallest next implementation step, hosted only:** a Dell/Lynx Point qualification profile plus a pure *routing plan* model (which XUSB2PR bits would be set, preconditions such as controller halted and BIOS handoff done, postconditions to re-read), with no execute path and no write primitive. Its parameters come from step 1, so build it only after those results.
3. Only after both: a separately authorized first physical write step (BIOS handoff, halt, reset containment) on the Dell.
