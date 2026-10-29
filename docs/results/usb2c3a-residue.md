# USB.2-C3a: firmware DMA residue guard and dry-run controller lifecycle

**Verified for PR review, not accepted or merged.** C3a adds an opt-in
(`vos.usb2-residue=1`) boot guard. The guard reads the xHCI state that firmware
left behind, finds every page the firmware's command ring, event ring, ERST and
DCBAA occupy, and reserves those pages in the kernel PMM for the rest of the boot
so that no later allocation can reuse memory a still-armed controller may DMA
into. It then generates and validates a halt/reset plan. **Nothing executes that
plan.**

**No device write.** C3a writes no PCI configuration register, xHCI operational
or runtime register, doorbell or legacy ownership semaphore. It performs no
reset, run/stop change, ring publication, transfer or interrupt enablement.
Verification is QEMU-only; physical hardware is NOT TESTED and physical
controller takeover remains unauthorized.

Baseline: accepted `main` at PR #28, `62ce8de2b3d4b12e53c465f9ad3bb92fb0bfeffe`.
Worktree `/home/veasnawt/src/vos-usb2c3a`, branch `feat/usb2c3a-residue-guard`.
See the [exact inventory](usb2c3a-residue/changed-files.txt),
[hash manifest](usb2c3a-residue/manifest.json),
[measurements](usb2c3a-residue/measurements.json),
[budget](usb2c3a-residue/budget.json), [storage](usb2c3a-residue/storage.json)
and [preservation](usb2c3a-residue/preservation.json) records.

## Design gate: reservation ordering

**The risk is real.** In a normal UEFI boot, OVMF's xHCI ring and context pages
sit in multiboot2 type-1 memory. The PMM therefore treats them as free; every
normal, probe and conflict boot below shows them with `avail=1 alloc=0`. Firmware
also leaves bus mastering enabled. Before C3a, nothing prevented the PMM from
handing those pages to a kernel user.

**How the guard is ordered.** The boot order is:

```text
resv -> pmm_init -> vmm_init -> kmap_init -> devmap_init -> [C3a guard] -> screen_init -> USB.1 probe -> irq_init -> sched_init -> USB.2-B -> C1 -> C2 -> vblk
```

The guard needs the device window to read the BAR, so it cannot run before
`devmap_init`. It runs immediately afterwards, before the screen, IRQ, scheduler,
USB and storage users.

**The guard does not trust its position.** Three mechanisms establish that no
firmware page was reused before the reservation:

1. **Allocation count.** The guard records `pmm_allocated_before`, the number of
   frames allocated since `pmm_init`. It is 1 in every native boot: the devmap
   page directory.
2. **Per-frame check.** Before reserving a page, the guard consults the PMM. A
   page whose alloc bit is already set is **never** reserved: it is reported as a
   HAZARD and the plan is refused, because the page may already have been
   written.
3. **Independent observation.** After the shell exits, QEMU HMP reads VOS's own
   `kernel_pmm` avail/alloc bitmaps for every firmware page, with no help from
   the guard. In every UEFI guard boot, each firmware page shows `avail=1 alloc=1`
   and appears in the guard's reserved list.

The PMM is first-fit from the lowest free frame, so early allocations land just
above the kernel image (C2's later frames are at 0x1dc000), far below the
firmware pages (≈0x6750000). The guard's correctness does not rely on that:
the per-frame check would refuse if one had been reused.

**What C3a does not do: true pre-PMM reservation.** Reserving firmware pages
before `pmm_init` would need the device window, and so static early MMIO page
tables, before the allocator exists. That is a boot-memory architecture change
and outside C3a's authority. C3a instead proves after the fact, per frame, that
no reuse happened, and fails closed if any did. A narrow proposal is in
[Limitations](#limitations-and-residual-risks).

## Architecture

| Layer | Responsibility |
| --- | --- |
| `kernel/rust/usb/xhci/residue.rs` (canonical, safe, portable) | `assess()` walks the DCBAA (slots + 1 entries, scratchpad array, device contexts of 1 or 2 KiB per CSZ), the command ring (bounded TRB scan, Link TRBs read raw, at most 8 pages and 4 segments), and every interrupter's ERST (size ≤ 2^ERST_Max, 64-byte alignment, 16–4096 TRB segments, ERDP inside a segment). Through the `Memory` trait it **protects a page before reading it**: it reads only pages it owns and has no write method (compile-fail control). Each finding is a bit; `complete()` requires zero findings and a halted controller. |
| `kernel/rust/usb/xhci/lifecycle.rs` (canonical) | `admit()` checks preconditions; `plan()` emits `MaskInterrupter(i)…, Halt, AwaitHalted, DisableMaster, Reset, AwaitResetReady, VerifyCleared`; `validate()` is a state machine that rejects bad length, duplicates, misordering, a missing mask/halt/master-off step, incomplete plans and trailing steps. `Plan` can only come from `plan()` (E0451), and `Step` has no Run (E0599). Each `Step` maps onto the canonical `controller::Action`/observation vocabulary; no executor exists. |
| `arch/x86_64/rust/xhci_residue.rs` | `Kernel: Memory` over the C registry. Registers are read through C1's `BarWords` inside C1's qualification lease, using C1's `qualify()` with a new inspection hook, so C1's double-observation and refusal rules apply unchanged. Exports `vos_xhci_residue_v1` with a 352-byte report whose layout is asserted on both sides. |
| `arch/x86_64/xhci_qualify.{c,h}` | A 16-frame residue registry, open only during the guard session. `xhci_residue_page` classifies a frame as free, owned, allocated, protected (outside usable memory or unaligned) or untracked (beyond the PMM range). `xhci_residue_reserve` refuses duplicates, a full registry and PMM failure. `xhci_residue_read64` permits only 8-byte-aligned volatile reads inside owned frames through the kernel RAM alias. Integrity checks follow, then verdict printing. |
| `arch/x86_64/pmm.{c,h}` | `pmm_reserve_frame`: refuses frames that are not ready, unaligned, out of range, not available (`EPROTECTED`) or already allocated (new `PMM_EINUSE`). It panics only if accounting is corrupt (`free == 0`). It never zeroes or touches the frame. **There is no release path.** |

**Untrusted addresses are never read directly.** A physical address taken from
a register or a firmware structure is only dereferenced after its page passes
`xhci_residue_page` as usable RAM inside the PMM range and has been reserved.
Pages in reserved or untracked memory are not inspected at all; they become
UNINSPECTED and the guard refuses.

**Separation from C2.** Residue ownership lives in its own registry, not in the
C2 pool. In guard + C2 boots, C2's frames (0x1dc000–0x1df000) are disjoint from
the reserved firmware pages.

## PMM protection guarantees

| Requirement | Mechanism and evidence |
| --- | --- |
| No already-allocated frame silently becomes reserved | `EINUSE` in the PMM, `Page::Allocated → HAZARD` in Rust. Covered by the hosted "already-allocated frame" scenario (8 checks) and the Rust test `already_allocated_frame_is_a_hazard_never_reserved`; removing the check is caught by the `reserve-allocated` mutation. |
| Duplicates do not corrupt accounting | A frame already owned counts as `owned_again`, and the registry refuses duplicates (-3). Hosted integrity check: `pmm free + count == before`. |
| Reservations last the whole boot; no automatic or timer release | No release function exists, in the PMM or the registry. |
| Exhaustion fails closed | Registry full → `Err(Budget)` → EXHAUSTED → refused, with existing reservations kept (hosted: 7 checks). |
| Uncertain ownership is never reported as safe | Protected or untracked pages become UNINSPECTED and the guard refuses. The `inspect-protected` mutation is detected. |
| Existing reservations stay intact | `EPROTECTED` for unavailable frames; the guard never changes avail bits. |
| A mid-guard state change is caught | The hosted scenario fails the integrity check while keeping the reservations. |

## Residue found per firmware

| | OVMF (UEFI) | SeaBIOS |
| --- | --- | --- |
| Controller | halted (USBSTS.HCH=1, R/S=0), bus mastering on, IMAN0.IE set | halted, bus mastering on |
| Pointers (guard boot) | CRCR 0x6756241, DCBAAP 0x6756000, ERSTBA0 0x6759240, ERDP0 0x6757240, segment of 512 TRBs | CRCR 0x7fdfc00, DCBAAP 0x7fdfd80, ERSTBA0 0x7fdfd40, segment of 16 TRBs |
| Memory type | multiboot2 type 1: **reusable by the PMM** | e820 reserved: never handed out |
| Guard result | **complete**: 7 page claims, 4 frames reserved (0x6756000 DCBAA, 0x6757000 command ring, 0x6759000 ERST, 0x6758000 event segment), 3 owned again, 0 findings | **refused**: pages are protected, so their contents cannot be inspected safely (UNINSPECTED); nothing reserved, and nothing is needed because the PMM cannot allocate them |
| Dry-run plan | `0x10 0x2 0x3 0x4 0x5 0x6` (mask interrupter 0, halt, await halted, bus master off, reset, await reset ready, verify cleared): **PLAN VALID (dry run, not executed)** | none (refused) |

OVMF's page locations differ between boots (0x674a000 or 0x6756000 in this
matrix), so the guard discovers them each boot and never assumes an address.

## Acceptance gates

| Gate | Result | Evidence |
| --- | --- | --- |
| A. Pure Rust and adversarial fixtures | **PASS.** 12 tests: OVMF fixture, SeaBIOS uninspected, already-allocated hazard, duplicates, registry exhaustion, running controller, device contexts and scratchpads present, malformed ERST/ERDP, bounded ring walk (Link-pointer reserved bits, cycles, page and segment limits), plan generation, every refusal, and every ordering violation. 3 compile-fail controls: no `Step::Run` (E0599), no `Memory::write64` (E0599), `Plan` cannot be constructed (E0451). 4 mutation controls, each detected (exit 101): reserve-allocated, inspect-protected, masked-link-pointer, no-master-off-check. The core portability scan passes. | [c3a-host](usb2c3a-residue/hosted/c3a-host/result.json) |
| B. Hosted C/Rust under ASan/UBSan | **PASS.** 11 scenarios, 91 checks: the real PMM, devmap, C1 qualification and residue C code against the bare-metal Rust archive, with zero PCI writes. Covers OVMF-like completion, already-allocated, reserved-memory residue, exhaustion, running controller, device context, unsupported and absent controllers, a mid-guard state change, bad context, and the direct PMM/registry API. Rust code is not sanitizer-instrumented. | [sanitizer](usb2c3a-residue/hosted/c3a-host/sanitizer.txt) |
| C. Link guard | **PASS.** The kernel map contains no Rust `core-`/`compiler_builtins-` archive member. The guard is permanent in the test and in CI; it was added after candidate kernel-1 pulled in core formatting through a fallible slice index (+181,730 B). | [history](usb2c3a-residue/history/kernel1-core-linked-measurements.json) |
| D. BIOS/UEFI native | **PASS 15/15** (run 1: 13/15 with 2 runner parse failures, retained; the 2 affected cases rerun in run 2). See the table below. | [run 1](usb2c3a-residue/native/native-run-1/result.json), [run 2](usb2c3a-residue/native/native-run-2/result.json) |
| E. Independent PMM evidence | **PASS.** HMP reads of VOS's `kernel_pmm` bitmaps: OVMF firmware pages `avail=1 alloc=1` in every guard boot (r, d, q), and `alloc=0` without the guard (n, p, c). SeaBIOS pages `avail=0` throughout. | per-case `result.json` → `firmware_pages_pmm` |
| F. Zero device writes | **PASS.** Traced events after VOS's first serial byte show 0 xHCI MMIO writes, 0 xHCI or other PCI configuration writes, and 0 state events in all 15 boots. | per-case `trace-analysis.json` |
| G. Regressions | **PASS.** USB.2-A 46/46. USB.2-B 1,016 sanitized assertions. C1: 17 Rust tests, 2/2 mutations, 76 sanitized assertions. C2: 9 Rust tests, 6 compile-fail controls, 3 mutations, sanitized exercise. USB.1 host suites (45,713 xHCI assertions). R1: 497,127 bounds cases and 283 validator files. R2-B: hosted differential plus the native pointer matrix 4/4. Native: C1 qualification (q), the C2 exercise (d) and the USB.1 probe (p) inside the C3a matrix. GUI, vsh, apps, hello and Rixie pass in every full boot. | [hosted](usb2c3a-residue/hosted), [R2-B native](usb2c3a-residue/regressions/r2b-native/result.json) |

### Native matrix

Mode key:
- **n:** normal boot.
- **r:** guard.
- **q:** guard + C1.
- **d:** guard + C2.
- **p:** USB.1 probe.
- **c:** probe + guard. This boot is refused at boot by the exclusivity check, and neither runs.

Read counts are xHCI MMIO reads attributed to VOS. The guard makes 127 reads;
q adds C1's 18 reads.

| Case | Verdict | Reserved | Firmware pages alloc | xHCI writes | Config writes | State events | xHCI reads |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: |
| bios-n | (no guard) | 0 | 0 (avail 0) | 0 | 0 | 0 | 0 |
| bios-r | REFUSED (uninspected) | 0 | 0 (avail 0) | 0 | 0 | 0 | 127 |
| bios-q | REFUSED (uninspected) | 0 | 0 (avail 0) | 0 | 0 | 0 | 145 |
| bios-d | REFUSED (uninspected) | 0 | 0 (avail 0) | 0 | 0 | 0 | 127 |
| bios-p | (probe) | 0 | 0 (avail 0) | 0 | 0 | 0 | 7 |
| bios-c | boot check refuses the conflict | 0 | 0 (avail 0) | 0 | 0 | 0 | 0 |
| bios-r, no xHCI | REFUSED | 0 | n/a | 0 | 0 | 0 | 0 |
| bios-r, NEC xHCI | REFUSED (unsupported) | 0 | n/a | 0 | 0 | 0 | 0 |
| bios-r, two xHCIs | REFUSED (unsupported) | 0 | n/a | 0 | 0 | 0 | 0 |
| uefi-n | (no guard) | 0 | **0** | 0 | 0 | 0 | 0 |
| uefi-r | PLAN VALID | 4 | **1** | 0 | 0 | 0 | 127 |
| uefi-q | PLAN VALID | 4 | **1** | 0 | 0 | 0 | 145 |
| uefi-d | PLAN VALID | 4 | **1** | 0 | 0 | 0 | 127 |
| uefi-p | (probe) | 0 | **0** | 0 | 0 | 0 | 7 |
| uefi-c | boot check refuses the conflict | 0 | **0** | 0 | 0 | 0 | 0 |

Firmware's own activity before the boundary serves as a positive control for
the tracer: in uefi-r, for example, the firmware made 55 xHCI MMIO writes,
58 xHCI configuration writes and 203 state events. The trace files and ISOs stay in `build/`; their hashes are in
[media.json](usb2c3a-residue/media.json).

### Screenshots

These are genuine running-VOS captures from the guard boots (mode r): the
desktop and the vsh shell running apps, hello and Rixie.

| | Desktop | Shell |
| --- | --- | --- |
| BIOS | ![BIOS desktop](../images/usb/usb2c3a/vos-usb2c3a-bios-gui.png) | ![BIOS shell](../images/usb/usb2c3a/vos-usb2c3a-bios-shell.png) |
| UEFI | ![UEFI desktop](../images/usb/usb2c3a/vos-usb2c3a-uefi-gui.png) | ![UEFI shell](../images/usb/usb2c3a/vos-usb2c3a-uefi-shell.png) |

## Resources

Measured against a kernel built from `62ce8de` with the same toolchain. The
ceilings come from the C3a authorization and did not change during execution.

| Measure | Result | Ceiling |
| --- | ---: | ---: |
| Code + read-only data (input sections) | **9,107 B** (text 7,887, rodata 1,220) | 10,240 B |
| Permanent RW growth | **160 B** (`.bss`: the 16-entry registry; `.data` +0) | 256 B |
| Firmware-residue reservations | registry of 16 frames; 4 used under OVMF, 0 under SeaBIOS | 16 |
| Largest stack frame | **432 B** C (`xhci_residue_boot`), 424 B Rust | 512 B |
| Loaded PT_LOAD growth, page-rounded | 8,192 B (code segment) | — |
| New threads, heap, DMA publication | 0, 0, 0 | 0 |
| Storage (build tree) | 500,346,880 B | 629,145,600 B (stop at 566,231,040) |

**Growth by contributor:**
- **Rust:** 5,759 B text and 176 B rodata.
- **`xhci_qualify.o`:** 1,800 B text and 352 B rodata.
- **`pmm.o`:** 208 B text and 8 B rodata.
- **`kernel.o`:** 120 B text and 684 B strings.

## Retained failures

These are kept under [history](usb2c3a-residue/history) and
[native-run-1](usb2c3a-residue/native/native-run-1):

1. **Kernel archive compile error.** Casting a data-carrying `Violation` to u32 failed; it was replaced by `Violation::code()`.
2. **Candidate kernel-1 over budget (+181,730 B).** A fallible slice range in `Plan::steps` linked Rust core/compiler_builtins. It was replaced with panic-free `get`, and the permanent link guard was added.
3. **First pure Rust run: a production defect.** The Link TRB pointer's reserved bits were masked before validation, so a malformed target was accepted. It now reads the raw pointer. The test was unchanged, and the `masked-link-pointer` mutation keeps the defect detectable. Candidate kernel-2 measurements, before this fix, are retained.
4. **Native run 1: 13/15.** For bios-q and uefi-q, the runner parsed the later C1 report's read count instead of the guard's (127 + 18 = 145, exactly what the trace showed). The parser was fixed, and only those two cases were rerun into run 2. Production code was unchanged.
5. **Ad-hoc archive rebuild hash mismatch.** The ad-hoc command did not use the make rule's recipe. Identity was proven with a real `make` rebuild: `kernel-repro` equals kernel-3 byte for byte (vos.bin `0b1e8f53…`, archive `124ba537…`).

## Limitations and residual risks

1. **Opt-in only.** Default boots still leave OVMF's xHCI pages allocatable, exactly as before C3a; the n/p/c rows show `alloc=0`. Making the guard default, or required before any C3b write, is a later decision.
2. **SeaBIOS refuses.** Its residue is in reserved memory that the guard deliberately does not read. This is safe, because the PMM cannot allocate it, but no plan is produced on BIOS.
3. **QEMU-specific observability.** The CRCR ring pointer is readable on QEMU's xHCI, but the xHCI specification lets real controllers return 0. A controller that hides CRCR would need a different discovery path and would be refused today, because the ring cannot be found.
4. **No IOMMU/DMAR qualification.** Protection is PMM-only. A device could still DMA to firmware-pointed pages, which is precisely why they are reserved, not reused.
5. **Not pre-PMM.** The guard runs after one PMM allocation and proves per frame that no reuse happened. **Proposal, not implemented:** a narrow early pass, placed after `resv` and before `pmm_init`, that maps only the qualified BAR's first page through a static early page table, reads CRCR/DCBAAP/ERSTBA, and feeds the pages into `resv`. This changes the boot-memory architecture and needs separate authorization.
6. **The plan is never executed.** Halt and reset behaviour, timeouts and post-reset verification are untested. That is C3b's scope and needs separate authorization.
7. **One profile.** QEMU `qemu-xhci` with OVMF and SeaBIOS only; there is no physical-hardware claim.

## Reproduction

```sh
rustc="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
out=build/usb2c3a-review
make ARCH=x86_64 BUILD=$out/kernel RUSTC="$rustc" verify
python3 -I -B scripts/test-usb2c3a.py --archive $out/kernel/rust/libvos_kernel.a --rustc "$rustc" \
  --kernel-map $out/kernel/kernel.map --out $out/host
python3 -I -B scripts/test-usb2c3a-native.py --kernel $out/kernel/vos.bin \
  --apps <dir with verified vsh/hello/rixie/gui-demo ELFs> --efi <x86_64-efi GRUB modules> --out $out/native
```

Build smoke CI runs `test-usb2c3a.py`, with the link guard, alongside the C2, C1
and USB.2-B host suites.
