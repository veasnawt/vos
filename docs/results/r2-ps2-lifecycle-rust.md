# R2-L: Rust PS/2 controller lifecycle (init / suspend / resume / pointer start / pointer stop)

**Correctness, lifecycle equivalence, resources, reproducibility, and the BIOS/UEFI runtime and GUI checks all PASS. No benchmark exceeds the 5% threshold. No merge without owner approval.** Baseline is accepted main `ed3b496e9160b9c88899d9572a9b86e235289a64` (owner-merged PR #61).

With this increment, every PS/2 controller access in VOS is Rust-owned. `arch/x86_64/keyboard.c` contains only the public C ABI functions, which forward to Rust, plus the `keyboard_poll` mode dispatch. It performs no `in`/`out` callback, defines no port macro, and has no wait or drain loop.

## Production scope and ABI

`keyboard_init()`, `keyboard_suspend()`, `keyboard_resume()`, `keyboard_pointer_start()` and `keyboard_pointer_stop()` keep their public C signatures. They tail-jump to `vos_rust_keyboard_init`, `_suspend`, `_resume`, `_pointer_start` and `_pointer_stop` respectively. `struct keyboard` (88 bytes), its field layout and every other entry point are unchanged.

- `kernel/rust/input/ps2_lifecycle.rs` is a safe core (`#![forbid(unsafe_code)]`) holding the five sequences. It issues commands only through the accepted bounded Rust helpers (`write_port`, `command`, `aux_command`, keyboard/aux reads), or a single status/data read for the drain loops. It contains no output-port, A20, reset, PIC or other new command.
- `arch/x86_64/rust/ps2_routed.rs`: a `Life` adapter calls the accepted helper entries out of line, so no copies are inlined. `pointer_start` reaches suspend/resume through their out-of-line entries, as the C called its public functions. State fields are written in place at the accepted points relative to the controller callbacks. The five helper entries gain `#[inline(never)]` and are otherwise unchanged.
- `keyboard_init` keeps the accepted io semantics. Like the C, it zeroes all 88 bytes (padding included) and then stores `io`. It validates `io`, `in` and `out` before any access, and runs its drain through the **caller's** io table, while its helpers use the object's table. Invalid object addresses are refused with no access; the C driver never produces one.

The accepted contract is preserved:
- **Init:** AD, A7, then drain at most 32 bytes, then require an empty output buffer. Read the mode with 0x20, apply `(mode | 0x60) & ~0x13`, and write it back with 0x60. Then AE, and F5, F0, 02, F4, each acknowledged. `ready` is set only at the end.
- **Suspend:** no-op success when not ready. `ready` is cleared before the F5 write. The ACK search takes at most 32 reads and the drain at most 32; the decoder is cleared only on success.
- **Resume:** refused when ready or when `io` is missing. The decoder is cleared before F4, and `ready` is set only after the ACK.
- **Pointer start:** AD, A8, aux FF, BAT 0xAA, ID 0x00, aux F6/E6/F4. On failure A7, then AE and resume, then a final A7. The field resets (pointer, pending, lost, key count, key head, then `pointer_ready`) happen only on success.
- **Pointer stop:** best effort, AD, aux F5, A7, AE (every step runs), then the accepted field resets.

## Verification (accepted R2 development worker, campaign `r2-ps2-lifecycle-dev-v1`, run r1)

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/ps2-lifecycle.c` is byte-identical to the `ed3b496` driver (`a50dc06e…`), with its `ed3b496` headers. The public APIs are renamed at compile time only. Reference and candidate share the accepted Rust helpers from the production archive. |
| Differential | **25,133** sequence comparisons each in production-archive release, C ASan/UBSan and checked Rust. Each compares return values, every controller read and write (port, value, callback table), a full 88-byte object snapshot whenever the object changed at a callback, response, write and read counts, queue state, packets and the final object. |
| Response coverage | Six sequences: init; init + suspend; + resume; init + pointer start (+ poll); + stop; and a 19-operation repeated suspend/resume and start/stop cycle. At **every** controller response (mode byte, each ACK, BAT, ID; up to 46 per sequence) the harness injects all 256 byte values, a channel flip, ERR and a dropped response. |
| Write waits | 99,999 busy polls (success) and 100,000 (timeout) before every write in five sequences |
| Drain edges | 31, 32 and 33 bytes queued before init: success, success, failure. 31, 32 and 33 extra bytes after every response in the suspend/resume/start/stop sequences. |
| Other coverage | 288 `io` substitutions after N accesses (init keeps using the caller's table for its drain). Edge states: not ready, pointer already active, io missing, nonzero non-1 flags. `io`, `in` or `out` missing is rejected with no access and the object is reset. The emulator fails the run on any controller command outside the accepted set. |
| Random scenarios | **3,000** random 24-operation sequences (init, poll, suspend, resume, start, packet poll, stop, key and mouse input) under fault masks 0–31: stall, parity error, NAK, input-buffer busy, stray bytes |
| Safe-core tests | 29/29 |
| Semantic mutants | 38/38 caught by their designated assertions. They cover: object reset order, disable order, drain bound/limit/final check/caller-io, mode command/set/clear/order, command order, missing command, ready timing, suspend no-op/ready timing/ACK value/ACK search bound/read failure/drain bound/decoder reset, resume guard/decoder timing/ready, start guard/suspend check/A8, BAT, ID, aux order, both rollbacks, resume skip and short-circuit, start reset order and `pointer_ready` timing, stop guard, best effort and reset. |
| Affected regressions | 11 CI scripts PASS: the PS/2 decoder, pointer, routed, ordinary-poll, controller-read and controller-write checks, the keyboard queue, console line/accept/reader, and boot-HID. The read and write scripts' lifecycle sections now compare their frozen C oracles against the Rust lifecycle. |
| Production ownership | Each of the five lifecycle and five helper entries has exactly one Rust `T` owner. The driver object has `U` for the five lifecycle entries and no controller access in its source. The final kernel's C functions tail-jump into Rust (`production-lifecycle-entries.asm`). |
| Reproducibility | Kernel `7c8dd5de…` and archive byte-identical across independent rebuilds |

**Ownership-assertion update (disclosed):** the accepted read-helper and R2-W scripts asserted that the C driver object *references* their entries. After R2-L the C driver no longer calls them; the Rust lifecycle does. Those two assertions now require that the C driver defines none of the helpers and forwards its lifecycle to Rust. Every other check in those scripts is unchanged and passes.

## Resources

- Rust lifecycle frames compared with the `ed3b496` C: `init` 40 = 40, `suspend` 40 = 40, `resume` 8 < 24, `pointer_start` 24 < 40, `pointer_stop` 8 = 8. The C ABI functions are 0 B tail jumps. The retained helper (40), ordinary (40) and routed (8) frames are unchanged.
- Loaded-image growth is **+0 bytes** against the 4,096 B limit (LOAD memsz 962,560 B before and after). The ELF file is 6,096 B smaller.
- No heap, DMA or new persistent state.
- **Evidence volume:** the regression job no longer keeps a duplicate copy of the identical archive disassembly in each script's output. Each pruned manifest keeps its size and SHA-256 (`461ef96e…`), equal to the copy the lifecycle job keeps. A run is 3.35 MB, against R2-W's 6.78 MB.

## Performance (paired release medians, 9 samples, `performance.csv`)

| Workload | C ns | Rust ns | Change |
| --- | --- | --- | --- |
| `keyboard_init` (full sequence) | 575.10 | 556.17 | −3.3% |
| suspend + resume | 80.53 | 82.33 | +2.2% |
| pointer start | 322.20 | 322.07 | −0.0% |
| pointer stop | 91.97 | 94.88 | +3.2% |
| resume refused (already ready) | 21.00 | 18.50 | −11.9% |
| init refused (no io) | 23.68 | 20.27 | −14.4% |

No workload exceeds 5%. These are hosted measurements against the emulator; they are not guest or physical-hardware performance.

## Runtime and GUI

- **Input console, BIOS and UEFI, 128 MiB:** every boot initializes the controller through the Rust lifecycle, and both logged `input: PS/2 ready, translated set 1, polling; IRQ masks unchanged`. Real QMP key events gave `input> VOS OK` and `input> hello vos`. Completed-line delivery, exact-limit editing, overflow discard and recovery passed. Screenshots: `bios-input-console.png`, `uefi-input-console.png`, `uefi-input-limits.png`.
- **Normal production boot, 128 MiB:** PASS. **Production images at 2 GiB, BIOS and UEFI:** boot markers PASS.
- **GUI, BIOS/UEFI × 128 MiB/2 GiB:** all ten ring-3 GUI cases and the process-exhaustion rollback PASS. A real key event changes native pixels, checked against the independent RGB oracle. Screenshots: `gui-bios-128M-after-key.png`, `gui-uefi-2G-after-key.png`.
- All five screenshots were captured fresh in run r1. They are byte-identical to the PR #61 captures of the same screens, because the rendering is deterministic and the visible output is identical.

## Limitations

- Run r1 was the first and only campaign run; no failed iteration occurred.
- All builds and tests ran inside the accepted R2 development worker, unchanged from PR #61. This is QEMU verification, not physical-hardware evidence. No physical hardware was accessed.
