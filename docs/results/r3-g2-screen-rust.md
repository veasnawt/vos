# R3-G2: Rust screen geometry and bounds decisions

**These all PASS:**
- a whole-module differential against the frozen accepted `screen.c`, and 51 mutation controls;
- ownership and panic-freedom checks;
- the accepted console, desktop and pointer host tests;
- BIOS/UEFI pixel-identical native console, desktop and GUI/pointer regressions;
- worker runs r1 and r2, and host GCC 13 CI equivalence.

No hot path regresses. Under the R-series policy, C entry-frame growth in eight `screen.c` functions needs an explicit owner decision (see [Resources](#resources)). No merge without owner approval.

Baseline is accepted main `0dbf404f08e40723537cc0b4c825ce8d339ee7e3` (R3-G1 merged). The branch starts at `39a7dfa`, whose tree `a173132b` is identical to main's, so no local fetch was needed.

## Scope

The pure arithmetic and validation decisions of the early-boot screen console move from `arch/x86_64/screen.c` into safe `no_std` Rust (`kernel/rust/gfx/screen.rs`, `#![forbid(unsafe_code)]`). All of them use the C's unsigned modulo-2^32 arithmetic.

| Area | Decisions now in Rust |
| --- | --- |
| Selection | `screen_choose` (unchanged `screen.h` ABI, byte-identical reason strings; a refused tag still reports the loader's own error pointer) |
| Rectangles and glyphs | Fill visibility and clipping; glyph bounds and font index (out-of-range bytes become `?`) |
| Cursor canvas | 32x32 overlap test for cursor damage; clip extents for the underlay save/restore loops |
| Text cells | Column/row/glyph positions; put action (newline, ignored byte, draw, wrap-then-draw); input-mode next row or scroll; ring advance and gap row; erase step-back |
| Geometry | Framebuffer columns/rows, desktop text columns/rows, the 8x2 minimum, 80x25 input clamps |
| GUI sink and pointer | 256x160 room, size limits, centred placement; pointer home, axis clamping, surface-local state and inside flag |

`screen_choose` is exported directly. The other 26 decisions are scalar `vos_rust_screen_*` entry points declared in `rust_kernel.h`. The only unsafe code is `screen_choose`'s write of its static reason pointer.

**Unchanged in C:**
- every framebuffer and VGA read and write, and rendering;
- the cursor lifecycle and its underlay;
- the 80x25 character shadow and the scroll redraw;
- the status bar and panic text layout;
- the GUI row validation;
- device mapping and ownership.

The public ABI, `struct screen_surface`, pixel formats, pitch handling, console and scrolling behaviour, cursor geometry and pointer routing are unchanged. No window, compositor or GUI ABI is involved; N6.4 remains deferred.

## Verification

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/screen-r3g2.c` is byte-identical to main's `screen.c` (blob `22a642e1`, manifest `tests/reference/screen-r3g2.json`). It is not linked into the kernel. |
| Whole-module differential (`scripts/test-screen-rust.py`, new Build smoke step) | The frozen and the production `screen.c`, each with the accepted `desktop.c` renderer, are built as two prefixed modules (`tests/host/screen-diff-unit.c`). They run the same seeded stream over guard-paged framebuffers, VGA cells and other surfaces, in 420 scenarios (framebuffers from 8x15 to 1280x800, padded pitches). The stream covers:<br>• `screen_init` over a validated tag or a refused mapping, and VGA text<br>• every byte value, console output with wrap, scroll and ring gap, and erase<br>• input mode, status, panic and the desktop<br>• GUI begin at the size edges, GUI rows and end<br>• pointer begin, end and moves, including moves aimed at the surface and screen edges<br>• fills and glyphs at the surface and cursor-canvas edges<br>After every operation the return values, the full module state, every framebuffer pixel and every VGA cell must match. `screen_choose` is compared exhaustively (215,040 input combinations, including error-pointer identity).<br>**0 mismatches.** The digest is identical in the release build (kernel archive) and the ASan/UBSan build (overflow-checked, debug-asserted Rust). Exact coverage is pinned: 452 desktop and 146 GUI sessions, 241 inside and 1,424 outside pointer states, 5,254 cursor-drawn states, 151 scrolls, 1,143 ring wraps, 616 erase step-backs. |
| Mutation controls | **51/51 detected**, covering every decision and both FFI sentinels; some are detected as faults on the guard pages.<br>Excluded as observably equivalent, with reasons in the script:<br>• `x <= width` visibility: a zero-width fill only hides and re-shows the cursor<br>• more permissive cursor-overlap tests: a false hit restores identical pixels |
| Accepted host tests | The console test (`tests/host/screen-input.c`), the N6 desktop renderer test and the pointer renderer (2,179,862 assertions) PASS against the Rust decisions. `screen-input.c` needs `desktop.c` linked, because the N6 status bar calls `desktop_bar`; the historical `test-keyboard-console.py` (not in CI) does not link it. |
| Ownership, panic freedom | `vos.bin` takes all 27 decisions from `libvos_kernel.a` (link map), and `screen.o` defines none. There is no panic machinery. Every decision makes no calls and has a 0 B frame. |
| Native pixel identity (`scripts/test-screen-native.py`) | Baseline (main) and candidate kernels boot the same image and run the same script. The screens are **pixel-identical at every step**:<br>• **Plain and desktop console** at 1024x768 on SeaBIOS/OVMF, the desktop at 640x480 and the 640x400 fallback: 16 real `help` commands (the text area scrolls), typed text, 7 backspaces and the echoed result.<br>• **GUI pointer route** on SeaBIOS/OVMF, host only, with the hash-verified accepted `gui-demo`/`pointer-demo`: ready, move, click, all four screen corners, and the surface's left and right edges. Each named step also asserts the cursor's exact tip position: (512,390), (524,382), the corners (0,0), (1023,0), (1023,767), (0,767), and the surface columns 384 and 639. |
| Worker campaign `r3-g2-screen-v1` | **r1 PASS** (seal `4a157cf1…`), five jobs:<br>• containment<br>• build, and a byte-identical rebuild of kernel `958a3baa…` and archive `ac79184c…`<br>• the R3-G2 gate (751 s; the same digest in both builds; 51/51 mutants)<br>• native console/desktop identity, 6/6 cases pixel-identical at every step, against an in-sandbox baseline rebuilt from the frozen `screen.c` (sections equal main's) with source-built `vsh`/`hello`/`rixie` at the accepted identities<br>**r2 PASS** (seal `56eb9c74…`): every Build smoke group except C4.1/C4.2a (repository metadata; run on the host), plus the QEMU runtime job. `ci-boot`, which includes both graphics gates, took 785 s, and the build tmpfs peaked near 58 MB. |
| CI equivalence (host) | **GCC 13.4 on the exact committed source/test tree: every hosted Build smoke step PASS (39/39)**, including the R3-G1 and new R3-G2 steps. The R3-G2 step took 599 s with 4 CPUs, and its peak output was 41.6 MB. |

### Disclosed development iterations

- **R3-G1 gate link change:** the R3-G1 gate's checked host library now also links the screen glue, because its renderer tests compile `screen.c`. This is in the source commit, and no check changed.
- **Pointer-motion fix (`test(gfx): deliver pointer moves in bounded steps`):** the first native GUI route sent 3,000-pixel moves as single events and lost part of the motion identically on both kernels. Its step names (corners, surface edges) were therefore inaccurate even though the screens matched. Moves are now bounded and the cursor positions asserted. Worker r1 ran the console/desktop cases with the earlier version of the script, whose console steps are unchanged.
- **Equivalent mutants and coverage:** the gate's coverage was strengthened (aimed pointer and cursor-edge operations, all byte values, GUI size edges) until every boundary mutant was detected deterministically. The coverage pins were updated at each step.

### Not tested and pre-existing

- `scripts/test-ps2-pointer-native.py` still fails identically on main, in its gdb execution proof at `pointer_idle` (recorded in R3-G1). It is **not** reported as passing. The GUI/pointer pixel-identity route above is a separate, passing check.
- The GUI pointer route runs only on the host. The accepted `gui-demo`/`pointer-demo` ELFs are outside the worker's bind set, and a source build of `gui-demo` does not reproduce the accepted identity.
- There is no physical-hardware acceptance.

## Resources

- **Rust decisions:** 0 B frames and no calls.
- **C entry frames** (frozen → production, kernel flags, GCC 15). Most of these were frameless leaves that now call into Rust and keep the stack aligned:

  | Function | Frozen | Production |
  | --- | --- | --- |
  | `screen_fill` | 64 | 72 |
  | `screen_putc` | 8 | 24 |
  | `screen_erase_previous` | 0 | 8 |
  | `screen_gui_available` | 0 | 8 |
  | `screen_gui_begin` | 0 | 24 |
  | `screen_pointer_move` | 8 | 24 |
  | `screen_pointer_begin` | 0 | 8 |
  | `screen_pointer_end` | 0 | 8 |

  `screen_glyph`, `screen_init`, `screen_desktop_begin` and `screen_gui_row` are unchanged, and `screen_begin_input` drops 24 → 8.

- **Whole-path stack depth** (linked kernels, GOT-indirect calls resolved), base → candidate:

  | Path | GCC 15 | GCC 13 |
  | --- | --- | --- |
  | `kernel_main` | 12,336 = 12,336 | 12,336 = 12,336 |
  | `gui_syscall` | 6,352 = 6,352 | 6,352 = 6,352 |
  | `screen_putc`, `serial_putc` | 168 → 208 | 176 → 192 |
  | `screen_init` | 488 → 528 | 512 → 528 |
  | `screen_gui_begin` | 96 → 128 | 80 → 112 |
  | `screen_pointer_move`, `screen_pointer_begin` | 56 → 80 | 64 → 96 |
  | `screen_fill` | 88 → 96 | 72 → 80 |
  | `screen_erase_previous` | 88 → 96 | 88 → 96 |
  | `screen_gui_available` | 0 → 16 | 0 → 16 |
  | `screen_pointer_end` | 24 → 32 | 24 → 32 |
  | `screen_desktop_begin` | 248 → 256 | 240 → 224 |
  | `screen_status` | 200 → 208 | 208 → 192 |
  | `screen_glyph` | 80 = 80 | 80 = 80 |

  **Exception requested:** the C entry-frame growth above, and these whole-path increases (at most +40 B, on paths far below the unchanged deepest kernel paths). The single Rust decision that had a 16 B frame (`pointer_state`) was reformulated per axis to 0 B before publication.
- **Image:** loaded memory is unchanged (GCC 15: 970,752 B, end `0x1ED000`; GCC 13: 966,656 B, end `0x1EC000`). The ELF file grows by 2,936 B (GCC 15) and 2,648 B (GCC 13).
- No heap, DMA, I/O or new state.

## Performance (R-series performance policy)

The method is paired hosted medians: 10 alternating runs × 9 samples × 20,000 operations on one CPU. The frozen and production modules are built with kernel-equivalent codegen (`-O2 -fno-builtin -U_FORTIFY_SOURCE`) against the production archive, drawing into a 1024x768 framebuffer in RAM. These are hosted micro-benchmarks, not BIOS/UEFI or write-combining framebuffer latency, which is NOT TESTED.

| Workload | Frozen C | Rust decisions | Change |
| --- | --- | --- | --- |
| Console output with scrolling (per byte) | 4,963 ns | 4,963 ns | −0.0% |
| Output plus erase (per pair) | 317.2 ns | 309.5 ns | −2.4% |
| Glyph | 170.3 ns | 165.5 ns | −2.8% |
| 24x8 fill | 113.7 ns | 112.2 ns | −1.4% |
| Pointer move with cursor redraw | 3,980 ns | 3,094 ns | −22.3% |

These numbers are for the final archive (`ac79184c…`, identical to the worker build). The pointer gain comes from computing the cursor clip extents once per hide/show instead of re-evaluating the loop bounds every iteration. An earlier run, before `pointer_state` was reformulated, measured the fill at +1.3% (below the 5% threshold). The final run shows no regression on any path.

## Evidence

The evidence directory `r3-g2-screen-rust/` contains:
- `final-result.json`
- `differential.json`
- `native-pixel-identity.json`
- `performance.json`
- `stack-depth.txt`
- `reproducible-build.txt`
- `worker-containment.json`
- genuine screenshots of the scrolled desktop console, the erased line, and the GUI cursor at the screen corner and at the surface edge

`retained-captures.json` indexes every retained screenshot by SHA-256. The evidence is QEMU verification, host differential testing and static analysis only; there is no physical-hardware acceptance.
