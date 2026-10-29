# USB.2-C3b.1: controller containment dry-run

**Verified for PR review, not accepted or merged.** C3b.1 is a **read-only
rehearsal** of controller containment. It shows what C3b.2 would do to the one
qualified QEMU xHCI profile, and when it would refuse, without doing any of it.

With opt-in `vos.usb2-contain=1`, the kernel does three things:

1. **Admission.** It admits only a strictly idle controller left by UEFI. Any
   active slot, device, transfer ring, scratchpad buffer, pending or enabled
   interrupt, ambiguous ownership or residue hazard means refusal.
2. **Re-observation.** It observes the controller three times (A, R0, R1).
   Every pass is a complete set of fresh reads, and all three must be identical.
3. **Proposal as data.** It records the two proposed effects as **trace data
   marked `NOT-ISSUED`**:
   - W1: clear PCI Bus Master Enable.
   - W2: set USBCMD.HCRST.

   The reset poll and the post-reset verification that would follow are
   recorded as `NOT-RUN`.

**There is no write path in this build.** No new source names a write primitive.
Neither the kernel's Rust archive nor the xHCI C objects reference a PCI write
symbol. The `Source` trait has no write method, and `Rehearsal` has no issue
method; compile-fail controls prove both. `ISSUED` is a `const false`. In every
native boot, after VOS starts:

- 0 xHCI MMIO writes;
- 0 xHCI configuration writes;
- 0 configuration writes of any kind outside the accepted virtio-blk driver.

**The dry-run establishes nothing about real reset behavior.** No controller was
reset, halted, started, masked or published. Verification is QEMU-only (OVMF and
SeaBIOS); physical hardware is NOT TESTED. **C3b.2 is not authorized.**

- **Baseline:** merged C3a.3, PR #31, `55f6693a4e537248c912aaed61f0194cc07be811`.
- **Worktree:** `/home/veasnawt/src/vos-usb2c3b1`, branch `feat/usb2c3b1-containment-dry-run`.
- **Final candidate:** `kernel-3`. `vos.bin` SHA-256 `2bf50222f843dcf5…`; Rust
  archive `4baf4f8e63b65fa3…`. An independent rebuild (`kernel-3-repro`) is
  byte-identical.
- **Records:**
  - [inventory](usb2c3b1-containment-dry-run/changed-files.txt)
  - [hash manifest](usb2c3b1-containment-dry-run/manifest.json)
  - [measurements](usb2c3b1-containment-dry-run/measurements.json)
  - [budget gate](usb2c3b1-containment-dry-run/history/budget-gate.json)
  - [blackout incident](usb2c3b1-containment-dry-run/history/blackout-incident.json)
  - [storage](usb2c3b1-containment-dry-run/storage.json)
  - [preservation](usb2c3b1-containment-dry-run/preservation.json)
  - [Gate 0](usb2c3b1-containment-dry-run/gate0/gate0.json)
  - [retained failures](usb2c3b1-containment-dry-run/history/failures.json)

## Authorization and decisions

C3b.1 was authorized as a QEMU-only read-only rehearsal, with decisions D1–D6:

- **D1:** omit interrupt masking only when interrupts are verifiably disabled.
- **D2:** propose a bus-master clear before reset.
- **D3:** strict eligibility.
- **D4:** no automatic storage increase.
- **D5:** verify C3b.1 before requesting C3b.2.
- **D6:** AHCI is a separately verified QEMU residual.

The first candidate exceeded the 4,096 B code ceiling, at 9,674 B. Optimization
without removing checks reached 8,899 B. Work then **stopped at the budget gate**
before any test, commit or publication. The owner approved a narrow increase for
C3b.1 only:

- code and rodata ≤ 9,728 B;
- permanent RW ≤ 96 B;
- largest frame ≤ 512 B.

A 12 KiB cumulative C3b planning allowance is provisional and is not authority
for C3b.2. The storage stop line and the C: reserve are unchanged. The oversized
measurements are retained in `history/`.

A power **blackout** interrupted the work after the budget request. It wiped the
session scratch directory, which held throwaway smoke outputs and a copy of the
request. The source was not affected:

- All 14 files of the saved post-blackout patch (`73ec66fe…`) match the candidate
  byte for byte.
- A rebuild from the recovered tree is byte-identical to `kernel-3`.
- All worktrees are clean, and the C3a.3 manifest matches 160/160 files.

See [blackout-incident.json](usb2c3b1-containment-dry-run/history/blackout-incident.json).
All later artifacts are kept under `build/`, which is persistent.

## Architecture

| Layer | Responsibility |
| --- | --- |
| `kernel/rust/usb/xhci/containment.rs` (canonical, `forbid(unsafe_code)`, portable) | `Observed` (one complete observation pass), `refusals()` (19-condition mask), `rehearse()` (the dry-run state machine), the failure model C3b.2 would need (`bus_master_outcome`, `reset_outcome`) and `ISSUED = false`. `Source` exposes only `observe`. |
| `kernel/rust/usb/xhci/lifecycle.rs` | D1: `Facts.interrupts_disabled`. The interrupter mask step is omitted only when interrupts are verifiably disabled. Otherwise the C3a lifecycle still requires the mask (`MissingMask` / `Unneeded`). The C3a guard passes `false`, so its plan is unchanged. |
| `kernel/rust/usb/xhci/residue.rs`, `qualify.rs` | The residue `Outcome` gains `interrupter_pending` and a `digest` over every value read, so re-observation compares memory contents, not only counts. The PCI walk records the PCI Express capability and rejects duplicates. |
| `arch/x86_64/rust/xhci_qualify.rs` | A phase-aware C1 read budget (`PHASE_LIMIT` 4, `READ_LIMIT` 256 per phase), so three full passes stay bounded. |
| `arch/x86_64/rust/xhci_contain.rs` | Private C ABI `vos_xhci_contain_v1`. `Live` implements `Source` with C1's configuration adapter, the C1 BAR lease and the C3a.3 residue reader. None of these can write. It reads PCIe Device Status (Transactions Pending) and every PORTSC on each pass. The report layout is asserted on both sides (376 B). |
| `arch/x86_64/xhci_qualify.{c,h}`, `kernel.c` | `xhci_contain_boot()`: runs once and only with the option. UEFI is detected from the boot information: an EFI system table, one tag-17 map, boot services exited. While the export runs, it holds a **freeze flag** that makes `xhci_residue_reserve()` fail (-4). It prints the C1 report, 12 containment fields and the trace, then checks integrity: lease released, devmap restored and audited, PMM free count and check, residue registry unchanged. It is exclusive with `vos.usb_probe=1` and the other C-series options. |

### State machine

1. **Admission.** Observation A runs inside C1 qualification, between C1's two
   USBCMD/USBSTS observations. `refusals(A) == 0` is required, and the C3a
   lifecycle plan must equal the expected five steps: AwaitHalted, DisableMaster,
   Reset, AwaitResetReady, VerifyCleared.
2. **R0.** A fresh pass must equal A. An unreadable pass or any difference means
   refusal.
3. **W1 is proposed.** Trace: PCI write `NOT-ISSUED`, width 16, offset 0x04,
   old = command, new = command & ~BME.
4. **R1.** A fresh pass must equal A, with the command normalized through
   `expected_command(before, ISSUED)`, which is the unchanged command here. In
   C3b.2 this same pass would confirm that W1 took effect and changed nothing
   else.
5. **W2 is proposed.** Trace: MMIO write `NOT-ISSUED`, width 32, offset
   operational+0 (USBCMD), old = USBCMD, new = USBCMD | HCRST.
6. **Planned but not run.** Reset poll (`NOT-RUN`): USBSTS, at most 1024
   iterations, until HCRST = 0 and CNR = 0. Verify (`NOT-RUN`): USBSTS = 1, and
   USBCMD, CRCR, DCBAAP and the interrupters cleared.
7. **DryRunComplete** (state 6), or **Refused** (7) with the refusal mask. States
   8–14 (`BusMasterOff`, `BusMasterStuck`, `ContainedBmeOnly`, `ResetStuck`,
   `ResetUnverified`, `Contained`, `IntegrityFault`) exist only in the tested
   failure model. No live code path reaches them.

### The 19 refusal conditions

| Bit | Name | Refused when |
| --- | --- | --- |
| 0 | not-uefi | Not a UEFI boot with boot services exited and one EFI map. |
| 1 | unqualified | C1 qualification failed, or the C1 integrity check failed. |
| 2 | profile | Not the exact QEMU `qemu-xhci` identity, class, capability header, extended capabilities and protocol ports. Any legacy-support capability also refuses here, which covers ambiguous BIOS/OS ownership. |
| 3 | pci-command | PCI command ≠ 0x0007 (memory, bus master and I/O decode, with INTx enabled and nothing else). |
| 4 | pci-status | PCI status ≠ 0x0010: an INTx status bit, an error bit, or a missing capability list. |
| 5 | pcie-unknown | No PCI Express capability. |
| 6 | pcie-busy | PCIe Device Status Transactions Pending is set. |
| 7 | interrupts | Any of: an interrupter IMAN.IE set, IMAN.IP pending, USBCMD.INTE or USBSTS.EINT set, MSI or MSI-X enabled, INTx status set. |
| 8 | running | USBCMD.RS set, or USBSTS.HCH clear. |
| 9 | usbcmd | USBCMD ≠ 0. |
| 10 | usbsts | USBSTS ≠ exactly 0x1 (HCH only). |
| 11 | ports | Any PORTSC with CCS/PED/PR/OCA or a change bit set. |
| 12 | slots | A device context or slot is in use (residue finding). |
| 13 | scratchpads | Scratchpad buffers are present (finding, or HCSPARAMS2 count non-zero). |
| 14 | residue | The C3a.3 residue walk is incomplete, or any finding, hazard, reservation or second ownership. |
| 15 | memory | No firmware memory was inspected in place (excluded = 0). |
| 16 | changed | R0 or R1 differs from A. |
| 17 | plan | The lifecycle plan is not the expected five steps. |
| 18 | unreadable | A re-observation pass could not complete. |

## Audits required by the resume authorization

1. **Candidate equals the saved patch.** PASS: 14/14 files match.
2. **Failure records preserved.** The budget gate, the oversized measurements
   (`measure-1` 9,674 B, `measure-3` 8,860 B, `measure-4` 8,899 B, against the
   4,096 B ceiling at the time), the empty `measure-2` from a failed build, and
   the blackout record are all retained.
3. **Strictly read-only.** PASS.
   - *Statically:* a write-primitive scan of both new sources, and `nm -u` on the
     archive, `xhci_qualify.o` and `efi_memory.o`, find no PCI write symbol.
   - *Type system:* the `source-has-no-write` (E0599), `rehearsal-cannot-issue`
     (E0599) and `no-executor` (E0425) controls.
   - *Dynamically:* 0 writes in all 32 native boots. An independent QEMU trace
     analysis checks MMIO, configuration and register provenance.
4. **State machine, mask and revalidation.** PASS.
   - 11 pure tests.
   - A 20,000-round randomized comparison of `refusals()` against an independent
     oracle.
   - Each condition is enforced individually.
   - The three native refusal masks are reproduced exactly.
   - Every field change, and every unreadable pass, is detected at R0 and at R1.
   - 17 mutations are all detected, among them skipping either revalidation,
     resetting before the bus-master clear, `ISSUED = true`, a wider W1, a W1
     that clears more bits, a trace that claims a write was issued, and an
     accepted reset timeout.
5. **Freeze flag.**
   - There is one site that sets it, immediately before the export, and one
     that clears it, immediately after, together with the session flag, the
     boot information and the reservation table.
   - The export cannot return early around the clear; every C return follows it.
   - Its only reader is `xhci_residue_reserve()`. A reservation attempted during
     the session fails, and the PMM is not touched (`c_freeze`).
   - The dry-run allocates nothing.
   - After the session, the PMM free count, the PMM check, the devmap count and
     audit, and the residue registry are compared with their values before it
     (`c_integrity`, plus the native PMM bitmaps).
6. **The proposed trace cannot execute.**
   - Trace entries are plain `u32` data.
   - No function consumes a trace and performs an access.
   - The C side only prints it.
   - The compile-fail controls and the `issued-true` / `trace-claims-issued`
     mutations guard this.

**Further safety requirements.**

- **Strict UEFI-only eligibility:** bits 0 and 2–15.
- **C3a guard unchanged:** the C3a suite plus the native `r`/`q`/`d` boots.
- **Firmware-originated events kept apart from VOS writes:** see the native
  results.
- **Bus-master-before-reset revalidated:** see the specification evidence. The
  `reset-before-bus-master` mutation is detected.

## Verification

| Gate | Result | Evidence |
| --- | --- | --- |
| 1. Hosted Rust tests | **PASS.** Containment suite 11/11. Residue suite updated for `interrupts_disabled` (unchanged behavior). | [c3b1-host](usb2c3b1-containment-dry-run/hosted/c3b1-host/result.json) |
| 2. ASan/UBSan | **PASS.** 24 C3b.1 scenarios (397 checks) in the real PMM/devmap/qualification/residue C, run by the C3a suite: the eligible profile, 20 refusals, freeze, drift in pass 2 and pass 3, and a mid-session PCI change (integrity failure, freeze released). All existing C3a/C3a.3 scenarios are unchanged and pass. | [c3a-host](usb2c3b1-containment-dry-run/hosted/c3a-host/sanitizer.txt) |
| 3. Mutation and compile-fail controls | **PASS.** 17/17 mutations detected by the named test (exit 101). 3/3 compile-fail controls rejected with the expected error code. | [c3b1-host](usb2c3b1-containment-dry-run/hosted/c3b1-host/result.json) |
| 4. Native BIOS/UEFI matrix, including refusals | **PASS 32/32** (table below). | [native](usb2c3b1-containment-dry-run/native/result.json) |
| 5. Independent PCI/MMIO write-trace checks | **PASS.** After VOS's first serial byte, all 32 boots show 0 xHCI MMIO writes and 0 xHCI configuration writes. The only configuration writes are the accepted N3 virtio-blk driver's, to its own function, in the storage boots. Register provenance: no register value was last written by VOS. Read counts match the reports exactly (411 for the eligible dry-run, 149 for a single refused pass). | `trace-analysis.json` per boot |
| 6. AHCI residual DMA | **PASS in all 32 boots.** The firmware left an AHCI engine armed (command list / FIS pages, for example 0x6750000–0x6751000 on OVMF). Those pages are unallocatable throughout. AHCI is outside scope (D6). | `ahci` in each `result.json` |
| 7. PMM and firmware-memory protection | **PASS.** Avail bitmap equal to the independent oracle at the first allocation. The guard window (entry to `screen_init`): avail/alloc bitmaps unchanged, firmware page hashes unchanged, only the existing kernel alias. Unchanged again after the GUI run, and no excluded or unavailable frame ever allocated. | `guard_window`, `low_water` |
| 8. Regressions | See [Regressions](#regressions). | [hosted](usb2c3b1-containment-dry-run/hosted) |
| 9. GUI, shell, apps, Rixie | **PASS** in every boot without a USB keyboard: GUI keys, `apps`, `hello`, `rixie`/`help`/`exit`, `echo`. Storage boots also run `ls`/`cd`/`pwd`/`cat` on both firmwares. Boots with a USB keyboard stop after the GUI presents (see limitations). | native `result.json` |
| 10. Growth, stack, RW, storage | **PASS** (Resources). | [measurements](usb2c3b1-containment-dry-run/measurements.json) |

### Native matrix (kernel-3, `--expect containment`)

`t` = `vos.usb2-contain=1`; `n` = no option; `r`/`q`/`d`/`p`/`c` = the C3a.3
modes (guard, guard+C1, guard+C2, USB.1 probe, conflict).

| Firmware | Device | Mode | Verdict | Refusal mask |
| --- | --- | --- | --- | --- |
| UEFI | qemu-xhci | t | **DRY-RUN COMPLETE** (state 6) | 0 |
| UEFI | qemu-xhci + virtio-blk | t | **DRY-RUN COMPLETE** | 0 |
| UEFI | qemu-xhci + usb-kbd | t | REFUSED: interrupts usbsts ports slots residue | 0x5c80 |
| UEFI | qemu-xhci + usb-kbd + usb-tablet | t | REFUSED: same | 0x5c80 |
| UEFI | none / nec-xhci / two qemu-xhci | t | REFUSED: unqualified | 0x2 |
| BIOS | qemu-xhci (idle) | t | REFUSED: not-uefi pci-command ports residue memory | 0xc809 |
| BIOS | qemu-xhci + virtio-blk | t | REFUSED: same | 0xc809 |
| BIOS | qemu-xhci + usb-kbd (running) | t | REFUSED: not-uefi pci-command interrupts running usbcmd usbsts ports residue memory | 0xcf89 |
| BIOS | none / nec-xhci / two qemu-xhci | t | REFUSED: not-uefi unqualified | 0x3 |
| both | qemu-xhci (+disk, +kbd) | n | no C3b.1 output | — |
| both | qemu-xhci, other controllers | r q d p c | C3a.3 results unchanged | — |

The eligible trace, from `uefi-t-qemu-xhci/serial.txt`:

```text
usb2c3b1: trace=1 revalidate ok width=0 offset=0x0 old=0xbf7f64e2 new=0xbf7f64e2
usb2c3b1: trace=2 pci-write NOT-ISSUED width=16 offset=0x4 old=0x7 new=0x3
usb2c3b1: trace=3 revalidate ok width=0 offset=0x0 old=0xbf7f64e2 new=0xbf7f64e2
usb2c3b1: trace=4 mmio-write NOT-ISSUED width=32 offset=0x40 old=0x0 new=0x2
usb2c3b1: trace=5 reset-poll NOT-RUN width=32 offset=0x44 old=0x400 new=0x800
usb2c3b1: trace=6 verify NOT-RUN width=32 offset=0x44 old=0x1 new=0x0
```

In the reset-poll line, `old` is the poll bound (1024) and `new` is the CNR
mask. In the verify line, `old` is the expected USBSTS and `new` is the expected
pointer value. The revalidation value is the observation fingerprint.

**Firmware-originated versus VOS-originated events.** SeaBIOS drives a USB
keyboard itself and leaves the controller **running**. In the BIOS keyboard
boots, the QEMU trace shows firmware-originated `usb_xhci_ep_kick`,
`xfer_retry`/`xfer_success` and `queue_event` (ER_TRANSFER into firmware memory
below 1 MiB):

- `bios-n-qemu-xhci+kbd`, the baseline boot with no VOS USB code: 1 event.
- `bios-t-qemu-xhci+kbd`: 0 events.
- Smoke runs: 1 and 5.

The count depends on timing, and the baseline shows these events without any
VOS USB code. For those boots only, the runner records the events
(`firmware_originated_state_events`) instead of requiring zero. It still
requires 0 VOS MMIO and configuration writes and no VOS register provenance. It
also skips two equality checks that a running controller legitimately breaks:
register-snapshot staleness and firmware page hashes. Every other boot still
requires 0 state events. **C3b.1 refuses this controller** (running, interrupts,
not UEFI).

### Regressions

| Suite (kernel-3 archive `4baf4f8e…` unless noted) | Result |
| --- | --- |
| USB.2-C3a guard (with the C3b.1 scenarios) | **PASS.** Residue tests 17/17. 4 compile-fail controls, all mutations detected, link guard. Sanitizer: every C3a/C3a.3 scenario plus the 24 C3b.1 scenarios. |
| USB.2-C3a.2 EFI exclusion | **PASS, sanitizer harness only:** 262,450 checks against the new archive. Its pure Rust tests and mutations are reused from C3a.3 by source identity, because `kernel/rust/boot`, `tests/rust/efi_memory.rs`, `efi_memory.c` and the harness are unchanged from the baseline (`git diff 55f6693`, empty). |
| USB.2-C2 DMA | **PASS.** 9 tests, 6 compile-fail, 3 mutations, sanitizer 32,818 checks. |
| USB.2-C1 qualification | **PASS.** 17 tests, mutations, sanitizer (with the phase-aware read budget). |
| USB.2-B ownership | **PASS.** 1,016 assertions. |
| USB.2-A portable core (`test-xhci-core.py`, full) | **PASS.** 46 host tests, 2/2 compile controls, freestanding `no_std` builds, stale-handle mutation, benchmark, 0 unsafe blocks. |
| USB.1 probe and input | **PASS.** 45,713 xHCI assertions, console handoff 49,499 assertions, keyboard and diagnostics. |
| R2-B PS/2 pointer | **PASS.** Hosted differential (16,777,216 triplets) and native matrix 4/4 (BIOS/UEFI × normal/verify; verify kernel built from this tree). |
| R1 ELF bounds | **PASS.** 497,127 bounds cases; 283 validator files. |
| N-series boot, GUI, shell, apps, storage | **PASS** in the native matrix above. |

Retained: the first C3a.2 sanitizer step exited 127 because the runner called a
shell function through `guard.sh`. It was re-run as `c3a2-san-2` (PASS). See
the failures record.

### Screenshots

Genuine running-VOS captures from this matrix (QMP `screendump`). Each shows the
GUI demo and then the shell after the apps ran. The dry-run itself prints to the
serial console only.

- UEFI: [gui](usb2c3b1-containment-dry-run/native/uefi-n-qemu-xhci/gui.png),
  [shell](usb2c3b1-containment-dry-run/native/uefi-n-qemu-xhci/shell.png)
- BIOS: [gui](usb2c3b1-containment-dry-run/native/bios-n-qemu-xhci/gui.png),
  [shell](usb2c3b1-containment-dry-run/native/bios-n-qemu-xhci/shell.png)

## Resources

Measured against the C3a.3 build of the merged baseline with the same toolchain
(`nightly-2026-09-03`); identical when re-measured.

| Quantity | Result | Ceiling |
| --- | --- | --- |
| Code + rodata growth | **8,899 B** (.text +7,936, .rodata +963) | 9,728 B (approved) |
| Permanent RW growth | **32 B** (.bss; .data +0) | 96 B |
| Largest stack frame | **480 B** (`xhci_contain_boot`, C); Rust max 472 B | 512 B |
| Loaded image (PT_LOAD memsz) | +8,192 B (two 4 KiB pages of R E) | — |
| Peak PMM use during a boot | 95 frames (unchanged class) | — |

The ceiling applies to individual frames. The call chain `xhci_contain_boot` →
`vos_xhci_contain_v1` → qualification hook → `Live::observe` → residue walk is
the deepest USB.2 chain so far. Its measured frames sum to about 2.1 KiB, plus
the C1 `qualify` frame. The total chain depth was not measured.

**Storage** (Antigravity owns storage; nothing was cleaned, moved or deleted):

- The tree is measured before each step against the 566,231,040 B stop line and
  the 20 GiB C: reserve ([storage-log](usb2c3b1-containment-dry-run/storage.json)).
- Native traces were compressed through the evidence-preserving workflow:
  deterministic gzip, decompressed and compared with the raw SHA-256 before the
  raw file was removed, then re-verified independently: 32/32 plus the 2
  failed-smoke traces.
- Each boot keeps `trace.preservation.json` (raw and gzip hashes). The `.gz`
  traces remain in `build/`; restore with `gzip -dk`.

## Retained failures

See [failures.json](usb2c3b1-containment-dry-run/history/failures.json).

- The code-size gate stop.
- The empty `measure-2`.
- The blackout.
- Two runner defects found in smoke runs before the matrix. Neither involved the
  kernel:
  - a refusal check shadowed the PMM oracle variable;
  - a missing traceback made the first failure opaque.
- A C drift test that first flipped an already-read port. The test was
  corrected; production code is unchanged.

## Limitations and residual risks

- **No hardware effect was exercised.** QEMU's xHCI reset model says nothing
  about real controllers. Reset timing, CNR behavior and BME semantics on
  physical hardware are unverified.
- **Primary specification review is incomplete.** The xHCI and PCIe
  specification texts could not be retrieved in this environment. The D2
  ordering and the reset-completion criteria rest on secondary sources and
  established driver practice (Gate 0, `spec_review`). That HCRST leaves PCI
  configuration, including BME, untouched is the expected separation, but it is
  **not confirmed from the specification**.
- **Clearing BME does not prove that in-flight requests have drained.** C3b.1
  requires Transactions Pending to be clear at each pass. C3b.2 would add a
  non-posted read-back. Some hardware reportedly signals MSI despite BME = 0.
  The QEMU profile has MSI and MSI-X disabled and is refused otherwise.
- **USB keyboard boots stop after the GUI presents.** QMP key events go to the
  USB keyboard rather than VOS's PS/2 driver, so the shell and apps were not
  driven in those 5 boots. They are covered in the other 27.
- **BIOS controllers are never eligible.** SeaBIOS leaves different state, and
  firmware memory is e820-reserved and uninspected.
- **AHCI firmware DMA is outside scope.** It is a QEMU residual (D6); its pages
  stay unallocatable.
- **CI** on this branch is reported after publication.

## C3b.2 assessment

**CONDITIONAL GO for requesting C3b.2 authorization (QEMU only).** Every C3b.1
gate passed, and the rehearsal shows the exact effects, ordering and refusal
behavior. C3b.2 would add the first device writes in the USB.2 track. These
conditions must be met first:

1. **Specification evidence.** Obtain the authoritative xHCI §4.2/§5.4.1 text
   (HCRST, CNR, register-access rules during reset) and the PCIe Command/Device
   Status text (BME, Transactions Pending), or have the owner explicitly accept
   the secondary-source review.
2. **Budget.** C3b.1 uses 8,899 B of the 12 KiB provisional allowance. A minimal
   write path plus failure handling probably needs about 1.5–3 KiB. That needs
   an explicit C3b.2 ceiling.
3. **One reviewed write primitive per effect.** Each must be bound to the leased
   BDF and BAR, reachable only from `DryRunComplete` of a fresh rehearsal in the
   same session, with the freeze flag held. It must sit behind a separate
   execute option that is exclusive with every other C-series option.

**Exact proposed hardware effects.**

- **W1:** 16-bit PCI configuration write, offset 0x04, `0x0007 → 0x0003`.
  It clears only Bus Master Enable and preserves all other and reserved bits.
- **W2:** 32-bit MMIO write to USBCMD (BAR0 + CAPLENGTH + 0x00 = +0x40),
  `0x0 → 0x2` (HCRST).

**Preconditions.** All of the following, at A, R0 and R1:

- refusal mask 0;
- UEFI with boot services exited;
- exact profile;
- PCI command 0x0007 and status 0x0010;
- TP clear;
- USBCMD 0, USBSTS exactly 1;
- every port idle;
- no slots or scratchpads;
- residue complete with excluded > 0 and no hazards;
- interrupts disabled by every measure;
- the five-step plan;
- the C1 lease held;
- the freeze flag held.

**Read-backs.**

1. **After W1:** a non-posted configuration read of command must equal 0x0003,
   with status 0x0010 and TP still clear. Then a full R1 pass must equal A
   except for BME. USBSTS must still be 1, which means HSE is not set.
2. **After W2:** poll at most 1024 times until USBCMD.HCRST = 0 and
   USBSTS.CNR = 0. Then verify:
   - USBCMD = 0 and USBSTS = 1;
   - CRCR = 0 and DCBAAP = 0;
   - every interrupter's IMAN.IE = 0 and ERSTSZ/ERSTBA/ERDP = 0;
   - PCI command still 0x0003.

**Failure states, all fail-closed.** Bus mastering is never re-enabled.
Firmware memory remains excluded for the whole boot.

| State | Meaning | What C3b.2 would do |
| --- | --- | --- |
| `BusMasterStuck` | BME still set after W1 | Stop; no reset. |
| `ContainedBmeOnly` | R1 fails after W1 | Leave BME off; no reset. |
| `BusMasterOff` | W1 confirmed | Intermediate state before W2. |
| `ResetStuck` | HCRST or CNR still set after the bound | Stop. No further register access, which the specification forbids during reset. BME stays off. |
| `ResetUnverified` | Reset completed, but a pointer or register is not cleared | Refuse; stop. |
| `Contained` | All verification passed | Success. |
| `IntegrityFault` | Lease, mapping, PMM or registry mismatch | Boot check fails. |

**Reset completion criteria.** All of the following:

- HCRST = 0 and CNR = 0 within the bound;
- USBSTS = 1 (halted, no HSE);
- USBCMD = 0;
- CRCR, DCBAAP and all interrupter registers zero;
- PCI configuration unchanged except BME.

QEMU completes the reset synchronously. A real controller may need
milliseconds, and C3b.2 must not claim otherwise.

**Remaining DMA risks.**

- In-flight requests at the moment BME is cleared (mitigated by TP plus the
  read-back).
- Non-compliant MSI after BME = 0 (none on this profile).
- No IOMMU.
- AHCI residual DMA (D6).
- BIOS and running controllers are never contained (refused).
- Firmware rings remain excluded and are never reused, even after a successful
  reset.

**Specification evidence.** Secondary sources only (Gate 0). The primary review
is **incomplete** and is condition 1 above.

## Reproduction

```sh
rustc="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
out=build/usb2c3b1-review
make ARCH=x86_64 BUILD=$out/kernel RUSTC="$rustc" verify
python3 -I -B scripts/test-usb2c3b1.py --kernel-build $out/kernel --rustc "$rustc" --out $out/c3b1
python3 -I -B scripts/test-usb2c3a.py --archive $out/kernel/rust/libvos_kernel.a \
  --kernel-map $out/kernel/kernel.map --rustc "$rustc" --out $out/c3a
python3 -I -B scripts/test-usb2c3a2-native.py --kernel $out/kernel/vos.bin --expect containment \
  --compress-traces --apps <dir with verified vsh/hello/rixie/gui-demo ELFs> \
  --efi <x86_64-efi GRUB modules> --disk <read-only accepted ext2 disk> --out $out/native
```

`--expect containment` adds the `t` boots and the USB keyboard/tablet variants
to the C3a.3 matrix. `--compress-traces` preserves each analysed trace as a
verified gzip with recorded hashes. Build smoke CI runs `test-usb2c3b1.py` with
the C3a/C3a.2 suites.
