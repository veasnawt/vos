# R2: Rust bounded PS/2 controller reads (read_keyboard / read_aux)

**Correctness, lifecycle equivalence, resources, reproducibility, and the BIOS/UEFI runtime and GUI checks all PASS. One micro-benchmark, a 10-spin OBF wait (`delay10`), is 5.3% slower (+5.65 ns), which is above the 5% threshold. Under the standing performance policy it needs explicit owner approval of this exact source. No merge without owner approval.** This is the next increment of the ongoing selected-input R2 program. Baseline is accepted main `85f83e9437e4841fc3008965156b6fef14bd7da4` (owner-merged PR #59).

## Production scope and ABI

Only the two bounded controller-read helpers in `arch/x86_64/keyboard.c` move to Rust. `read_keyboard()` and `read_aux()` keep their static C signatures and forward to the private entries `vos_rust_keyboard_read_keyboard` and `vos_rust_keyboard_read_aux`.

Everything else stays in C and is unchanged:
- the controller-write helpers (`write_port`, `command`, `aux_command`)
- the lifecycle functions (`keyboard_init`, suspend/resume, pointer start/stop)
- the queue, IRQ rules and scheduler
- the public ABI

`struct keyboard` stays 88 bytes.

- `kernel/rust/input/ps2_controller_read.rs` is a safe core (`#![forbid(unsafe_code)]`) behind a one-method `ControllerRead` adapter. It has no heap, DMA, persistent state or port writes.
- `arch/x86_64/rust/ps2_routed.rs`: the existing `Raw` boundary implements the adapter with its accepted `read` accessor, which reloads `io->in` on every port access. The entries refuse an invalid object address (the existing routed range and alignment check) and a null output. They write the output byte only on success.

The accepted contract is preserved:
- The exact wait limit is 100,000 iterations (`0x186a0` in the production entry).
- Status (0x64) is always read before data (0x60), and data is never read while OBF is clear.
- When OBF is set, the data byte is consumed first. ERR (0xC0) then fails before channel filtering, whatever the channel.
- A byte from the opposite channel is consumed and the wait continues.
- On success the byte is returned with result 1. On failure the result is 0 and the output is untouched.

## Verification (accepted R2 development worker, read-helper run r1)

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/ps2-controller-read.c` is byte-identical to the `85f83e9` driver (`2cf4069c…`), with its `85f83e9` headers. A wrapper exports the original static helpers. APIs are renamed at compile time only. |
| Helper differential | **211,346** comparisons each in production-archive release, C ASan/UBSan and checked Rust (overflow checks). Each compares the exact return value, the output byte or untouched sentinel, the full 88-byte object and the exact port-read trace. |
| Coverage | Per channel: all 65,536 first (status, data) cells; the exact 100,000-read timeout; a 200,000-read opposite-channel flood; OBF-clear delays {0, 1, 2, 1000, 99,998, 99,999, 100,000} (success iff ≤ 99,999); 40,000 seeded random cases (seed `72326374726c7264`); 128 mid-wait `io` swaps; NULL, NULL-output, misaligned, wrapping and guard-page-edge objects |
| Lifecycle regression | **3,000** scenarios driven through an emulated i8042 with 32 fault masks (stall, parity error, NAK, input-buffer busy, stray byte): init → poll ×2 → pointer start → pointer poll → poll → stop → suspend → resume → poll. The production set is compared with the frozen C set on results, objects and full read+write traces. A fault-free run must succeed. |
| Safe-core tests | 14/14 |
| Semantic mutants | 12/12 caught by their designated assertions: bound, limit 100,001, OBF, OBF-continue, ERR, ERR order, channel, AUX bit, status/data order, data, other-channel consumption, final none |
| Affected regressions | 9 CI scripts PASS: the PS/2 decoder, pointer, routed and ordinary-poll checks, the keyboard queue, console line/accept/reader, and boot-HID |
| Production ownership | Each entry has exactly one Rust `T` owner in the archive, and the driver object has `U` for each. The final kernel has 7 C call sites into the two entries (`production-read-entries.asm`). |
| Reproducibility | Kernel `7b27f1e6…` and archive `9ceea503…` are byte-identical across independent rebuilds. |

## Resources

- The Rust read entries use **40-byte** frames. The C callers' frames equal an unwrapped build of the `85f83e9` driver: `keyboard_init`, suspend and resume 40 B, `keyboard_pointer_start` 56 B, `keyboard_pointer_stop` 8 B. The retained ordinary (40 B) and routed (8 B) frames are unchanged.
- Loaded-image growth is **+0 bytes** against the 4,096 B limit (LOAD memsz 962,560 B before and after). The ELF file is 256 B smaller.
- No heap, DMA or new persistent state.

## Performance (paired release medians, 9 samples, `performance.csv`)

| Workload | C ns | Rust ns | Change |
| --- | --- | --- | --- |
| keyboard immediate | 48.15 | 40.10 | −16.7% |
| aux immediate | 46.12 | 43.28 | −6.1% |
| 10 OBF-clear spins then keyboard (`delay10`) | 106.15 | 111.80 | **+5.3%** |
| other channel then keyboard | 22.00 | 20.80 | −5.5% |
| ERR | 12.28 | 11.14 | −9.2% |
| 100,000-read timeout | 414,015.80 | 432,720.80 | +4.5% |

The `delay10` regression is consistent: Rust is slower in all 9 pairs. It costs about 0.2 ns per OBF-clear spin, and the timeout workload shows the same per-spin effect at +4.5%, just under the threshold. Every path that returns a byte or an error without waiting is faster.

The emulated port read in this benchmark costs about 1 ns. On real i8042 hardware each status read is an I/O-port access costing on the order of a microsecond, so the spin loop's own overhead is not material. **Owner approval of this exact source is required for the `delay10` regression.**

## Runtime and GUI

- **Input console, BIOS and UEFI, 128 MiB:** real QMP key events covered shift, caps-lock, backspace and Enter, giving `input> VOS OK` and `input> hello vos`. Completed-line delivery, exact-limit editing, overflow discard and recovery passed. Controller initialization now reads its responses through the Rust entries. Both boots logged `input: PS/2 ready, translated set 1, polling; IRQ masks unchanged`. Screenshots: `bios-input-console.png`, `uefi-input-console.png`, `uefi-input-limits.png`.
- **Normal production boot, 128 MiB:** PASS. **Production images at 2 GiB, BIOS and UEFI:** boot markers PASS.
- **GUI, BIOS/UEFI × 128 MiB/2 GiB:** all ten ring-3 GUI cases and the process-exhaustion rollback PASS. A real key event changes native pixels, checked against the independent RGB oracle. Screenshots: `gui-bios-128M-after-key.png`, `gui-uefi-2G-after-key.png`.
- All five screenshots were captured fresh in run r1. They are byte-identical to the PR #59 captures of the same screens, which is expected because the rendering is deterministic and the visible output is identical. `runtime-result.json` is identical for the same reason.

## CI reliability change (separate commit)

The PR #59 post-merge Build smoke run `42304789235` stays **INCOMPLETE**. Its "Freestanding builds" job was cancelled at the 25-minute job timeout in both attempts, while package installation ran against a 49.6 kB/s mirror. Its QEMU job passed. A pass on this PR does not change that historical record.

This PR's separate CI commit does three things: it bounds the build job's package-install step at 20 minutes, adds apt `Acquire::Retries=3`, and raises that job's timeout to 45 minutes. No test, check or step is removed or weakened.

## Environment notes and limitations

- All builds and tests ran inside the accepted R2 development worker (4 GiB memory, no swap, 64 tasks; read-only worktree; the only writable areas are the 256 MiB build tmpfs and 64 MiB scratch). UEFI images use the hash-recorded GRUB `x86_64-efi` tree, bound read-only. The worker shims `cc`, `c++` and `awk` as in PR #59.
- This is QEMU verification, not physical-hardware evidence.
- Run r1 was the first and only campaign run; no failed iteration occurred.
