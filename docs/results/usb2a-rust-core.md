# USB.2-A: portable Rust xHCI controller core

Status: locally verified software model, for owner review. **No live controller
adapter, MMIO, DMA allocation, PCI changes, reset, interrupt registration or USB
device operation was implemented or executed.** A simulated completion is not
an xHCI hardware success.

Base: design commit `2cd3bf3e7b7132fdfe6f5b643919916f39b465b3`, directly above
accepted main `95eb0e0c944277a4ed9f3ad0e8671e25d2d9f6cd` (merged USB.1 PR #22).
The [complete USB.2 design](../plans/usb2-rust-xhci.md) is preserved unchanged.
Work uses the separate `vos-usb2a` checkout and
`feat/usb2a-rust-controller-core`; original design, USB.1 and D1 checkouts remain
unchanged. No push, PR, merge or USB.2-B/C implementation is part of this work.

## Implementation and boundaries

| Source | Actual responsibility |
| --- | --- |
| [lib.rs](../../kernel/rust/usb/xhci/lib.rs), [mod.rs](../../kernel/rust/usb/xhci/mod.rs) | `no_std` crate, shared errors/constants, `forbid(unsafe_code)`; no allocator or platform dependency |
| [capabilities.rs](../../kernel/rust/usb/xhci/capabilities.rs) | Parse copied bytes and platform facts with checked ranges; no address dereference |
| [ring.rs](../../kernel/rust/usb/xhci/ring.rs) | TRB values, checked page/index arithmetic, command credit/identity, independent command/event cycles and typed decoded completions |
| [resources.rs](../../kernel/rust/usb/xhci/resources.rs) | Non-Copy logical lease records and transactional bounded resource arithmetic; no physical allocation |
| [controller.rs](../../kernel/rust/usb/xhci/controller.rs) | Cooperative lifecycle, explicit errors/deadlines, symbolic backend actions, cancellation, cleanup, retention and backend resource-namespace checks |
| [Host tests](../../tests/rust/xhci.rs), [fake backend](../../tests/rust/xhci_fake.rs) | Deterministic ownership/operation traces, fixed-capacity software rings and fault injection; same portable core, not a C reimplementation |
| [Freestanding fixture](../../tests/rust/xhci_freestanding.rs) | Force the generic lifecycle methods through bare-metal code generation; compile only, never linked or executed in VOS |
| [Host benchmark](../../tests/bench/xhci-core.rs), [test script](../../scripts/test-xhci-core.py) | Separate optimized logical-ring timing, strict tests, bare-metal compilation and isolated mutation control |

No existing kernel, application, ABI, build or CI file changed. The portable crate
is deliberately **not linked into the production kernel**. There is no boot flag,
C export, worker registration or live implementation of `Backend`. It can later
be included in the existing Rust kernel crate without a second runtime; doing so
requires a separate integration review.

The backend contract is synchronous and nonblocking per call. It supplies owned
logical leases, symbolic actions and observations; its stable nonzero resource
namespace prevents an active controller from accidentally using a different
backend. It is a trusted kernel implementation contract, not an application,
plugin, executor-attestation or hostile-process security interface. A future
adapter must actually establish resource ownership and hardware completion;
copied facts and a backend's boolean observations cannot establish those facts
about real hardware by themselves.

## Validation, rings and lifecycle invariants

Capability parsing follows the initial design profile: bus 0, one completely
scanned candidate, `1b36:000d`, xHCI 1.0, qualified 16 KiB BAR, memory decoding/D0,
identity/coherent DMA facts, 4 KiB page support, <=16 ports and no scratchpads.
BAR width and AC64 are distinct. Arithmetic/alignment, overlapping register
regions, capability bodies and forward links are checked. Traversal is limited
to 64 headers. Required USB Supported Protocol records have a recognized name,
supported revision, bounded speed-table extent and disjoint valid port ranges.
Unknown capability bodies are skipped without interpretation; this is not full
device/protocol descriptor validation or a physical-platform qualifier.

Hardware field references are Intel
[xHCI 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf),
sections 5.3, 7.1/7.2, 4.9.3/4.9.4 and 6.4.2/6.4.3. No copied implementation or
third-party dependency was added.

The command ring has 256 TRB positions: 255 command positions plus a Link TRB
with Toggle Cycle. It admits **one outstanding command**; a second request fails
without changing the cursor or pending identity. The event ring has 256 positions
and no Link TRB. Producer and consumer cycles wrap independently. Regions require
one aligned page, representable end, address-limit compliance and no 64 KiB
boundary crossing. An unavailable event cycle is ignored before decoding.

A completion must be decoded by the event parser, match the current software
request handle/epoch and pending TRB address, and have the expected No-Op type,
success status and reserved/slot fields. Rejected completions do not retire the
pending command. Software serials distinguish reused addresses; **the hardware
wire format has no corresponding serial nonce**. These tests reject stale
software handles/epochs and wrong/duplicate events; they cannot authenticate a
hostile device replaying the same wire pointer after reuse.

The lifecycle progresses through Uninitialized, Qualified, Ownership, Halting,
Resetting, Starting, Ready, Polling, Stopping, Joining and terminal
Stopped/Failed/Quarantined states. Ownership/reset/preparation are symbolic actions
and observations only. Control storage is acquired before the modeled worker.
Every allocation failure is unwound; an uncreated worker does not prevent release
of its argument storage. Before modeled address publication, acquired leases can
be released safely; after publication they require the quiescence observation.

Cancellation follows the same stop path. Missing quiescence retains DMA/mapping
records; missing worker reclamation retains control/worker records. A release
error preserves ownership and its error. A completion flag is not worker reaping.
Repeated shutdown is idempotent, a quarantined controller cannot restart, and
terminal controllers issue no new backend effects. A service step observes one
condition or examines at most 32 events. Deadlines expire at the exact boundary,
use wrap-safe elapsed arithmetic and reject clock regression. The caller must
advance time and wait/yield; no physical timing guarantee or scheduler liveness
is inferred from the model.

## Verification and preserved stops

Final [verification record](usb2a/verification.json) and
[raw Rust test output](usb2a/rust-tests.txt): **46 passed, zero failed/ignored**.
Coverage includes:

- 16,384 truncated BAR lengths; 12,000 deterministic capability mutations;
  405 generated budget vectors; arithmetic, indices and malformed metadata.
- Two independent 260-round-trip tests, command/event wrap and Link semantics,
  stale handle after pointer reuse, wrong epoch/pointer/type/status and duplicates.
- Full 256-entry fake event queue, rejected overflow, single command credit and
  exactly bounded event consumption.
- Delayed/dropped completion, exact timeout boundary, all initialization
  observation timeouts and backend errors.
- Failure at each of seven logical acquisitions and each initialization action;
  partial rollback, cancellation in active states, failed stop/release/join,
  repeated shutdown, no premature control release and no post-terminal effects.
- 32 successful initialization/teardown cycles with fresh epochs and zero
  retained leases; rejection of a foreign backend namespace.

The [mutation control](usb2a/mutation-control.txt) disables only software-handle
equality in a temporary copy. Its targeted stale-handle test **fails as expected
with exit 101**, detecting incorrect retirement. The original sources are not
mutated. This expected negative control is separate from the 46 passing tests.

Two preparation failures remain recorded in [history](usb2a/history.json): an
unused import rejected by `-Dwarnings`, and the harness's `mutant.rlib` filename
rejected because an extern rlib must use `lib*.rlib`. The import and output names
were corrected without changing warnings or test expectations. Both failed roots
and earlier passing checkpoints remain retained; the final run uses a fresh root.

## Actual memory, code and host cost

Measured on the pinned x86_64 toolchain, also enforced by compile-time bounds:

| Type | Bytes |
| --- | ---: |
| Controller | 424 |
| PermanentSnapshot (diagnostic data, not a live quarantine owner) | 144 |
| Capabilities | 40 |
| CommandRing | 64 |
| EventRing | 24 |
| Usage | 8 |
| TRB | 16 |
| Fake, including host ring/trace fixtures | 30,944 |

The core has **zero explicit unsafe blocks**, forbids unsafe code, uses no heap,
and creates no permanent buffers/global state. The test harness uses normal host
`std`; its fixture arrays are not kernel state or physical DMA allocations.
`Controller` and `Lease` are non-Copy and explicitly non-Send/non-Sync through
private zero-size `PhantomData` markers. The raw-pointer type in that marker is
not a pointer value, address or dereference. Both
[Send](usb2a/send-negative.txt) and [Sync](usb2a/sync-negative.txt) compile-negative
controls reject attempts to transfer/share these ownership types (exit 1), without
relaxing compiler warnings. Plain diagnostic snapshots are not ownership grants.
The 424-byte controller fits the design's 2 KiB object bound. A 144-byte retention
snapshot fits the 512-byte bookkeeping bound, but no production global owner has
been installed. Dropping a model must not be interpreted as hardware cleanup:
the caller drives shutdown to a terminal state and audits retained records.

Tests model four DMA pages (16 KiB), one CPU control page (4 KiB), at most two
mapping PT pages (8 KiB), one mapping and one existing worker slot. The admitted
physical high-water is **28 KiB**; excess and subtraction underflow are rejected.
Confirmed stop/reaping returns the model to zero. Unconfirmed quiescence retains
24 KiB after successful joining. If joining itself is unconfirmed, its control
storage and worker must remain retained too, within the original 28 KiB bound.
**No corresponding physical allocations or native threads occurred.**

[Object audit](usb2a/object-review.json): both `no_std` archives compile with the
existing higher-half/static/no-red-zone/panic-abort settings. The companion
compile-only fixture instantiates generic controller methods; a generic rlib
metadata build alone would not prove that code generation. GNU `size` reports
3,264 text-class bytes for the core object and 9,164 for the concrete fake fixture,
with zero data/BSS. No FP/SIMD, allocator, OS, TLS or unwind findings. Expected
unresolved memory intrinsics, Rust core bounds-panic support and companion-crate
symbols still require final linkage later; this is not a linked driver-size or
stack proof. The large fixture must not become a kernel worker stack object.

[Optimized host samples](usb2a/host-benchmark.json) measure 200,000 logical
prepare/decode/complete/advance round trips per sample with `black_box`: totals
3,740,300; 3,759,200; 3,382,300; 1,597,301; 1,303,800 ns (median **16.9115 ns per
logical round trip**). The earlier run's median was 6.5345 ns. The
[comparison](usb2a/performance-comparison.json) verifies byte-identical benchmark
executables across the zero-size marker change: these variable host timings
provide no evidence of an executable-code regression. No faster rerun was selected
to hide the spread. This is WSL host microbenchmark timing, separate from
checked test builds and C sanitizer tests. It measures no controller latency,
USB throughput, idle hardware CPU use or physical performance. There was no
previous equivalent operation to compare and no language-speed claim is made.

## Kernel compatibility, reproduction and preservation

[Compatibility audit](usb2a/compatibility.json) verifies 311 architecture,
userspace, ABI and build/checker files unchanged from accepted main. Fresh
`make ... ARCH=x86_64 verify` passes the unchanged Multiboot2 and strict five-load,
page-tiled/W^X, below-4-MiB layout checks. All five loaded segments match the
accepted USB.1 integrated native-verification image byte for byte; only nonloaded
debug-path metadata explains a different full ELF digest. The production Rust
archive remains exactly
`abb3773f1b21e291a2eb1943f9a5a7e697f151f1ca79bcf2113ae9bfbfeb692b`.
**Production kernel code/data growth from USB.2-A is zero.**

Fresh compatibility checks: [R0 ABI](usb2a/r0-host.txt), 4,132 pairs plus two fixed
vectors PASS; [R1 bounds](usb2a/r1-host.txt), 497,127 cases PASS; existing
[USB.1 host checks](usb2a/usb1-host.json), 45,713 xHCI and 49,499 console assertions
plus keyboard/editor/diagnostics PASS. C sanitizer instrumentation does not
instrument Rust machine code. No new BIOS/UEFI run is claimed: the accepted
[USB.1/R0/R1 native evidence](usb1-rust-integration.md) is reused by exact source,
archive and loaded-byte identity. No hardware activation was needed for this
portable-only task. CI was not run remotely; publication was not authorized.

```sh
cd /home/veasnawt/src/vos-usb2a
python3 -I -B scripts/test-xhci-core.py \
  --rustc "$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc" \
  --out build/usb2a-reproduction
```

Use a fresh output root and the existing compiler with `x86_64-unknown-none`;
no packages, device permissions or firmware changes are required. Exact compiler
commands, input hashes and flags are in the verification record. The script
never starts QEMU or registers a live backend.

[Preservation](usb2a/preservation.json), [resources](usb2a/resources.json),
[exact changed files](usb2a/changed-files.txt) and [SHA-256 manifest](usb2a/manifest.json)
record finalization. Historical evidence, unfinished D1 work/symlink, original
USB/design checkouts, 13 original worktrees and two stashes are preserved.
No legacy controller, funding or accounting mutation was used. Ordinary disk
checks admitted this separate checkout; no cleanup or reservation retirement ran.

## Remaining hardware prerequisites and next scope

USB.2-A is a protocol/resource **model**, not a hardware-safe adapter by itself.
The next separately authorized USB.2-B scope should implement and test the narrow
scoped-MMIO lease and PMM/platform ownership contracts, including rollback of
partial table allocation, cache/address qualification, kernel-CR3 release and
real argument-storage/worker lifetimes. No BME/reset/command activation is needed
to test those resource primitives. Preserve R0/R1 symbols and the existing ABI.

Later USB.2-C must turn symbolic actions into reviewed, bounded register sequences
and barriers, prove actual handoff/quiescence/reaping, install the bounded global
quarantine owner, link/audit the real backend and run separately authorized
BIOS/UEFI hardware-operation checks. It must not simply trust the fake's boolean
proofs or diagnostic snapshots. Physical BAR resources, coherent/identity DMA,
IOMMU assumptions and precise clock behavior remain unqualified on real PCs.
No USB.3 enumeration/HID, USB keyboard/mouse, application authority or physical-
hardware functionality is claimed or begun.
