# R2-W: Rust bounded PS/2 controller writes (write_port / command / aux_command)

**Correctness, lifecycle equivalence, resources, reproducibility, and the BIOS/UEFI runtime and GUI checks all PASS. One micro-benchmark is above the 5% threshold: the 100,000-poll all-busy timeout (`write_timeout100k`), +13.4% (about +0.38 ns per poll). It needs explicit owner approval of this exact source. No merge without owner approval.** Baseline is accepted main `74abed0de259a7c0a271c087acd8ab7cbb6150a7` (owner-merged PR #60).

## Production scope and ABI

The three bounded controller-write helpers in `arch/x86_64/keyboard.c` move to Rust. `write_port()`, `command()` and `aux_command()` keep their static C signatures and forward to the private entries `vos_rust_keyboard_write_port`, `vos_rust_keyboard_command` and `vos_rust_keyboard_aux_command`.

No controller write remains in C: the driver contains no `->out(` call and no IBF wait loop. Everything else stays in C and is unchanged:
- the lifecycle functions (init, suspend/resume, pointer start/stop)
- the queue, IRQ rules and scheduler
- the public ABI

`struct keyboard` stays 88 bytes.

- `kernel/rust/input/ps2_controller_write.rs` is a safe core (`#![forbid(unsafe_code)]`). It writes only through a typed `Port`, which has two values, data (0x60) and command (0x64); no other port can be expressed. ACKs use the accepted bounded Rust controller read.
- `arch/x86_64/rust/ps2_routed.rs`: the `Raw` boundary's output callback changes from an opaque pointer to a nullable `Option<unsafe extern "C" fn(u16, u8)>` of the same size. Every status read and every write reloads `io`, as the accepted C does.
- The entries refuse an invalid object address with no access, using the existing range and alignment check. `write_port` also refuses any port other than 0x60/0x64 with no access, and a null `out` callback is refused instead of called. The C driver never reaches these refusal cases; they harden the Rust entry points.

The accepted contract is preserved:
- `write_port`: at most 100,000 iterations (`0x186a0`), each reading status (0x64). It writes exactly once, to the given port and value, after IBF clears. After a timeout it returns 0 and never writes.
- `command`: write to 0x60, then a keyboard-channel ACK, which must be 0xFA. It stops at the first failure.
- `aux_command`: 0xD4 to 0x64, then the byte to 0x60, then an aux-channel ACK of 0xFA, with the same short-circuit order.

## Verification (accepted R2 development worker, campaign `r2-ps2-write-helpers-dev-v1`, run r2)

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/ps2-controller-write.c` is byte-identical to the `74abed0` driver (`fe25c367…`), with its `74abed0` headers. A wrapper exports the original static helpers. APIs are renamed at compile time only. |
| Helper differential | **553,774** comparisons each in production-archive release, C ASan/UBSan and checked Rust. Each compares the exact return value and an FNV hash plus count of every read *and write* (port, value, callback table), plus read and write counts, script position and the full 88-byte object. |
| Exhaustive coverage | `write_port`, both ports: every first status byte × every value (2 × 65,536). `command`: write status × read status, and read status × ACK byte (2 × 65,536). `aux_command`: both write statuses, and read status × ACK byte (2 × 65,536). |
| Wait-limit boundaries | 0, 1, 2, 1,000, 99,998, 99,999 and 100,000 busy polls, for both ports of `write_port`, the `command` write, and both `aux_command` writes. Success iff ≤ 99,999. At 100,000 there are exactly 100,000 status reads and no write. |
| Failure injection | Every step of `command` (write timeout, ACK timeout, ERR, NAK, aux byte consumed) and `aux_command` (prefix timeout, data timeout, ACK timeout, ERR, NAK, keyboard byte consumed). Exact write and read counts are asserted. |
| Other coverage | 160,000 seeded random streams (seed `7232772d77726974`) and 512 mid-operation `io` substitutions, where later reads and writes must use the new table. Refusals: 8 unauthorized ports, NULL, misaligned and wrapping objects, and a null `out`. A guard-page-edge object succeeds. |
| Lifecycle regression | **3,000** emulated-i8042 scenarios over 32 fault masks (stall, parity error, NAK, input-buffer busy, stray byte): init → poll → pointer start/poll → stop → suspend → resume → poll. The production set is compared with the frozen C set on results, objects and full read+write traces. |
| Safe-core tests | 24/24 |
| Semantic mutants | 25/25 caught by their designated assertions. Wait bounds: bound, limit 100,001. Wrong status bit and status port. Write order (write before status). Missing write, extra write, write after timeout, write result ignored. Value. Port address, port mapping, port refusal. Command port, channel, ACK check and short-circuit. ACK value. Aux prefix value, prefix port, missing prefix, write order, short-circuit, channel and ACK check. |
| Affected regressions | 10 CI scripts PASS: the PS/2 decoder, pointer, routed, ordinary-poll and controller-read checks, the keyboard queue, console line/accept/reader, and boot-HID |
| Production ownership | Each entry has exactly one Rust `T` owner, and the driver object has `U` for each. The final kernel has 26 C call sites, all in the lifecycle functions (`production-write-entries.asm`): `keyboard_init` 6 + 4, `keyboard_suspend` 1, `keyboard_resume` 1, `keyboard_pointer_start` 5 + 1 + 4, `keyboard_pointer_stop` 3 + 1. |
| Reproducibility | Kernel `de8f1fa4…` and archive `33e886e4…` are byte-identical across independent rebuilds, and identical between runs r1 and r2. |

## Resources

- All three Rust write entries use **40-byte** frames. The C callers are equal or smaller than an unwrapped build of the `74abed0` driver: `keyboard_init` 40 = 40, suspend 40 = 40, resume 24 < 40, `keyboard_pointer_start` 40 < 56, `keyboard_pointer_stop` 8 = 8. The retained read (40), ordinary (40) and routed (8) frames are unchanged.
- Loaded-image growth is **+0 bytes** against the 4,096 B limit (LOAD memsz 962,560 B before and after). The ELF file is 1,344 B smaller.
- No heap, DMA or new persistent state.

## Performance (paired release medians, 9 samples, `performance.csv`)

| Workload | C ns | Rust ns | r2 change | r1 change (same production code) |
| --- | --- | --- | --- | --- |
| `write_port`, immediate | 9.44 | 9.18 | −2.8% | +7.8% |
| `write_port`, 10 busy polls | 47.67 | 49.56 | +4.0% | +13.5% |
| `command`, ACK | 17.55 | 15.04 | −14.3% | −14.3% |
| `aux_command`, ACK | 22.42 | 21.93 | −2.2% | −3.9% |
| `command`, NAK | 17.54 | 15.04 | −14.3% | −15.6% |
| `write_port`, 100,000-poll timeout | 283,824.75 | 321,809.70 | **+13.4%** | +6.4% |

The Rust and C wait loops compile to the same per-poll sequence: one `io` reload, one indirect status call, one bit test, one counter step and one taken branch. The only added work is fixed per call:
- the existing address check
- the two-value port check
- the null-`out` check

The `write_port` results moved between −2.8% and +13.5% across two runs of byte-identical production code, so this benchmark is sensitive to code layout and timing noise. The timeout case regressed in both runs.

The timeout path runs only when the controller never clears IBF across 100,000 polls, which is a failure path. On real hardware each status read is a microsecond-scale I/O access, so the extra ~0.38 ns per poll is not material. Every `command` and `aux_command` path is equal or faster. **Owner approval of this exact source is required for `write_timeout100k`.**

## Runtime and GUI

- **Input console, BIOS and UEFI, 128 MiB:** controller initialization now performs every controller write through the Rust entries, and both boots logged `input: PS/2 ready, translated set 1, polling; IRQ masks unchanged`. Real QMP key events covered shift, caps-lock, backspace and Enter, giving `input> VOS OK` and `input> hello vos`. Completed-line delivery, exact-limit editing, overflow discard and recovery passed. Screenshots: `bios-input-console.png`, `uefi-input-console.png`, `uefi-input-limits.png`.
- **Normal production boot, 128 MiB:** PASS. **Production images at 2 GiB, BIOS and UEFI:** boot markers PASS.
- **GUI, BIOS/UEFI × 128 MiB/2 GiB:** all ten ring-3 GUI cases and the process-exhaustion rollback PASS. A real key event changes native pixels, checked against the independent RGB oracle. Pointer start/stop, which now uses the Rust `aux_command`, is covered by the lifecycle differential above. Screenshots: `gui-bios-128M-after-key.png`, `gui-uefi-2G-after-key.png`.
- All five screenshots were captured fresh in run r2. They are byte-identical to the PR #60 captures of the same screens, which is expected because the rendering is deterministic and the visible output is identical.

## Iterations and limitations

- **r1 (STOP, preserved):** the write-helper job failed on a harness defect. The new driver-ownership check matched the substring `IBF`, which also appears in the forwarding comment. Before that stop, the differential, sanitizer, checked Rust, 24 tests, 25 mutants and frames had all passed. The check now looks for no `->out(` call and no `#define IBF`. Production sources were unchanged between runs: kernel and archive hashes are identical. `failed-iteration-r1.json` records r1, including its performance numbers.
- **Environment:** all builds and tests ran inside the accepted R2 development worker, unchanged from PR #60 (4 GiB, no swap, 64 tasks, read-only worktree, tmpfs build and scratch). UEFI uses the hash-recorded GRUB tree.
- This is QEMU verification, not physical-hardware evidence. No physical hardware was accessed.
