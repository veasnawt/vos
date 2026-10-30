# VOS Status

Current accepted graphical baseline: owner-merged
[PR #15](https://github.com/veasnawt/vos/pull/15),
[`bf5eb43f2e07478e31be8b4123c5a297add67efe`](https://github.com/veasnawt/vos/commit/bf5eb43f2e07478e31be8b4123c5a297add67efe).
VOS is pre-alpha. **N0-N5 are accepted; N6 is still in progress.**
No physical-hardware or complete-desktop acceptance is claimed.
The accepted main used for this review is
`662a3ccb6d1111393c30c486ca4af657fac5808e`, following R0/R1,
[USB.1 integration PR #22](https://github.com/veasnawt/vos/pull/22),
[portable USB.2-A PR #23](https://github.com/veasnawt/vos/pull/23),
[USB.1 Rust migration PR #24](https://github.com/veasnawt/vos/pull/24) and the
[USB.2-B ownership foundation](results/usb2b-resource-ownership.md)
([PR #25](https://github.com/veasnawt/vos/pull/25)). USB.2-B executes real
non-device mapping/PMM and worker-reaping fixtures; live xHCI, DMA qualification
and USB device operations remain unimplemented.
The [R2-B Rust PS/2 pointer decoder](results/r2b-ps2-pointer-rust.md) is merged
through [PR #26](https://github.com/veasnawt/vos/pull/26): production
`pointer_decode()`/`pointer_idle()` are Rust. It is not a USB input fix.
[USB.2-C1 read-only qualification](results/usb2c1-qualification.md) is merged
through [PR #27](https://github.com/veasnawt/vos/pull/27): the USB.2-A controller is
linked but never activated, and opt-in `vos.usb2-qualify=1` inspects the QEMU
`1b36:000d` profile read-only.
The [USB.2-C2 DMA ownership foundation](results/usb2c2-dma.md) is merged through
[PR #28](https://github.com/veasnawt/vos/pull/28): a fixed-table Rust pool for
DMA-designated pages with publication statically unavailable.
The [USB.2-C3a firmware residue guard](results/usb2c3a-residue.md) is merged
through [PR #29](https://github.com/veasnawt/vos/pull/29): opt-in
`vos.usb2-residue=1` and a dry-run halt/reset plan that nothing executes.
The [USB.2-C3a.2 early UEFI firmware memory exclusion](results/usb2c3a2-efi-exclusion.md)
is merged through [PR #30](https://github.com/veasnawt/vos/pull/30): on every UEFI
boot, a Rust decision over GRUB's EFI memory map removes BootServicesCode/Data and
any other non-RAM or unclassified memory from the allocatable set before
`pmm_init`, so before the first allocation; a UEFI boot without a valid EFI map is
refused. BIOS boots are unchanged.
The [USB.2-C3a.3 protected firmware DMA inspection](results/usb2c3a3-protected-inspection.md)
is merged through [PR #31](https://github.com/veasnawt/vos/pull/31). The opt-in residue guard
reads that excluded firmware memory in place through the existing kernel alias
(no new mapping, allocation or reservation), validates the xHCI rings, and reports
reuse protection and inspection completeness separately; a complete inspection
never makes firmware memory releasable. BIOS residue (e820-reserved) stays
protected but uninspected. No PCI/xHCI write; physical hardware remains unsupported.
The [USB.2-C3b.1 containment dry-run](results/usb2c3b1-containment-dry-run.md)
is merged through [PR #32](https://github.com/veasnawt/vos/pull/32): opt-in
`vos.usb2-contain=1` admits only an idle QEMU xHCI left by UEFI, re-observes it
twice and records the proposed bus-master clear and HCRST as `NOT-ISSUED` trace
data, with no PCI-write or register-write path.
The [USB.2-C3b.2 controlled containment](results/usb2c3b2-controlled-containment.md)
is merged through [PR #33](https://github.com/veasnawt/vos/pull/33) at
`03e7532aba739c636438c887e8aa9d6169f4cfcb`. **It exists only in a
separately built, explicitly QEMU-only test image** (`VOS_CONTAIN_TEST=1` plus an
acknowledgement token on the make command line, a `contain-test` build directory,
a goal whitelist, and a per-link binary check that production kernels carry no
trace of it). Behind the one-shot `vos.usb2-contain-execute=1`, and only for the
strictly eligible idle UEFI QEMU xHCI in a verified QEMU 10.2.1 TCG environment,
it makes exactly two writes: a 16-bit PCI Command write clearing Bus Master
Enable, then a 32-bit USBCMD read-modify-write requesting HCRST, each verified,
with every later outcome final and bus mastering never re-enabled. Real xHCI
reset behavior, timing, Transactions Pending and DMA drain remain unverified;
physical hardware is NOT TESTED and unauthorized.
This branch's [USB.2-C4.1 hosted initialization protocol model](results/usb2c41-hosted-init-model.md)
is hosted-only and unmerged: a `no_std`, `forbid(unsafe_code)` Rust state machine under
`kernel/rust/usb/xhci_init/` that no kernel, ISO or Makefile input references, verified by
Rust tests, an independent Python oracle and mutation controls. It performs no I/O, enables no
bus mastering and writes no register; its register writes are values returned to a test.

## Current accepted components

| Component | Delivered scope | Accepted integration and evidence |
| --- | --- | --- |
| N0-N3 | Boot, protected kernel/userspace, scheduling/IPC and capability-scoped read-only storage | [N3 closure](results/n3-closure.md) |
| N4 | Keyboard, buffered console, Rust vsh/SDK and deterministic offline Rixie | [N4 closure](results/n4-closure.md) |
| N5 | Bounded kernel Ethernet/ARP, IPv4/ICMP, UDP and passive TCP data/retry/closure | [N5 closure](results/n5-tcp-close.md), PR #7 at `39b40a9` |
| Unified applications | Common payload/catalog, ABI v4 execution grants/broker, isolated children and foreground console transfer/restoration | [Payload](results/app-payload.md), [execution](results/app-exec.md), [handoff](results/app-handoff.md); PRs #9-#11, integrated at `e211c738d9f0b98a0ddb717b4d17f0063dfe38f0` |
| N6.1 | Validated framebuffer scene and bounded console viewport | [Desktop slice](results/n6-desktop.md); PR #12 at `3308cc86cf99d98da4cb353e6678d7782e623d47` |
| N6.2 | One native process-owned GUI surface, bounded pixel/event ABI v5 and input-worker bootstrap cleanup | [GUI and hardening](results/n6-gui.md); PR #13 at `dfd292ca807370a0ceaf735d319fc1d15bd45370` |
| N6.3 | PS/2 pointer input, primary-button/capture/loss handling, secure routing and clipped cursor | [Pointer evidence](results/n6-pointer.md); PR #14 at `78cc80c9ac2b8b9d36ffb411ab06f5a63811bbc3` |
| N6.3 cursor refinement | 21x28 visible arrow on a 32x32 canvas; 4,096-byte underlay and exact tip hotspot | [Refinement](results/n6-cursor-refinement.md); PR #15 at the baseline above |

The shell's `apps`, `hello` and `rixie` commands now use the trusted catalog
and kernel execution broker. Children receive explicit grants; execution,
filesystem and network authority are not inherited automatically. Console
ownership returns only to a live authorized parent after child termination and
kernel-CR3-safe reclamation. Normal interactive sessions have no verification
deadline. Arbitrary paths, installed packages, background jobs and general
external-program launching are not supported.

N6.2's accepted bootstrap hardening verified 18 injected ELF-load failures per
firmware, with one input worker created and reclaimed per iteration.
N6.3's original hosted results include 3,194,258 sanitizer assertions, three Rust
tests and 16 logger cases. The cursor refinement adds 2,179,862 fresh renderer
assertions; unaffected service/console/Rust/logger evidence is reused by source
identity. These are per-increment counts, not a single freshly repeated suite.
Both firmware paths have native movement, primary press/release, exact-corner,
clipping, keyboard, cancellation and application-restoration evidence.

Authentic current captures: [BIOS](images/n6/vos-cursor-refined-bios.png) and
[UEFI](images/n6/vos-cursor-refined-uefi.png).
The owner personally completed the refinement's full manual QEMU checklist;
all checks passed. See the [evidence index](results/n6-cursor-refinement/evidence.json)
and [normal preview commands](results/n6-cursor-refinement.md#normal-interactive-preview).
Automated overflow/timing verification is separate from ordinary manual use.

## R0 and first R1 helper merged

The [R0-A assessment](results/r0a-rust-kernel-assessment.md) is complete at
`689260f6c93c97e9d062e7829f078a13e92350be`. Its dated read-only findings and
then-proposed R0-B work are preserved unchanged. The assessment is included in
this publication tree, not available only in a private checkout.

[R0-B](results/r0b-rust-kernel-foundation.md), implemented at
`8e2f247dacf7a5d2a4f37db7c73df191f6c4cc1b`, executes one scalar `no_std` Rust
function inside the higher-half C/assembly kernel and forwards Rust panic to the
existing C handler. Local evidence records 4,134 C/Rust ABI checks and ten
BIOS/UEFI cases, including actual panic and incorrect-result controls. The
five-segment layout, 4 MiB image limit and application ABIs v0-v5 are preserved.
Both R0-A and R0-B are merged through PR #20 at
`0291b1ab8f7d7d491bd2f276efa59a46b638d400`.
This is a kernel integration proof, not a migrated subsystem or physical
hardware qualification. See the [publication review](results/r0-publication-review.md)
for final-tree checks and the exact provenance.

The [first R1 candidate](results/r1-elf-bounds.md), commit
`f67ebd968169560979fc1d5eefbc7ca6402a6910`, migrates the real ELF `inside`
predicate to the same Rust kernel archive. Local boundary, host-tool and native
loader checks pass. The [performance/publication review](results/r1-performance.md)
compares actual original C and Rust-backed paths, including call overhead and
timing variation. R1 is merged through PR #21 at
`7c8082b98751f65ab0bcdf884339a6bf8298e2ce`. Dated R0/R1
reports and original evidence retain their historical status wording.

## USB.1 accepted discovery and Rust migration candidate

The original USB.1 commit `bd22803b8b5635b776c848d08d1494f65b49dd21`
is integrated without rewriting its history or evidence. The
[separate integration report](results/usb1-rust-integration.md) records fresh
host, Rust-backed ELF and BIOS/UEFI checks on the accepted R1 baseline.
`vos.usb_probe=1` enables bounded PCI bus-0 discovery, assigned-BAR validation
and capability-header reads. No reset, DMA, bus-master enablement, ownership
handoff, USB transfers, device enumeration or USB HID driver is implemented.
Normal input remains PS/2. Physical hardware is NOT TESTED. USB.1 is merged
through PR #22. Its original integration report retains its dated status.

USB.2-A's [portable Rust core](results/usb2a-rust-core.md) is merged through
PR #23. It is a tested software lifecycle/ring/resource model without a live
MMIO/DMA/reset backend. USB.2-B's non-activating ownership foundation is merged
through PR #25, USB.2-C1 read-only qualification through PR #27 and USB.2-C2
software DMA ownership through PR #28 and the USB.2-C3a residue guard through
PR #29 and the USB.2-C3a.2 early UEFI firmware memory exclusion through PR #30
and the USB.2-C3a.3 protected firmware DMA inspection through PR #31.
The USB.2-C3b.1 read-only containment dry-run is merged through PR #32. The
USB.2-C3b.2 QEMU-only controlled containment is merged through PR #33. The USB.2-C4.1 hosted
protocol model is a review candidate on this branch and executes nowhere; native USB.2-C4
controller initialization, physical-PC activation and USB HID remain unimplemented.

The [USB.1 Rust publication review](results/usb1-rust-publication-review.md)
records the candidate production migration, preserved read-only behavior,
shared header validation, explicit FFI contracts and real Rust execution traces.
It retains one extra 4 KiB kernel page and higher synthetic software cost.
The original pre-kernel UEFI timeout remains failed evidence; a bounded paired
comparison did not reproduce it. Cause and hardware reliability remain unproven.

## Proposed next work, not execution approval

N6 desktop design candidates include surface composition and ownership/focus,
window interaction, a native Rust **Terminal** using the existing shell,
and eventually a dock and Settings/Accessibility (including cursor-size options).
These are incomplete planning topics; their order, contracts and budgets require
owner approval. The accepted mockup is a design reference, not implemented UI.

Rust-first kernel migration beyond the merged R1 helper and broader
physical-hardware/architecture portability remain proposals. They require separate compatibility, unsafe/FFI/DMA,
boot/interrupt/device and resource-management plans with reproducible checks.
VOS targets real x86_64 computers; QEMU success does not establish firmware,
GPU, USB, storage or input support on physical systems.

## Historical milestone publication notes

The following N3-N5 publication narrative preserves earlier checkpoints.
Its local-branch statements, original allocation ceilings and descriptions of
individual prototypes are historical, not the current baseline or storage
authorization. Current launch/GUI/pointer behavior is summarized above.
Original evidence, failures, monitoring gaps and result counts remain unchanged.

**N4 is complete, accepted and integrated into main** at
`bbca69d850e8c168bfa824ace475a27226b2cdb7`. Its tree matches accepted publication
`50cbc8bc49bd10d5272647b267ed23895d2f533d`, including Rixie Core v0.
[N4 records](results/n4-closure.md) preserve the pre-merge readiness and evidence.

**N5 networking is complete and formally accepted within its original and
subsequently authorized bounded prototype scope** (owner decision, 2026-10-25).
[PR #7](https://github.com/veasnawt/vos/pull/7) integrated closure `9f443746` and
feature integration `93053b81` at
[`39b40a904cce0778c307dd9154fff2531567d6d4`](https://github.com/veasnawt/vos/commit/39b40a904cce0778c307dd9154fff2531567d6d4).
Build smoke and secret history audit passed on the feature head. Acceptance
covers a bounded kernel prototype, not production networking or physical-hardware
support. Verification is QEMU-only. Production networking, DNS, DHCP, IPv6, TLS
and userspace networking remain deferred.

The following per-profile results describe the respective implemented increments. The
[first networking increment](results/n5-first-network.md) provides bounded modern
virtio-net initialization and raw frame I/O. Host checks and isolated BIOS/UEFI
frame exchanges pass. There are no application network grants or new syscalls.
The first 40-file increment is published at `27fe770676b4ce138eb9596c881a556c820cbe74`.
[Ethernet/ARP](results/n5-arp.md) now passes 4,598 sanitizer-backed host checks
and both firmware wire checks (4 TX/11 RX, 8 rejected frames each). It reuses
the unchanged driver and a four-entry expiring cache, published at `a6571c1`.
[Native IPv4/ICMP](results/n5-ipv4.md) now passes 1,806 sanitizer-backed checks
plus the 4,598-check ARP host regression. Each BIOS/UEFI boot completes two pings,
answers one request and rejects 11 malformed/unsupported/unmatched packets.
IPv4/ICMP is published at `33693b3`. [Bounded kernel UDP](results/n5-udp.md)
now passes 136,730 sanitizer-backed host checks, the existing IPv4/ARP host
regressions and BIOS/UEFI wire checks. Each boot sends four UDP datagrams,
receives seven, rejects 12 invalid/unsupported/unbound packets and safely
rejects one four-slot queue overflow. UDP is published at `a5889f9`.
[Passive TCP establishment](results/n5-tcp.md) now passes 3,808 sanitizer-backed
host checks and both firmware tests: one completed handshake, one half-open
timeout and 20 rejected inputs each. There is one fixed connection record,
with no streaming, retransmission, TCP options or FIN/RST closure. TCP changes
were published at `4e799c2`. [Bounded TCP payload exchange](results/n5-tcp-data.md)
now passes 5,397 sanitizer-backed checks and BIOS/UEFI wire tests. Each boot
accepts 144 bytes, consumes 80, transmits 68 and verifies partial/full ACKs,
window exhaustion/reopening and explicit timeout/reset erasure. Context size is
504 bytes with fixed 64-byte RX and TX buffers, below the existing 512-byte bound.
Payload exchange is published at `ff251a7`. [Bounded retransmission](results/n5-tcp-retry.md)
now passes 1,711 new sanitizer checks and BIOS/UEFI loss/ACK-loss/partial-ACK tests.
Three retries and a 200-tick absolute TX horizon bound terminal cleanup. The
context remains 504 bytes; retry metadata occupies six former padding bytes.
Retransmission is published at d512db7 with all 61 reviewed files.
[Bounded connection closure](results/n5-tcp-close.md) now passes 8,505 closure
ASan/UBSan checks, existing TCP regressions and BIOS/UEFI exchanges (23 TX / 26 RX
each), with independent packet verification and authentic captures. It supports
active/passive/simultaneous FIN, exact-sequence reset, FIN retries and timeout
cleanup in the same 504-byte context. Live active TIME-WAIT is checked and
explicitly aborted; full 2xMSL expiry is host-tested without shortened timers.
Closure is published and merged through PR #7. Application networking is not exposed.
Historical integration checkpoints before PR #7 (retained for context):
GitHub main then contained the owner's [PR #4](https://github.com/veasnawt/vos/pull/4)
merge `d01b3f5`, whose tree matches IPv4 publication `33693b3`. Local main remains
`bbca69d`; the executor did not merge, fetch or change that local branch.
The owner's later [PR #5](https://github.com/veasnawt/vos/pull/5) advanced GitHub
main to `ffaf2fb`, with a tree identical to TCP establishment `4e799c2`.
Rixie Connected is not started. The existing 272 MiB continuation meter is enforced
alongside the unchanged inherited 3360 MiB aggregate ceiling and host reserves.

N3 remains complete and accepted on 2026-10-24.
[Final N3 closure and evidence](results/n3-closure.md) consolidate D1a–D1f,
five mutation controls and fifteen larger-RAM stress runs.

**N3 publication:** `897312fb991b2f55799b37fbd4459fb552acfe82` on
`feat/n3.6-storage-closure`. GitHub main integrated it through PR #2 at
`d9a4ade0db1df8ab7c8c7fe3c4553cd3f36c3622` during N4 development. Local main
and `feat/n4-interactive-console` were fast-forwarded to that merge; its tree
is identical to N3. The first N4 keyboard/echo implementation is published on
`feat/n4-interactive-console`. No new release tag was created; the older U1 tag
remains `checkpoint-2026-10-19-u1`.

**The original N4 scope is complete.** The [first input increment](results/n4-first-input.md)
is published at `3a2b642`; [buffered input](results/n4-buffered-input.md) is
published at `87b99dfc867c45bd01f512cf9ecac0117d1a6eac`. The subsequent
[native Rust vsh](results/n4-native-vsh.md) runs in ring 3 with a granted ABI v3
console and supports all seven requested commands plus `exit`. File commands
use real read-only ext2/ABI v2 operations in an explicitly granted namespace.
Host checks and BIOS/UEFI runtime checks pass, including five ring-3 console
probes per firmware and the original normal boot-and-halt path. Native shell
publication is `1beb40677bfeb605da5b2920b70af0fc24138493`.
The subsequent [shared Rust SDK and independent app](results/n4-rust-foundations.md)
also pass host, loader and BIOS/UEFI checks and are published at
`f7ac43478ea548ddc4bb74ae177b0bb828e67596`. The requested
[normalized cwd prompt](results/n4-dynamic-prompt.md) is published at
`bdde1bc33f99030ba51fa78078a6db90f87f2a07`; three host tests and both firmware
interactions pass. [Rixie Core v0](results/n4-rixie-core.md) subsequently passes
six host tests and 17 interactions on each firmware, plus independent vsh smoke
checks. It is a deterministic offline Rust ring-3 application with a console-only
grant, launched through its own trusted boot configuration. No external-app shell
launcher or new kernel subsystem was added. Main now contains this accepted N4
implementation; the older publication statements below remain historical.

Historical N3.5 approval, integration, failed attempts and original authorization
boundaries below remain records of those checkpoints, not current N3 blockers.

VOS is an experimental, independently written x86_64 kernel (with a legacy i386 kernel). It
runs only in QEMU so far: no physical machine has booted it. This is a development checkpoint,
not a release.

## Completed milestones

The current table above also records accepted unified application launching and
N6.1-N6.3 components; it does not mark the entire N6 desktop complete.

N5 networking is formally accepted at `39b40a9`: bounded native Ethernet/ARP,
IPv4/ICMP, UDP and passive TCP establishment, payload, retransmission and closure.
Original result records retain their evidence and pre-publication wording.

N4 now delivers keyboard input, the buffered console, native Rust `vsh` with the
seven requested commands and dynamic cwd prompt, and reusable Rust application
foundations with an independent example. The authorized extension adds the first
minimal Rixie Core prototype. All are integrated into main at the verified N4 merge.

N1 (user mode, system calls), N2 (scheduling, preemption of user programs, IPC), N3.0–N3.4
(W^X higher-half memory, virtio-blk, block cache, read-only ext2, interrupt-driven block I/O),
N3.5 (capability-scoped read-only userspace file ABI v2), N3.6/N3 (integrated
storage stress and final closure), U0 (UEFI feasibility) and U1 (UEFI readiness
with BIOS compatibility).
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
- Additive ABI v3 console, [ABI v4 execution](abi/syscall-v4.md) and
  [ABI v5 GUI](abi/syscall-v5.md), with explicit process-owned authority;
  earlier ABI behavior remains supported.
- Kernel threads.
- Ring-3 programs in separate address spaces, preempted by the timer.
- IPC channels.
- System-call ABI v0/v1, plus integrated N3.5 ABI v2 file operations;
  [Stage 3 implementation](results/n3.5-stage3.md) and [Stage 4 security verification](results/n3.5-stage4.md).
  Stage 4 closure is approved. [Stage 5 final verification](results/n3.5-stage5.md)
  passes 74 BIOS and 38 fresh UEFI runs, 845/0 file assertions, 302/0 static checks
  and five mutation controls. Frozen hashes and resource budgets hold.
  N3.5 closure is approved. N3.6's accounting foundations, deterministic stress
  and final D1/D2/C20 verification are also complete; see the final N3 record below.

**Interrupts and timing:**
- 8259 PIC and PIT.
- Device IRQ lines with top and bottom halves, and a storm guard.
- Deadlines for blocking waits.

**Storage (read-only):**
- virtio-blk, interrupt-driven where a level-triggered INTx line exists, otherwise polling.
- Block cache.
- ext2 reader and capability-scoped read-only ABI v2 file calls in local main.
  Only explicit trusted launches receive non-root directory grants; normal
  hello still requires no disk. No writable operations or authenticated-media claim.

**Display:**
- Validated 32-bpp BGRX framebuffer, original 8x16 diagnostic font and panic banner.
- Accepted N6.1 scene: one system bar, bounded console viewport and no normal
  desktop PASS badge. This panel is not a native graphical Terminal application.
- N6.2: one <=256x160 process-owned surface, 40 backing frames, 4 KiB uploads
  and 16 queued events; framebuffer/device addresses remain supervisor-only.
- N6.3: kernel-owned 32x32 cursor canvas, 21x28 visible arrow and fixed 4 KiB
  underlay, independent of application pixels and the event ABI.
- VGA text only when text mode is positively known.

## N3 final accepted verification (2026-10-24)

| Check | Result |
| --- | --- |
| D1a/D1b/D1c | 74 BIOS, 19 primary UEFI and 19 additional UEFI passing verdicts |
| D1d/D1e | Static ELF/ABI checks; 139 frozen hashes, 131 old ELF comparisons and 55 ISO payload audits pass |
| D1f | 43 layouts pass; cumulative static/reserved growth 45864/57344 B stays within 65536/131072 B |
| D2 | rights-width, symlink, error-copy, incarnation and reserved-inode: 5/5 detected by intended assertions |
| C20 | 15/15 profiles: each Gate B/C/D on BIOS default 2 GiB and UEFI default/max at 1 and 8 GiB |
| Evidence | D1, D2 and C20 sealed; independent archive verification 566/566, 64/64 and 204/204 |
| Integrity | Seven protected references unchanged; prior evidence, failures, 13 worktrees and two stashes preserved |

D1's C2-T held-publication correction fixes an invalid test assumption about
un-notified requests; production and protected stress images remain unchanged.
D1r3 is still unfinished, unsealed and unarchived. Monitoring interruptions and
failed controller/dependency attempts remain documented, not erased.
The [closure record](results/n3-closure.md) contains complete scope, hashes,
reproducibility constraints, storage accounting and authentic screenshots.

## N3.5 accepted verification checkpoint (historical)

The user approved closure on the evidence committed at `0109d9b`; the formal closure
changes documentation only. Historical stage reports and failures remain unchanged.

| Accepted check | Stage 5 result |
|---|---|
| Full BIOS | 74 passing QEMU verdicts |
| Full fresh UEFI plus additional configurations | 19 + 19 = 38 passing runs, fresh VARS; reader/file default and CPU max at 128 MiB, 1 GiB and sparse 8 GiB |
| File / static / mutation checks | 845/0 file assertions per full file run; 302/0 static; all five deliberate mutation controls detected |
| Frozen compatibility | 139 hashes and 131 old userspace binaries unchanged; ABI v0/v1 behavior preserved |
| Reproducibility | Paired production binaries identical; ISO payloads verified separately from timestamps/identifiers |
| Cumulative footprint vs `c5f5acb` | +43808 B static of 65536 B; +57344 B reserved image of 131072 B |

See the [closure record](results/n3.5-closure.md) for the evidence review and accepted
limitations, and [N3.6 preparation](plans/n3-storage.md#11-n36-storage-closure-complete)
for the historical design questions. Final N3/N3.6 acceptance is recorded above.

The subsequent integration audit retained the completed BIOS/UEFI and static
results, completed the three unfinished mutation controls after sparse-copy
harness recovery, and rechecked hashes, production reproducibility and resource
limits. It did not rerun the full regression matrix after the harness repair.
The repair changes test orchestration only; production and ABI sources remain
unchanged from the accepted implementation.

## U1 verification (historical 2026-10-19 checkpoint)

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

- **Hardware:** QEMU only. Untested on any physical machine.
- **Directory progress:** the 512-unit work limit can reject deep single-block
  chains and scans before block 344 in the sparse `/scope/huge` fixture; EAGAIN
  carries no progress cookie. Gate D records exact boundaries; resumable progress
  remains future work.
- **Kernel stack:** full dynamic kernel-stack high-water proof is incomplete.
  Compiler frame reports and runtime guard/canary tests do not constitute that proof.
- **Filesystem trust:** read-only; trusted immutable provisioned directory graphs.
  Namespace/parser validation does not authenticate malicious media or inode/block aliases.
- **Storage lifetime/timing:** finite work and poll bounds do not promise real-time latency
  or fairness. Unconfirmed device reset may retain bounded DMA quarantine; physical
  reset-refusal/hot-unplug reclamation is untested.
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
- **Input:** bounded polling through one i8042 keyboard/auxiliary owner;
  US keyboard layout, 128-byte console line limit and standard three-byte PS/2
  relative mouse/primary button. No USB input, wheel, extra buttons or hotplug.
  Editing appends/deletes at the end; an overlong submission is discarded in full.
  Boot configuration selects the diagnostic, shell or GUI/pointer session;
  default production hello boot still halts.
- **Shell:** native Rust vsh supports trusted catalog launches of hello and Rixie;
  no quoting, pipes, history, arbitrary executable paths or background jobs.
- **Missing features:** writable storage, production/userspace networking, SMP and ACPI power management.

## Unsupported (by design, for now)

- Physical-disk installation.
- `grub-install` / `efibootmgr` use.
- Writes of any kind to storage.
- Secure Boot.
- Hardware other than QEMU's emulated PC.


Historical resume checkpoint before PR #7 (not current branch status):
The owner merged PR #6 at 369153dc5bd9a891dd1d805791ef4ea1805f54bf,
with the same source tree as published retransmission d512db7. The feature is now
zero ahead/three behind main. Local main/origin-main stay bbca69d; closure stays
uncommitted. The original final audit passed before interruption. Its historical
remote pin is preserved; the independently checked resume record links the new
owner merge, unchanged runtime evidence and final accounting.
