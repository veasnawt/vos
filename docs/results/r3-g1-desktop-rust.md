# R3-G1: Rust desktop request and layout decisions

**These all PASS:**
- the frozen-C differential and the mutation controls;
- the ownership, panic and ABI checks;
- the accepted N6 desktop and pointer render host tests;
- the BIOS/UEFI pixel-identical native desktop regressions;
- host GCC 13 CI equivalence.

Worker run r1 passed. Worker CI-equivalence run r2 stopped on build-tmpfs capacity in the new gate; that is fixed and disclosed below. No merge without owner approval.

Two measurements need an explicit owner decision under the R-series performance and stack policy (see [Resources](#resources)):
- Under GCC 15 only, the unchanged C caller `desktop_paint` grows its entry frame by 16 B, because it now calls `desktop_compute` across the language boundary instead of inlining it.
- The final `desktop_compute` measured +0.2% in one run and −0.9% in a quiet rerun, which is parity within noise.

Baseline is accepted main `2c0890a3155953c85369f281c036f3c46560ef39` (R3-N3-S merged). The branch starts at `f3c6048`, whose tree `f3ec9622` is identical to main's, so no local fetch was needed.

## Scope

The two pure decisions of the N6 desktop scene move from `arch/x86_64/desktop.c` into safe `no_std` Rust (`kernel/rust/gfx/desktop.rs`, `#![forbid(unsafe_code)]`):

| Function | Decision |
| --- | --- |
| `desktop_requested(const char *)` | A space/tab-separated command-line field is exactly `vos.desktop=1`; a prefix, suffix or substring never matches; NULL is 0 |
| `desktop_compute(width, height, out)` | Refuse below 640x480 or above `BOOT_FB_MAX_DIM` (8192), leaving `*out` untouched; otherwise write `{24, 44, w-48, h-104, 36, 84, w-72, h-156}` |

`arch/x86_64/rust/desktop.rs` exports both with the unchanged `desktop.h` C ABI. It is the only unsafe code: it reads the caller's NUL-terminated command line and writes the caller's `struct desktop_layout`, which const asserts pin at 32 B (offsets 0/12/16/28). There is no C shim.

`desktop_requested` is the accepted C's own scan, written as safe Rust over an inlined byte-reader closure. It reads exactly the bytes the C reads, and stops at the NUL or at the first matching field. The guard-page differential verifies that it never reads past the NUL.

**Unchanged:** `desktop_paint`, `desktop_bar`, `screen.c`, `kernel.c`, rendering and colours, framebuffer ownership, `desktop.h`, every layout value, and the return conventions. No window, compositor or GUI ABI is added or changed. N6.4 remains deferred.

## Verification

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/desktop-r3g1.c` is byte-identical to main's `desktop.c` (blob `d67f9aa5`, manifest `tests/reference/desktop-r3g1.json`). It is not linked into the kernel. |
| Differential (`scripts/test-desktop-rust.py`, new Build smoke step) | **563,445 command lines:**<br>• structured fields with 11 separators, up to three fields<br>• every substitution of the token with each byte 1–255, every insertion, deletion and prefix, in five contexts<br>• page-long lines<br>• 200,000 seeded random lines<br>Each line sits flush against a PROT_NONE guard page.<br>**890,761 layouts:**<br>• rows and columns 0–10,000 against 29 edge values<br>• dense boxes at the 640x480 and 8192 bounds<br>• all edge pairs<br>• 300,000 random pairs<br>Outputs are prefilled and compared byte for byte, including untouched outputs on refusal. NULL inputs are also covered.<br>**0 mismatches**, with an identical digest `14508fd8…` in the release build (production kernel archive) and in the ASan/UBSan build (overflow-checked, debug-asserted Rust). The exact case counts are pinned. |
| Mutation controls | **27/27 detected.** They cover:<br>• both bounds of each range and the three limit constants<br>• all eight layout values<br>• the token, the separator set (no tab, extra newline, untrimmed leading tab, detected as a hang)<br>• prefix, end-of-line, short-compare and substring matching<br>• NULL handling, writing on refusal, and swapped fields |
| Host render tests | The accepted N6 desktop host test (`tests/host/desktop.c`, real `screen.c` renderer over guarded RAM) and the pointer renderer (2,179,862 assertions) pass against the Rust decisions. The CI pointer gate (`scripts/test-ps2-pointer-rust.py`) now links `desktop_compute` from the production archive. |
| Ownership, panic freedom, ABI | `vos.bin` defines both symbols from `libvos_kernel.a` (link map), and `desktop.o` defines neither. There is no panic machinery in the kernel. Both bodies make no calls and have 0 B frames, equal to the C. |
| Native pixel identity (`scripts/test-desktop-native.py`) | The baseline (main) and candidate kernels boot the same image, with the accepted `vsh`/`hello`/`rixie` ELFs, to the real vsh prompt. The QMP raw screendumps are **pixel-identical in all 12 cases**, with the same desktop decision:<br>• SeaBIOS and OVMF at 1024x768, 800x600 and the 640x480 minimum<br>• OVMF at 1280x800<br>• a tab-separated field<br>• not requested, and a near-miss token on BIOS and UEFI<br>• the 640x400 refusal and console fallback on SeaBIOS<br>OVMF offers no mode below 640x480 in this configuration, so the native refusal is SeaBIOS-only; the host differential covers every refusal exhaustively. |
| Worker campaign `r3-g1-desktop-v1` | **r1 PASS** (seal `bf96af18…`), five jobs:<br>• containment<br>• build, and a byte-identical rebuild of kernel and archive<br>• the R3-G1 gate<br>• the native pixel-identity matrix (12/12), against a baseline kernel rebuilt in the sandbox from the frozen `desktop.c`, whose `.text/.rodata/.data/.bss` equal main's, and source-built `vsh`/`hello`/`rixie` equal to the accepted identities<br>**r2 STOP** (CI equivalence, seal `1858ffe2…`, preserved): `ci-build` passed; in `ci-boot` the 256 MiB build tmpfs filled during the R3-G1 gate, because it kept one host Rust staticlib per mutant. This was fixed in `test(gfx): bound the R3-G1 gate's mutant build footprint` (peak output above 200 MB → about 16 MB, with no check changed). A repair run was not authorized, and it would approach the 100,000-byte receipts cap (69,647 used). The remaining worker CI groups are therefore **NOT TESTED in the worker**; they are covered below and by exact-head GitHub CI. |
| CI equivalence (host) | **GCC 13.4 on the exact committed source/test tree: every hosted Build smoke step PASS (38/38)**, including the new R3-G1 step, the updated pointer gate and C4.1/C4.2a/USB.3-A. After the footprint fix, the gate PASSES again under GCC 13 and GCC 15 with identical digests and 27/27 mutants. |

### Pre-existing harness failure (not changed)

`scripts/test-ps2-pointer-native.py`, the accepted R2-B native pointer/GUI regression, fails identically on the **baseline main kernels** and on the candidate. It times out in its default gdb execution proof at `pointer_idle`, a symbol that `pointer.c` and the Rust `ps2_pointer.rs` both define, after the guest has reached `pointer-demo: READY`. The script was not modified. Its pointer/GUI interaction checks are therefore **NOT TESTED** for this increment. Desktop rendering is covered by the pixel-identity matrix, and the pointer renderer by the host test.

The historical N6 acceptance scripts (`test-n6-desktop.py`, `test-gui.py`, `test-pointer.py`, `test-cursor-refinement.py`) compile `desktop.c` on the host and pin private historical evidence. They are not in CI and are not modified.

## Resources

- **Frames:** `desktop_requested` and `desktop_compute` have 0 B entry frames and make no calls, as the frozen C did under kernel flags.
- **Whole-path stack depth** (linked kernels, GOT-indirect calls resolved), base → candidate:

  | Path | GCC 15 | GCC 13 |
  | --- | --- | --- |
  | `kernel_main` | 12,336 = 12,336 | 12,336 = 12,336 |
  | `screen_init` | 488 = 488 | 512 = 512 |
  | `screen_desktop_begin` | 232 → **248** | 240 = 240 |
  | `desktop_paint` | 216 → **232** | 208 = 208 |

  **Exception requested (GCC 15 only):** `desktop_paint`'s own entry frame grows 72 → 88 B. `desktop_paint` itself is unchanged C. It now makes a cross-language call to `desktop_compute`, which GCC 15 previously inlined, and keeps one more callee-saved register across that call. The deepest kernel path is unchanged.
- **Image:** loaded memory is unchanged (GCC 15: 970,752 B, end `0x1ED000`; GCC 13: 966,656 B, end `0x1EC000`). The ELF file is 1,264 B smaller (GCC 15) and 1,208 B smaller (GCC 13).
- No heap, statics, DMA, I/O or new state.

## Performance (R-series performance policy)

Paired hosted medians: 10 alternating runs × 9 samples × 200,000 calls on one CPU, built `-O2 -fno-builtin -U_FORTIFY_SOURCE` with the frozen C object against the production archive. Both functions run once (requested) or twice (compute) per boot. These are hosted micro-benchmarks, not BIOS/UEFI latency, which is NOT TESTED because a single boot-time call is below measurement resolution.

| Version | `desktop_requested` | `desktop_compute` |
| --- | --- | --- |
| First Rust (byte state machine) | +51.9% | +21.5% |
| C scan ported to safe Rust | −12.1% | +21.3% |
| **Final** (dimensions tested before the NULL output), first run | **−9.8%** | **+0.2%** |
| **Final**, quiet rerun (no concurrent load) | **−10.3%** | **−0.9%** |

The investigation is preserved in `performance.json` and the disassembly files:
- The state machine did a table lookup and four compares per byte, where the C skips non-matching fields with one test per byte.
- LLVM merged the two range tests into `setb`/`or` until the NULL test moved after them. The refusal set and the no-write behaviour are identical, as the differential proves.

The final archive is byte-identical to the benchmarked one. `desktop_compute`'s two runs (+0.2% and −0.9%, about ±0.03 ns) bracket zero. Any residual difference is reported for an explicit owner decision rather than accepted silently.

## Evidence

The evidence directory `r3-g1-desktop-rust/` contains:
- `final-result.json`
- `differential.json`
- `native-pixel-identity.json`
- `performance.json`
- `stack-depth.txt`
- `disassembly-v1-statemachine.txt` and `disassembly-final.txt`
- `reproducible-build.txt`
- `worker-containment.json`
- genuine screenshots: `desktop-640x480-uefi.png` (the minimum accepted mode) and `fallback-640x400-bios.png` (the refusal)

`retained-captures.json` indexes every retained screenshot by SHA-256. The evidence is QEMU verification, host differential testing and static analysis only; there is no physical-hardware acceptance.
