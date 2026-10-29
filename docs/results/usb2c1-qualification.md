# USB.2-C1: Rust controller integration and read-only xHCI qualification

**Verified for PR review, not accepted or merged.** C1 does two things:

- **Links the USB.2-A Rust controller into the production kernel** without ever
  instantiating or running it.
- **Adds an opt-in, read-only qualification path** (`vos.usb2-qualify=1`) for the
  QEMU `1b36:000d` xHCI profile.

Nothing in C1 writes a PCI configuration register or an xHCI controller register.
It does not touch firmware ownership semaphores, halt or reset the controller,
enable or disable bus mastering, allocate or publish DMA, start a worker, or
enumerate USB devices.

Verification is QEMU-only. Physical hardware is NOT TESTED, and C1 does not add
USB keyboard or mouse input.

Baseline: accepted `main` at PR #26,
`d8edfcec643e541cc882062e5121175b50778f89`. Worktree
`/home/veasnawt/src/vos-usb2c1`, branch `feat/usb2c1-qualification`.
See the [exact inventory](usb2c1-qualification/changed-files.txt),
[hash manifest](usb2c1-qualification/manifest.json),
[measurements](usb2c1-qualification/measurements.json),
[preservation](usb2c1-qualification/preservation.json) and
[storage](usb2c1-qualification/storage.json) records. The architecture remains the
[USB.2 design](../plans/usb2-rust-xhci.md).

## Budget history (owner decision)

The first implementation stopped at Gate F. The 8 KiB provisional image budget
was exceeded: 21,110 B before reductions and 16,600 B after in-scope reductions,
as the [stop record](usb2c1-qualification/history/gate-f-stop-8kib.json) shows.
My Stage 0 estimate came from the 3.3 KiB USB.2-A object and left out both the
qualification code and the generated controller code.

The owner approved raising the **C1-only** text+rodata budget to **20,480 B**
([authorization](usb2c1-qualification/budget-authorization.json)). BSS (≤256 B),
stack frames (≤512 B), zero workers, zero DMA and zero heap allocations were
unchanged, and Rust optimization flags were not changed globally.

The final growth is 19,176 B. It rose from 16,600 B after the change because the
link anchor was corrected to generate the *full* controller lifecycle; it had
previously generated only the error paths (see below).

## Implementation

| Area | Change |
| --- | --- |
| One canonical Rust tree | `kernel_probe.rs` includes `kernel/rust/usb/xhci/mod.rs` once. `discovery_core.rs` and USB.2-B's duplicate `#[path]` copies of `types.rs`/`resources.rs` are removed. USB.1 discovery, USB.2-B `Owned`, C1 qualification and the controller share one `Lease`, `Kind` and `Error`. USB.2-B's adapter changed only its import line. |
| One parser | `capabilities::inspect(&mut dyn Words)` is the single xECP/profile parser. `Capabilities::parse` delegates to it with unchanged errors (USB.2-A: 46/46). It walks at most 64 headers; offsets only move forward, so cycles cannot be represented. It rejects truncation, overlap with the register blocks, malformed links and duplicate legacy support. It records USBLEGSUP and USBLEGCTLSTS without writing them. |
| PCI qualification (`qualify.rs`, safe, portable) | Requires a complete bus-0 scan with exactly one xHCI candidate. Checks class `0x0C0330` and a type-0 header. The identity must be `1b36:000d`; anything else fails closed. Checks BAR form, 16 KiB natural alignment, address width and overflow, and that memory decoding is on. Walks at most 48 PCI capabilities with DWORD alignment, valid offsets, a visited-slot cycle bitmap and truncated bodies rejected. Duplicate PM/MSI/MSI-X capabilities are rejected. Requires D0; a function with no PM capability is D0 by PCI PM definition. MSI-X table and PBA must lie inside the 16 KiB BAR. Command, BAR, PMCSR, MSI and MSI-X controls are reread and compared afterwards. |
| Linked controller | `vos_xhci_controller_link_v1` is a C-ABI symbol with no prototype and no caller. It uses a backend that refuses every operation, returning an opaque `Result`. That generates the full `acquire`/`prepare`/`poll`/`release`/`join`/`stop`/`step` lifecycle, 6,546 B, without proving success paths dead. The kernel never constructs a controller. |
| BAR lease | `devmap_scope_acquire_region` takes a qualification-bound descriptor. Only the 16 KiB profile extent is accepted, naturally aligned and UC; `devmap_check` still rejects RAM, existing mappings (including USB.1's permanent mapping) and MAXPHYADDR. It uses one fresh page table with an unmapped guard page and a generation-tagged token, and requires kernel CR3, IF=0 and a non-IRQ context. The USB.2-B fixture path keeps its exact validation and its two-table straddle. Fixture and BAR handles cannot release each other's lease. |
| C adapter (`xhci_qualify.c`) | Scans bus 0 and counts candidates. Configuration reads are limited to the selected function. The BAR descriptor is bound to the BAR register as it reads at acquisition time. Register access is aligned 32-bit volatile reads only; there is no write path. The report printer is table-driven. |
| Rust glue (`xhci_qualify.rs`) | A non-Copy, !Send/!Sync `BarLease` built on the canonical `Lease`. `BarWords` copies at most 256 DWORDs. Order: lease → inspect → USB.1 `decode_caps` on the same copied header → second observation of USBCMD, USBSTS and the legacy words → release → PCI reread. A fail-closed refusal returns 1. A changed state or a retained lease returns −1 and becomes a boot check failure. |
| Boot | `vos.usb_probe=1` together with `vos.usb2-qualify=1` is refused before either path reads or maps the controller, and is recorded as a boot check failure. Qualification runs after `sched_init`, only when selected. |

## Supported profile and observed controller state

The controller passes in both QEMU firmware modes:

- **Identity and BAR:** `1b36:000d`, BAR0 64-bit. SeaBIOS placed it at `0xfebf0000`, OVMF at `0xc000000000`.
- **PCI capabilities:** MSI-X at `0x90` (16 vectors, table `0x3000`, PBA `0x3800`) and PCIe. There is no PM capability.
- **Registers:** xHCI 1.0, CAPLENGTH `0x40`, 8 ports, 64 slots, 4 KiB pages, no scratchpads.
- **Extended capabilities:** two protocol capabilities (USB2 ports 1–4, USB3 ports 5–8). **No legacy-support capability**, so there is no firmware ownership semaphore to observe in QEMU.
- **Reads:** exactly **18** register reads.

**State left by firmware, observed and not changed by VOS:**

- **Bus mastering is already on.** PCI command is `0x107` under SeaBIOS (IO, MEM, BME, SERR) and `0x7` under OVMF (IO, MEM, BME). The controller does **not** have BME disabled.
- **The controller is halted:** USBCMD `0x0`, USBSTS.HCH `1`.
- **Firmware DMA pointers remain programmed.** CRCR, DCBAAP, ERSTBA and ERDP still point at firmware memory. These are relevant to the C2/C3 handoff design.

## Proof of no device-state modification

Every native boot ran under QEMU tracing with these events: `pci_cfg_write`, all
`usb_xhci_*`, `memory_region_ops_write` (every MMIO and port write to an ops-backed
region, covering all xHCI register blocks and the MSI-X table/PBA),
`msix_write_config` and `serial_write`. MMIO reads were traced from the GRUB prompt
onward.

The guest's serial bytes are reassembled from the same ordered log. The first byte
of `boot: x86_64 long mode` marks where VOS starts, and everything after it is
attributed to VOS ([analyzer](../../scripts/usb2c1_trace.py)).

**Positive controls.** The same traces record firmware activity before that
boundary, which proves these paths are observed:

- SeaBIOS (BIOS qualify boot): 13 xHCI MMIO writes and 20 configuration writes to `00:02.0`, including BAR sizing and the command register.
- OVMF (UEFI qualify boot): 55 xHCI MMIO writes, including its ring programming, and 58 configuration writes.
- Controller resets in both firmwares.
- OVMF's PORTSC polling just before hand-off: 18 reads in the qualify boot and 26 in the normal boot. The 18 matches VOS's own read count by coincidence; all of these reads come before the boundary.

**Results after the VOS boundary**, from the [native matrix](usb2c1-qualification/native/result.json):

| Mode/device | BIOS | UEFI | xHCI MMIO writes | Config writes (any device) | xHCI state events | xHCI reads (trace = guest) |
| --- | --- | --- | --- | --- | --- | --- |
| normal / qemu-xhci | PASS | PASS | 0 | 0 | 0 | 0 |
| qualify / qemu-xhci | QUALIFIED | QUALIFIED | 0 | 0 | 0 | 18 = 18 |
| conflict / qemu-xhci | refused | refused | 0 | 0 | 0 | 0 |
| USB.1 probe / qemu-xhci | PASS | PASS | 0 | 0 | 0 | 7 |
| qualify / no xHCI | REJECTED not found | same | 0 | 0 | 0 | 0 |
| qualify / two xHCIs | REJECTED ambiguous | same | 0 | 0 | 0 | 0 |
| qualify / NEC xHCI | REJECTED identity | same | 0 | 0 | 0 | 0 |

**Independent checks:**

- **PCI identity and BAR.** QMP `query-pci` showed the same xHCI identity and BAR regions before VOS and after the run.
- **Register snapshots.** HMP read-only register snapshots were taken after the shell exited, with read tracing off. BIOS normal and qualify snapshots are identical.
- **UEFI snapshots.** The CRCR, DCBAAP, ERSTBA and ERDP values differ between the two UEFI boots, by `0xC000` each. In each boot, every such value equals the last value OVMF wrote before VOS started, and VOS wrote none of them.

**Trace coverage limit.** The trace sees every CPU access that QEMU dispatches to
this emulated device. It cannot speak for physical hardware or for firmware SMM.

## Acceptance gates

| Gate | Result | Evidence |
| --- | --- | --- |
| A. Rust architecture | **PASS.** One canonical tree. Layout is asserted on both sides (report 224 B, align 8; `usb_resource` 16 B). Userspace and syscall ABIs are unchanged. The controller is linked: 6,546 B of controller code with link-map provenance in the Rust archive member. It is never instantiated; the only exported entry has no caller. | [measurements](usb2c1-qualification/measurements.json) |
| B. Pure Rust | **PASS.** 17 tests cover profile, unknown/ambiguous identities, header, BAR form, overflow and width, capability offsets, alignment, cycles and the 48-entry limit, duplicates, truncated bodies, D1–D3, MSI-X layout, observation, truncated and long xECP chains, duplicate legacy, semaphores, register profile and read budget. Two mutation controls (PCI cycle detection off, duplicate-legacy check off) are each detected (exit 101). | [hosted/c1-host](usb2c1-qualification/hosted/c1-host/result.json) |
| C. Hosted FFI/sanitizers | **PASS.** 76 ASan/UBSan assertions run the real PMM, devmap, USB resource and qualification C code against the actual Rust archive. They cover qualify and reject paths, overlap with a USB.1-style mapping and with RAM, page-table allocation failure, wrong CR3, IF set, IRQ context, stale, double and cross-kind tokens, base mismatch, bounds, mid-run command/MSI-X/semaphore changes (integrity failure), release failure with retention and explicit recovery, and the ABI null/misalignment checks. Zero PCI writes (the model aborts on any write). C sanitizers do not instrument the Rust code. | [sanitizer](usb2c1-qualification/hosted/c1-host/sanitizer.txt) |
| D. BIOS/UEFI native | **PASS, 14/14** (table above), plus the matrix-level firmware provenance checks. | [native](usb2c1-qualification/native/result.json) |
| E. Compatibility | **PASS.** USB.2-A 46/46. USB.2-B 1,016 sanitized assertions plus Copy/Send/Sync rejections. USB.1: 45,713 xHCI host assertions, keyboard, diagnostics and console suites, and the native probe in both firmware modes. R1: 497,127 bounds cases and 283 validator files. R2-B: 16,777,216-triplet differential and the native pointer matrix 4/4 (normal and GUI-verify on both firmwares). GUI a/b/q, vsh, apps, hello, Rixie and console restoration pass in every full boot. | [hosted](usb2c1-qualification/hosted), [regressions](usb2c1-qualification/regressions/r2b-native/result.json) |
| F. Resources | **PASS against the revised budget** (table below). | [measurements](usb2c1-qualification/measurements.json) |
| G. Evidence and preservation | **PASS.** Raw serial logs, QMP observations, trace analyses with full-trace hashes, commands, images and source identities are retained. Every failed attempt is kept under [history](usb2c1-qualification/history). Refs, worktrees and stashes are preserved; storage is disclosed. | [preservation](usb2c1-qualification/preservation.json) |

### Resources

These are input-section bytes from the GNU ld maps against the baseline built
from `d8edfce` with the same toolchain. Page-rounded loader sizes are shown
separately.

| Measure | Result | Limit |
| --- | ---: | ---: |
| Text + rodata growth | **19,176 B** (text 16,171, rodata 3,005) | 20,480 B |
| Loaded PT_LOAD growth, page-rounded | 16,384 B (code segment only) | — |
| BSS growth | 56 B (devmap 32, qualifier 24) | 256 B |
| Largest Rust frame | 488 B (`vos_xhci_controller_link_v1`, never called) | 512 B |
| Largest frame on the qualification path | 328 B (`inspect`), 312 B (`vos_xhci_qualify_v1`) | 512 B |
| Largest C frame | 304 B (`xhci_qualify_boot`) | 512 B |
| Runtime mapping | One page table plus a 16 KiB BAR window, released | ≤1 PT + window |
| Worker threads, DMA, heap | 0, 0, 0 | 0 |

Growth by contributor:
- **Rust text:** 13,195 B, including 6,546 B of linked controller code.
- **Qualifier C:** 2,208 B text and 1,029 B field table.
- **devmap:** 544 B.
- **`kernel.o`:** 224 B text and 1,317 B of merged strings.

**One-time qualification cost:** the median is 8.4 µs per qualification on the
host, measured in an optimized, unsanitized build with modelled MMIO and real
PMM/devmap work. That is not a hardware latency, and the cost inside the guest
was not measured separately.

## Retained failures and corrected reruns

These are kept under [history](usb2c1-qualification/history):

1. **Original 8 KiB Gate F stop** and the measurements from size attempts 3–5.
2. **First probe boot.** QEMU tracing failed because of malformed `-trace` arguments, so only the serial log exists. The calibration run then validated `-trace events=FILE,file=OUT`.
3. **Pure Rust first run:** 2 failures, both test-vector errors (an exact 2^40 BAR end and an unnamed protocol capability).
4. **First C1 hosted run on `kernel-5`.** The duplicate-legacy mutation **survived** because the test vector was too weak; the vector was strengthened.
5. **USB.2-B hosted link failure on `kernel-5`.** `Owned`'s drop glue referenced a C1 symbol. This was fixed by moving the BAR lease into its own type and keeping USB.2-B's code unchanged.
6. **Native run 1: 7 of 14 cases failed.** The analyzer's prefix `usb_xhci_run` also matched `usb_xhci_runtime_read`, and my HMP snapshot reads were traced. Re-analysis showed zero writes.
7. **Native run 2: all cases passed, but the UEFI cross-boot snapshot check failed.** The check wrongly assumed firmware places its rings deterministically. It was replaced by per-boot firmware provenance.

## Reproduction

```sh
rustc="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
out=build/usb2c1-review
make ARCH=x86_64 BUILD=$out/kernel RUSTC="$rustc" verify
python3 -I -B scripts/test-usb2c1.py --archive $out/kernel/rust/libvos_kernel.a --rustc "$rustc" --out $out/host
python3 -I -B scripts/test-usb2c1-native.py --kernel $out/kernel/vos.bin \
  --apps <dir with verified vsh/hello/rixie/gui-demo ELFs> --efi <x86_64-efi GRUB modules> --out $out/native
python3 -I -B scripts/measure-usb2c1.py --baseline <baseline build> --candidate $out/kernel --out $out/measure
```

Build smoke CI runs `test-usb2c1.py` and `test-usb-resources.py` against the archive
it builds. The native matrix needs QEMU, OVMF and local inputs.

## Limitations and C2 boundary

**What C1 qualifies:**

- **Device profile:** one emulated QEMU device profile, on PCI bus 0 only.
- **BAR extent:** 16 KiB, a property of the QEMU device model cross-checked by MSI-X placement, not sized by PCI writes.
- **Firmware handoff:** not observable in QEMU, because the device has no legacy-support capability.

**What C1 does not establish:**

- **DMA:** addressability, coherency and IOMMU state.
- **Operational support:** the operational profile stays unsupported.
- **Physical hardware:** none of this qualifies real PCs, including the owner's USB-input machine.

**Input for C2/C3 design:** firmware leaves BME enabled and its own ring pointers
programmed. Ownership and DMA design must quiesce or account for that firmware
state rather than assume a clean controller.

C2 (DMA ownership) and later increments need separate authorization.
