# R2: Rust ordinary keyboard_poll()

**Correctness, resources, reproducibility, BIOS/UEFI runtime and GUI checks PASS. Two micro-benchmarks regress by more than 5% (about 1 ns each). Under the standing performance policy they need explicit owner approval of this exact source. No merge without owner approval.** This is the next increment of the ongoing selected-input R2 program, not a newly named milestone. Baseline is accepted main `63a318d9d75e4e22951f549650db85ea245bba51` (owner-merged PR58).

## Production scope and ABI

Only the ordinary (`pointer_ready == 0`) bounded polling loop of `keyboard_poll()` moves from C to Rust. The public C dispatcher stays: it reads `pointer_ready` once, then calls either the existing routed Rust queue path or the new private entry `vos_rust_keyboard_ordinary_poll`. Initialization, controller writes, suspend/resume, pointer leases, the queue, IRQ rules, the scheduler and the public ABI are unchanged. `struct keyboard` stays 88 bytes, with `ready` at offset 16 and `pointer_ready` at offset 20.

- `kernel/rust/input/ps2_ordinary.rs`: a safe, `#![forbid(unsafe_code)]` core behind a four-method adapter (`ready`, `read`, `reset_decoder`, `decode`). It has no heap, DMA, persistent state or controller writes.
- `arch/x86_64/rust/ps2_routed.rs`: the existing `Raw` boundary implements the adapter by reusing its accepted accessors. `read()` reloads `io->in` on every port access. No borrow spans a C callback. The entry refuses null, misaligned or wrapping addresses, as the routed entry already did.

The accepted contract is preserved:
- `ready` is tested once, before any port access.
- There are at most 32 iterations and 64 reads.
- Status (0x64) is always read before data (0x60), and data is never read while OBF is clear.
- AUX bytes are consumed without decoding, and AUX takes precedence over ERR.
- ERR, 0x00 and 0xFF reset all seven decoder bytes.
- The first nonzero decoded character is returned (signed `char` bits preserved); otherwise 0.

## Verification (accepted R2 development worker, run r5)

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/ps2-keyboard-ordinary.c` plus headers, byte-identical to the `63a318d` source (`0a45e7a5…`), with APIs renamed at compile time only |
| Differential | **301,778** comparisons each in production-archive release, C ASan/UBSan, and checked Rust (overflow checks): exact return value, full 88-byte object and exact port-read trace |
| Coverage | All 65,536 first (status, data) cells; 128 decoder-flag states × 256 scans; all 65,536 byte pairs; `ready` classes; the exact 32-iteration/64-read bound; 128 callback-mutation profiles × 3 trigger points (decoder, `ready`, io swap, `pointer_ready`, queue fields); 40,000 seeded random cases (seed `72326f7264696e31`); guard-page edge, NULL, high and misaligned objects |
| Safe-core tests | 17/17 |
| Semantic mutants | 14/14 caught by their designated assertions: ready, bound, OBF, order, AUX, AUX precedence, error, zero, 0xFF, reset, reset-continue, first-return, high-byte return, final value |
| Affected regressions | The CI scripts for the PS/2 decoder, pointer, routed, queue, console line/accept/reader and boot-HID all PASS |
| Production dispatch | `keyboard_poll` reads `pointer_ready` once, then tail-jumps to `vos_rust_keyboard_ordinary_poll` or the routed path (`production-dispatch-and-ordinary.asm`); exactly one Rust owner, and C only forwards |
| Reproducibility | Kernel `6de30da1…` and archive `7629d20f…` byte-identical across independent rebuilds |

## Resources

- Own ordinary frame: **40 bytes**, equal to the measured accepted C frame (40 B) and the 40 B ceiling. The routed frames stay within their retained budgets.
- Loaded-image growth: **+0 bytes** against the 4,096 B limit (LOAD memsz 962,560 B before and after); the ELF file is 336 B smaller.
- No heap, DMA or new persistent state.

## Performance (paired release medians, `performance.csv`)

| Workload | C ns | Rust ns | Change |
| --- | --- | --- | --- |
| unready | 7.81 | 8.96 | **+14.7%** |
| empty (OBF clear) | 11.32 | 12.00 | **+6.0%** |
| one key | 20.05 | 19.37 | −3.4% |
| aux then key | 27.57 | 26.08 | −5.4% |
| error reset then key | 28.41 | 27.46 | −3.3% |
| 32-byte bound | 439.57 | 441.18 | +0.4% |
| shifted key | 31.29 | 31.91 | +2.0% |
| routed control | 13.37 | 13.09 | −2.1% |

Both regressions are on the two paths that do no keyboard work. They add about 1.15 ns and 0.68 ns: one cross-language tail call plus the retained numeric address validation at the private entry. Every path that reads or decodes is equal or faster. Removing the address validation would remove a safety check, so it was not done. **Owner approval of this exact source is required for these two regressions.**

## Runtime and GUI

- **Input console (ordinary path), BIOS and UEFI, 128 MiB:** real QMP key events covered shift, caps-lock, backspace and Enter, giving `input> VOS OK` and `input> hello vos`. Completed-line delivery, exact-limit editing, overflow discard and recovery passed. Screenshots: `bios-input-console.png`, `uefi-input-console.png`, `uefi-input-limits.png`.
- **Normal production boot, 128 MiB:** PASS. **Production images at 2 GiB, BIOS and UEFI:** boot markers PASS.
- **GUI, BIOS/UEFI × 128 MiB/2 GiB:** all ten ring-3 GUI cases and the process-exhaustion rollback PASS. A real key event changes native pixels, checked against the independent RGB oracle. Screenshots: `gui-bios-128M-after-key.png`, `gui-uefi-2G-after-key.png`.

## Environment notes and limitations

- **Where it ran:** all builds and tests ran inside the accepted R2 development worker (4 GiB memory, no swap, 64 tasks; read-only worktree; writable areas only the 256 MiB build tmpfs and 64 MiB scratch). This is QEMU verification, not physical-hardware evidence.
- **UEFI images:** these use the hash-recorded GRUB `x86_64-efi` tree from earlier UEFI work, bound read-only into the worker (`GRUB_EFI_DIR`), because this host has no system copy.
- **Tool shims:** the worker exposes no `/etc`. Each job therefore links `cc`, `c++` and `awk` to the same binaries the host's Debian alternatives resolve to.
- **GUI coverage:** `scripts/test-gui.py` is bound to a historical N6.2 phase root and accepted disk image, so it was not used as-is. The GUI job reuses its own QMP, capture and pixel-oracle helpers on images built from source, and stops before the disk-dependent shell flow.
- **Failed runs preserved:** earlier worker runs r1–r4 stopped on environment and harness defects:
  - r1: no UEFI GRUB tree
  - r2: a misleading-indentation warning in the new test
  - r3: no `cc` alternative in the worker
  - r4: the historical GUI script

  Their evidence is retained, not relabelled.
