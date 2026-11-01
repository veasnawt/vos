# H1-B: Rust USB boot-HID core

Locally verified candidate, 2026-11-01. Base main `77045d829f60475b7110e05816a82e75eea62da2`
(B1 PR #50). No merge or native USB-input claim. [API and supported subset](../usb-boot-hid.md).

## Implementation

Three safe, allocation-free Rust library files implement strict descriptor selection, keyboard/mouse
boot decoding and generation/owner-bound delivery. Ambiguous interfaces/endpoints are refused.
Reports require explicit transport-confirmed boot protocol; the library never performs that transfer.
Keyboard transitions are transactional. A non-droppable reset barrier handles lost/malformed input,
queue saturation, owner changes and disconnect; neutral rearming prevents stuck or inherited presses.

The library remains independent of the kernel build until a real transport/broker integration is
reviewed. No production C source, public C ABI, PS/2 behavior, controller operation or DMA budget changes.
Only test adapters bind it to actual existing VOS console/GUI code under hosted fixtures.

## Focused verification

| Gate | Evidence |
| --- | --- |
| Rust core, 15 tests | [output](h1b-rust-boot-hid/core-tests-bulk.txt): valid/composite descriptors, truncation, hostile lengths, speed/count/endpoint rules, ambiguity, protocol confirmation failures/staleness, key/modifier transitions, rollover, repeated reports, ownership epochs, disconnect and queue saturation |
| Independent bounded input cases | 50,000 descriptor mutations without panic; 10,000 oracle-checked keyboard transitions; all 65,536 signed mouse X/Y byte pairs |
| Actual console integration | [output](h1b-rust-boot-hid/hid-console.txt): foreground parent/child handoff, stale-owner rejection, epoch flushing, line delivery and disconnect; includes unchanged PS/2 console handoff regression |
| Actual GUI integration | [output](h1b-rust-boot-hid/hid-gui.txt): grants, direction, primary buttons, queue overflow and snapshot recovery; fixtures do not operate USB |
| Unchanged PS/2 regression | [output](h1b-rust-boot-hid/ps2-regression.txt): 987,802 assertions across production PS/2 decoding/demux/GUI behavior, modeled hardware/scheduler |
| Negative controls | [eight mutants](h1b-rust-boot-hid/mutations.json): missing protocol gate, owner check, repeat suppression, Y direction, overflow reset, total length, interface count and duplicate binding; each compiled and failed its intended assertion |
| Freestanding resources | [result](h1b-rust-boot-hid/resources-bulk.json): x86_64-unknown-none, opt-level 2, no red zone, forced frame pointers; LLVM .stack_sizes for every emitted executable section, plus concrete API wrappers for otherwise inlined methods |

The final parser review tightened interface numbers to the declared count, enforced full/high-speed
bulk packet sizes on unbound interfaces, and added the exhaustive mouse oracle plus bulk regression. Core/resource checks were rerun. The completed adapter and mutation evidence above
predates those range/packet-size tightenings and formatting-only changes; it is retained with original source hashes.
The adapter fixtures' valid interfaces (0/1) and decoded-event logic are unchanged. The public CI gate
runs the complete suite on the PR tree, rather than depending on those local reuse conclusions.

## Measured resources

- Session **264 B**, Event **6 B**, 32 events/session. Two sessions + 4096-byte descriptor buffer: **4624 B**.
- Maximum emitted frame **344 B** (`Session::report`); parser 128 B; constructor wrapper 232 B.
  All measured frames satisfy 512 B. This is per-frame compiler metadata, not whole-chain dynamic
  high-water proof or a cross-compiler guarantee.
- Core object: text **6729 B**, rodata **592 B**, no writable static state. API-wrapper overhead is
  measurement-only. Production kernel/image growth is zero because this library is not linked there.
- Undefined core/API dependencies are only `memcpy`/`memset`: no allocator, panic machinery or device calls.
- Heap allocations **0**, DMA pages **0**, controller/device operations **0**.

## Failures and limits

Preserved failures include an invalid test expecting an unchanged report-length high byte to be
rejected, a resource checker incorrectly expecting ten out-of-line functions when LLVM emitted only
three, GUI fixtures that initially omitted reset-overflow consumption and kernel-CR3 setup, and a
mutant rejected by unused-variable lint rather than its intended assertion. Corrected runs have
separate outputs; none of those records was relabeled PASS. Resource coverage now accounts for inlined
APIs explicitly. Bounds fallbacks also eliminate panic dependencies from the freestanding object.

All work stayed under the unchanged 160/175 MB storage lines, 21 GiB C: floor and 1.25x projections,
with supplementary physical E:/shared-Git/controller accounting. No cleanup, recovery-asset changes,
controller writes, USB requests or physical tests were performed. Historical B1 evidence stays intact.

ASan/UBSan cover C fixtures, not Rust machine code. No screenshot is offered: replay output is not
native USB input. General report descriptors, typematic/layout expansion, LEDs, wheels, alternate
settings and broader topology remain unsupported. Real transport success, firmware ownership, DMA
ordering/quiescence, hub/TT operation and the Dell EHCI path remain H2 prerequisites under separate
approval. See [policies](../../AGENTS.md) and the [publication checklist](../milestone-publication-checklist.md).
