# R2-X: formal closure of the R2 selected-input migration

**Verdict: the R2 exit criteria are satisfied for the selected-input scope defined below, with the evidence limitations and owner-approved deferrals stated here. Closure becomes formal when the owner merges this documentation-only record.** This review is read-only. It changes no source, re-runs no test, and claims neither physical-hardware acceptance nor removal of all C code.

Baseline: accepted main `06f206d8ef6f4833121b98c6c87e7a73a341ae1f` (owner-merged PR #62). Its post-merge Build smoke, QEMU runtime smoke and Gitleaks all passed (runs 42349553606 and 42349553620).

## Master Plan contract

[Master Plan §6](../plans/MASTER-PLAN.md#6-r-series-rust-first-kernel-migration) defines R2 as "selected input/drivers with understood device ownership". Its required gate is "hardware-facing error, IRQ and DMA contract verification". The owner selected the scope: the PS/2 keyboard/pointer driver and the console line input path.

| Criterion | Result | Evidence |
| --- | --- | --- |
| Selected input migrated | **PASS** | Pointer and keyboard decoding, console line extraction, editing and the blocking reader, routing, the queue, pointer polling, ordinary polling, controller reads and writes, and the controller lifecycle are Rust-owned (increments below) |
| Device ownership understood | **PASS** | One controller owner (the `user_console.c` keyboard instance, or `input_console.c` at boot). The driver logic is Rust behind a single FFI boundary. Port instructions stay in C callbacks (see the map). |
| Hardware error contract | **PASS** | Each increment ran a frozen C oracle differential with exact traces. R2-L injects every response byte value, channel flip, ERR, parity error, NAK, dropped response, stall, 99,999/100,000-poll write waits and 31/32/33-byte drains |
| IRQ contract | **PASS, with a stated limitation** | The driver is polling-only: init clears the controller's IRQ1/IRQ12 enables (mode bits 0 and 1; tested for all 256 modes). The Rust write path can address only 0x60/0x64 (typed `Port`), so it cannot reach PIC ports. There are no IRQ or PIC references in the driver. **Limitation:** no R2 test reads the PIC masks back at runtime; the boot line "IRQ masks unchanged" is a message, not a measurement. |
| DMA contract | **N/A, verified absent** | The PS/2 and console paths use no DMA; the modules contain no DMA code |
| ABI and capabilities preserved | **PASS** | The public `keyboard.h` API and the 88-byte `struct keyboard` are unchanged (compile-time asserts in Rust and every differential). No new controller command (R2-L's emulator fails on any unaccepted command). No output-port, A20, reset or PIC operation. |
| Verification gates per increment | **PASS** | Frozen oracle, sanitizer and checked-Rust differentials, named mutants, frame and image budgets, reproducible builds, BIOS/UEFI QEMU runtime and GUI checks, and required CI before every merge (one historical exception, below) |

## Accepted increments

| Increment | PR | Reviewed head | Merge |
| --- | --- | --- | --- |
| [R2-B pointer decoder](r2b-ps2-pointer-rust.md) | #26 | `4352815` | `d8edfce` |
| [Keyboard decoder](r2-keyboard-decode-rust.md) | #52 | `a7e8193` | `9ab5e9f` |
| [Console line extraction](r2-console-line-rust.md) | #53 | `68d5cd8` | `fbf11d9` |
| [Console editor](r2-console-accept-rust.md) | #54 | `664f7bb` | `9134d1c` |
| [Blocking console reader](r2-console-reader-rust.md) | #55 | `0186e0c` | `f6e9603` |
| [Shared PS/2 routing](r2-ps2-routed-drain-rust.md) | #56 | `95deb3c` | `e01c51e` |
| [Pointer polling](r2-ps2-pointer-poll-rust.md) | #57 | `920b667` | `7151c51` |
| [Keyboard queue](r2-keyboard-queue-rust.md) | #58 | `6d70ace` | `63a318d` |
| [Ordinary `keyboard_poll`](r2-keyboard-ordinary-rust.md) | #59 | `a64222b` | `85f83e9` |
| [Controller reads](r2-ps2-controller-read-rust.md) | #60 | `5c3fe8d` | `74abed0` |
| [Controller writes (R2-W)](r2-ps2-controller-write-rust.md) | #61 | `6687f6b` | `ed3b496` |
| [Controller lifecycle (R2-L)](r2-ps2-lifecycle-rust.md) | #62 | `9c5c14c` | `06f206d` |

Several per-increment reports still describe their pre-merge state ("pending CI", "not merged"). The status lines in [STATUS](../STATUS.md), the [ROADMAP](../ROADMAP.md) and this table are authoritative.

## Final ownership and ABI map

| Layer | Owner | Notes |
| --- | --- | --- |
| Decoding, routing, the queue, pointer and ordinary polling, the bounded controller reads and writes, init, suspend/resume, pointer start/stop | **Rust**: safe cores in `kernel/rust/input/` (`#![forbid(unsafe_code)]`), the FFI boundary in `arch/x86_64/rust/ps2_routed.rs` | 15 private `vos_rust_keyboard_*` entries in `rust_kernel.h`, each with one Rust owner |
| Public keyboard API (`keyboard_init`, `keyboard_poll`, suspend/resume, pointer start/stop/poll/lost) | **C, ABI forwarding only** (`arch/x86_64/keyboard.c`) | Every function forwards (tail jump) to Rust. `keyboard_poll` keeps the one-field mode dispatch (`pointer_ready`). No port access, port macro, wait or drain loop. |
| Port instructions | **C callbacks** (`inb`/`outb` in `cpu.h`, passed as `struct keyboard_io`) | An intentional interoperability layer: Rust performs controller I/O only through these callbacks and reloads the table on every access |
| Boot and I/O adapter | **C**: `arch/x86_64/input_console.c` | Documented C boundary: the `vos.console=1` option, the `inb`/`outb` callback table, and the boot console loop. Its line extraction, editing and reading are Rust-owned. |
| Foreground owner and exec lease | **C**: `arch/x86_64/user_console.c` | **Deferred to R4** (below); not migrated |
| USB survey status read | **C**: `xhci_survey.c` `xhci_survey_i8042_status()` | **Excluded** (below); not part of the PS/2 driver |

## Controller hardware-authority boundary

Rust may write only port 0x60 or 0x64; `Port` has no other value, and the C-facing write entry refuses any other port without access. The command set is exactly that of the accepted C:
- **Initialization:** AD, A7, 0x20, 0x60 plus the mode byte, AE, F5, F0, 02, F4
- **Suspend/resume:** F5, F4
- **Pointer lease:** AD, A8, D4 plus FF/F6/E6/F4/F5, A7, AE

Every wait is bounded: 100,000 status polls per read or write, and 32-byte drain and ACK-search limits. The driver never issues output-port, A20, reset or PIC commands.

## FFI and unsafe boundary (`arch/x86_64/rust/ps2_routed.rs`)

All unsafe code in the PS/2 path is in this file. It contains 42 `unsafe` blocks, documented by 13 grouped `SAFETY` comments plus per-entry `# Safety` contracts. The contract:
- **Object validity:** each entry refuses a null, misaligned or wrapping object before any access.
- **Callback table:** the `io` table is reloaded for every callback. No Rust reference spans a callback, and fields are read and written in place.
- **Initialization:** init zeroes all 88 bytes and validates `io`, `in` and `out` before any access, reading the `in` slot as raw bits.
- **Output:** the output byte is written only on success.
- **Writes:** a null `out` is refused, not called.

Remaining audit considerations (non-blocking):
- `SAFETY` coverage is per group, not per block.
- Entries after init rely on the existing contract that a callback never substitutes an `io` table with a null `in`; the accepted C had the same assumption.
- `rust_kernel.h` still declares `vos_rust_keyboard_routed_drain`, which only Rust now calls.
- Code size depends on `#[inline(never)]` on the helper entries.

## Performance decisions

Each decision is bound to its reviewed head and recorded in the linked report:
- **#52 (`a7e8193`):** decoder 1.529× in a hosted micro-benchmark (+2.07 ns per call). Disclosed and merged; it remains an optimization candidate needing separate source authorization.
- **#53 (`68d5cd8`), #54 (`664f7bb`), #55 (`0186e0c`):** remaining hosted regressions approved by the owner on 2 November 2026.
- **#56 (`95deb3c`):** full queue +2.47%; exact-source measurements approved.
- **#57 (`920b667`):** hosted pointer-poll regressions approved on 3 November 2026.
- **#58 (`6d70ace`):** four source-bound hosted regressions approved.
- **#59 (`a64222b`):** unready +14.7% (+1.15 ns) and empty +6.0% (+0.68 ns), approved.
- **#60 (`5c3fe8d`):** `delay10` +5.3% (+5.65 ns), approved; the 100,000-poll timeout +4.5% is recorded.
- **#61 (`6687f6b`):** `write_timeout100k` +13.4% (r2; +6.4% in r1 on byte-identical code), approved.
- **#62 (`9c5c14c`):** none above 5%.

All figures are hosted measurements, not guest or physical-hardware performance.

## CI history

Every R2 PR passed its checks at its exact head before merge (rechecked for this review). Freestanding builds, QEMU runtime smoke and Gitleaks passed for #52–#62. PR #26 predates the QEMU smoke job and passed Freestanding builds and Gitleaks. **PR #59's post-merge Build smoke run 42304789235 remains INCOMPLETE:** its build job was cancelled at the 25-minute job timeout in both attempts while package downloads were slow. That record is not relabelled. The same code later passed full post-merge CI on `74abed0`, `ed3b496` and `06f206d`, after PR #60's separate reliability change.

## Verification limitations

- All runtime evidence is QEMU (BIOS/SeaBIOS and UEFI/OVMF, 128 MiB and 2 GiB, real QMP key events, genuine framebuffer captures). **No physical-hardware acceptance is claimed.**
- Benchmarks are hosted micro-benchmarks.
- There is no runtime PIC-mask readback (see the IRQ criterion).
- The R2-W and R2-L GUI runtime cases verify keyboard delivery. Real PS/2 pointer movement was verified at runtime by R2-B's GUI regression (PR #26), on the controller code that was C at the time. This review did not confirm a runtime pointer-start case on the Rust controller code; there, the pointer-lease sequence is verified by the lifecycle differentials.

## Excluded: USB-survey status read

`xhci_survey_i8042_status()` in `arch/x86_64/xhci_survey.c` executes `inb(0x64)` and is called by the Rust USB survey (`xhci_survey.rs`). It runs only with the opt-in `vos.usb-survey=1` boot option. It reads the side-effect-free status register and never the data port. **By owner decision it is an explicitly excluded C-owned diagnostic read, not part of the PS/2 driver migration.** It may be reconsidered during future USB migration. It was not changed to claim complete Rust ownership of every PS/2 port access.

## Deferred to R4: console foreground and exec-lease policy

By owner decision, `user_console.c` stays C and is not counted as migrated. Its behavior is unchanged by R2-W and R2-L (no changes since PR #58's baseline). R4 must preserve these invariants:

1. **One foreground authority:** at most one text owner, one GUI owner or one exec lease at a time. Ownership is checked by process pointer **and** PID, liveness and kill status, and only a ready process with the right ABI can be granted it (text ABI 3/4, GUI ABI 5).
2. **Authority generations:** every ownership change advances `epoch`, which panics at its maximum instead of wrapping.
3. **No input crosses owners:** every handoff zeroes the pending line and passes through the keyboard suspend boundary (bounded drain). Destroying a GUI surface is also an input-epoch boundary.
4. **Exec lease:** runs only in the kernel address space with interrupts disabled (otherwise panic). Restoring the parent requires the parent to be unrevoked, at the same generation, with the same PID and thread, alive and running. Revoking the lease cancels the parent's exec. A reader surviving child reclamation panics.
5. **Reads:** one reader at a time. Reads are cancellable on owner change, kill or revocation. The trap frame is re-decoded and every output page re-validated after any wait. The copy is all-or-nothing, and the pending line is retained on error.
6. **Writes:** bounded to 256 bytes, copied from user space under validation, non-printable characters masked; diagnostics stay prefixed.
7. **Separation:** GUI authority is separate from the text grant and lease. Pointer leasing requires the GUI owner.

## Recommendations for later cleanup (not part of R2)

- Add a runtime PIC-mask readback to the input-console QEMU case.
- Add one `SAFETY` comment per `unsafe` block in `ps2_routed.rs`, and drop the stale `routed_drain` declaration.
- Update the pre-merge status wording in the per-increment reports to link here.
- Optionally optimize the PR #52 decoder under separate source authorization.
- Reconsider the USB-survey status read within the USB track, and the console policy within R4.
