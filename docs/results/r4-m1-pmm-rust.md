# R4-M1: Rust PMM read-only checks

**These all PASS:**
- a guard-paged differential against the frozen accepted `pmm.c` (9,256,573 probes, 0 mismatches) and 42 mutation controls;
- ownership, panic-freedom and ABI checks;
- native QEMU observation of the Rust checks with frozen-C replay on BIOS/UEFI at 128 MiB and 2 GiB;
- the PMM-exhaustion, mp and stress self-test images, and the network DMA-teardown suites;
- worker runs r1 and r2, the full host `make test`, and host GCC 13 CI equivalence.

Physical hardware is **NOT TESTED**. Two items need an explicit owner decision under the R-series policy (see [Exceptions](#exceptions-for-the-owner)):
- stack growth: `pmm_free_in` +32 B and `pmm_check_in` +16 B whole path;
- per-call performance regressions of 0.6–1.2 ns.

No merge without owner approval.

The baseline is accepted main `f5d470dffbb2cfdc8467d20912589f64bbdc7a69` (Phase A CI split merged). The branch `codex/r4-m1-pmm` starts at `16b2fc2`, whose tree `4397c0ea` is identical to main's. The source commit is `a221407c` (tree `5cf4d8c0`).

## Scope

`pmm_free_check_in`, `pmm_frame_state_in` and `pmm_check_in` move from `arch/x86_64/pmm.c` into safe, panic-free `no_std` Rust (`kernel/rust/mm/pmm.rs`, `#![forbid(unsafe_code)]`). The `pmm.h` C ABI, `struct pmm`, every error value and the order of the checks are unchanged:
1. readiness;
2. alignment and range;
3. allocatable;
4. allocated;
5. counters;
6. word by word, allocated outside avail, allocatable below 1 MiB, free below the hint;
7. totals.

The bit count keeps the C's clear-lowest-bit loop (no `popcnt`).

**Unchanged in C:** `struct pmm` and the `.bss` bitmaps, `pmm_init_in`, `pmm_alloc_in`, `pmm_free_in`'s mutation, `pmm_reserve_in`, the kernel wrappers, zeroing and poisoning, physical memory and interrupts. `pmm_free_in` and `pmm_free_frame` still call the free check. Allocation policy and reservation rules are untouched; R4-M2 is not started.

### Safety boundary

The adapter `arch/x86_64/rust/pmm.rs` is the only unsafe code. It has 4 `unsafe` blocks, each with a `SAFETY` justification:
- three reads of the caller's `struct pmm` (the C dereferences it unconditionally too);
- one formation of the two bitmap slices.

The core never touches a bitmap on its own. Each check reads instance fields where the C reads them (through an `Instance` trait), makes every decision that needs no bitmap word, and only then asks for the bitmaps. The adapter forms them as slices of exactly `frames / 64` words, and only for an instance within the documented bounds that `pmm_init_in` establishes for every READY instance. That condition (`instance_ok`) is evaluated in the safe core as one branch-free predicate:
- `frames` is a nonzero multiple of 64, at most `PMM_MAX_FRAMES`;
- both bitmap addresses are non-null and 8-byte aligned.

The slices live only for that call. The kernel is single-CPU and runs PMM code with IF=0 (interrupts only in the idle thread's `sti; hlt`), and the read-only checks call nothing that writes. The core also reads every word through bounds-checked access, so it has no panic path.

**Valid-input contract.** Every instance `pmm_init_in` can produce, including any later corruption of its counters, hint or bitmap contents (the corruptions `pmm_check_in` exists to detect), gets the accepted C result. Every input on which the C decides before reading a bitmap also gets the C result, whatever the other fields hold: unready or failed states, unaligned or out-of-range addresses (including the C's 2^64 wrap of `frames * PAGE_SIZE`), and zero or sub-word frame counts.

**Documented divergence.** A READY instance outside the documented bounds is one `pmm_init_in` never produces: `frames` not a nonzero multiple of 64 up to `PMM_MAX_FRAMES`, or NULL or misaligned bitmaps. Wherever the C would read a bitmap for such an instance, the Rust refuses without any bitmap access: `ECORRUPT` from the free check and the audit, `FRAME_UNAVAILABLE` from the frame state. The C would dereference NULL, read misaligned memory, or read past the documented `PMM_WORDS(frames)` words. The differential runs such instances with real, guarded buffers anyway. There, 677 of 1,161 C results differ from the Rust refusal, and these are reported and not hidden. No in-tree caller builds an instance by hand: the kernel instance and the self-tests' synthetic instances all come from `pmm_init_in`.

## Verification

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/pmm-r4m1.c` is byte-identical to main's `pmm.c` (blob `00778036`, manifest `tests/reference/pmm-r4m1.json`). It is not linked into the kernel. |
| Differential (`scripts/test-pmm-rust.py`, new Build smoke step in the core group) | `tests/host/pmm-differential.c` runs the frozen C (every public symbol renamed `ref_`) and the production Rust on the same instances. Every bitmap is flush against PROT_NONE guard pages; "no access" cases point the bitmaps at a PROT_NONE page or NULL. The classes are:<br>• instances built by the frozen `pmm_init_in`/alloc/free/reserve from memory maps (256 to 262,144 frames, holes, reservations, overflowing entries, a refused map), probed as they evolve, with double frees, a reserved frame, and exhaustion with the hint at `PMM_WORDS(frames)`<br>• uninitialized, failed and arbitrary states<br>• the 1 MiB boundary (frames 255/256)<br>• alignment and range, including 2^64<br>• frame and bitmap-word boundaries<br>• counter, hint and bitmap corruption: allocated outside avail, allocatable below 1 MiB, free below the hint, totals, `free >= usable`, `free > usable`, `UINT64_MAX`<br>• early refusal without bitmap access<br>• instances outside the documented bounds<br>• 20,000 seeded instances, every frame probed exhaustively for the 6,999 with 64–1,024 frames<br>After every call, the return values must match and neither side may change the instance or its bitmaps.<br>**9,256,573 probes, 27,777,895 checks, 0 mismatches.** Digest `a4f7a96ad9362c6b` is identical in the release build (kernel archive) and the ASan/UBSan build (overflow-checked, debug-asserted Rust). Every return code each function can produce is reached, and the gate requires each class. |
| ABI | `struct pmm` is 64 B, align 8, with the field offsets 0/8/16/24/32/40/48/56. These are asserted at compile time in the adapter and checked against C `offsetof` by the harness. The error and frame-state values are compared through the differential. |
| Mutation controls | **42/42 detected**: 29 by result mismatches, 13 by a fault on a guard or PROT_NONE page or a checked-build abort. They cover every check, the order of the checks, early bitmap access, the bit and word arithmetic, every bound in `instance_ok`, NULL/alignment, and the adapter's field, slice-length and argument wiring.<br>Excluded as observably equivalent, with reasons in the script:<br>• `free > usable` dropped (a recount never has more free than usable frames)<br>• the zero-frames bound alone (zero frames never reach a bitmap request)<br>• the core's slice-length check (the adapter forms exactly `frames / 64` words)<br>• `frame_state`'s range check weakened (the bounds-checked word read already answers `FRAME_UNAVAILABLE` without access) |
| Ownership, panic freedom | `vos.bin` takes all three checks from `libvos_kernel.a` (link map), and `pmm.o` defines none (it calls `pmm_free_check_in`). There is no panic machinery in the kernel. The core contains no `unsafe`. All three checks make no calls. |
| Native observation (`scripts/test-pmm-native.py`) | The VOS_TEST image boots under the QEMU gdbstub on SeaBIOS and OVMF at 128 MiB and 2 GiB. Every trap lands on the Rust symbol taken from the archive. The observer records the first 300 free checks, 300 frame-state calls and 24 full audits (the kernel instance, complete 2 x 32 KiB bitmaps) per boot. Every recorded call replays identically through the frozen C (`tests/host/pmm-replay.c`, itself built with ASan/UBSan). Results seen: free check OK, ENOTREADY, EALIGN, ERANGE, EPROTECTED, ENOTALLOC; frame state FREE and ALLOCATED; audit OK. Each image then runs to `VOS-TEST: PASS` (isa-debug-exit 33) with the pmm-synthetic, pmm-kernel, vmm and kmap self-test lines. The production images boot to `boot: checks passed` on both firmwares; [UEFI screenshot](r4-m1-pmm-rust/screen-uefi.png). |
| Self-test images (worker) | pmm-exhaust; mp and stress under qemu64, 2 GiB and `-cpu max`: **7/7 PASS** with the accepted markers. |
| DMA teardown | The accepted network suites (ARP/IPv4/UDP, virtio-net, TCP and focused TCP), whose boots call `pmm_check_in` on the kernel instance after every pool release: **PASS** (worker r1). |
| Full native suite (host) | `make ARCH=x86_64 test` on the candidate: **PASS** (27 min 51 s), covering every self-test image, fault case, isolation fixture and scheduler/IPC/mp/stress suite. It needs about 1.4 GB of build space, so it cannot run in the worker's 256 MiB tmpfs. |
| Worker campaign `r4-m1-pmm-v1` | **r1 PASS** (seal `132cb26e…`), nine jobs:<br>• containment<br>• build, and a byte-identical rebuild of kernel `b8ced121…` and archive `679402a8…` (GCC 15.2 in the sandbox)<br>• the R4-M1 gate (23 s; the same digest; 42/42)<br>• native observation (4/4 and both screenshots)<br>• the self-test images (7/7)<br>• net-a, vnet-full and net-b<br>Retained 1,809,054 B. BIOS screenshot `a3ec54b7…` and UEFI `a42cd600…` (the published one).<br>**r2 PASS** (seal `36e3e80e…`, 2,244 s): every Build smoke step in the three Freestanding groups except C4.1/C4.2a/USB.3-A (repository metadata; run on the host), plus the QEMU runtime job. `ci-boot`, which includes the new gate, took 176 s; `ci-gfx` took 642 s. Retained 1,659,442 B.<br>The root steps ran exactly as approved for r1 and r2. The plan is `9dae0dba…`; BOOT `14438e12…` and `payload.py` `c6e088cd…` are unchanged. |
| CI equivalence (host) | **GCC 13.4 on the exact source commit (`git archive` of `a221407c`): every hosted Build smoke step in the three groups PASS (39/39)**, including the new PMM gate (24 s) and the changed firmware-lease step (15 s). `Record tool versions` is informational on the host. The first attempt failed at C4.1 only because the archive export had no repository metadata (`git ls-files` exit 128). It is recorded, not hidden. C4.1 to the lease step passed after the export was given read-only git metadata for the same commit. The QEMU executor of C3b.2 ran in worker r2, not on the host. |

`scripts/test-usb-firmware-lease.py` includes `pmm.c`, but defines its own `boot_mmap_entry`, so it cannot link the kernel archive. It now links a host build of the production Rust checks (overflow-checked), and its Build smoke step passes `--rustc`. Its 595 checks and 12/12 mutants pass. The other hosted USB tests already link the kernel archive.

### Disclosed development iterations

- **Validation cost:** the first adapter tested the bounds as a chain of separate conditions. Rewriting them as one branch-free predicate in the safe core cut 6–9 instructions per check (free check 61 → 55, frame state 43 → 37) without changing a result (same digest).
- **Audit frame:** Rust's `count_ones` (SWAR) made `pmm_check_in` faster but gave it a 56 B frame. The C's clear-lowest-bit loop is kept instead (32 B). Other structurings did not reach the C's 16 B.
- **Harness defects found and fixed before any result was used:**
  - The first native observer kept the entry breakpoint armed across the return.
  - The first replay overflowed its token array, so 9 fields went into 8 slots. The replay is now built with ASan/UBSan.
  - The first deterministic instance (64 frames, built by `pmm_init_in`) exposed the finding below.
- **Equivalent mutant replaced:** `frame >= frames` weakened to `>` in the frame state is not observable (see above). It was replaced by an early-bitmap-access mutant, which is detected.

### Finding (pre-existing, not changed)

The accepted `pmm_init_in` clears frames [0, 256) whatever `frames` is. So for an instance of 64, 128 or 192 frames, which it accepts, it writes past `PMM_WORDS(frames)` words. The guard pages caught this on the first run. No in-tree caller is affected: the kernel uses `PMM_MAX_FRAMES` and the self-tests use 16,384 frames. `pmm_init_in` is out of R4-M1's scope; it is recorded for R4-M3 in the [inventory](../plans/r-series-inventory.md).

## Resources

**Rust frames** (GCC-independent): `pmm_free_check_in` 0 B, `pmm_frame_state_in` 0 B, `pmm_check_in` 32 B. The frozen C, built with kernel flags, is 0/0/16 B.

| Whole-path stack depth (linked kernel) | GCC 13 base → candidate | GCC 15 base → candidate |
| --- | --- | --- |
| `pmm_free_in` | 0 → **32** | 0 → **32** |
| `pmm_check_in` | 16 → **32** | 16 → **32** |
| `pmm_free_frame` | 256 → 256 | 272 → 272 |
| `pmm_alloc_frame`, `pmm_reserve_frame` | 272, 256 → unchanged | 288, 272 → unchanged |
| `exception_handler` (every trap, IRQ and system call) | 4,560 → 4,560 | 4,592 → 4,592 |
| `kernel_main` | 11,936 → 11,936 | 11,984 → 11,984 |

These are the only two functions whose whole-path depth grows, and they grow by at most 32 B; no recursion appears.

- `pmm_free_in`'s own frame grows from 0 to 24 B. The C inlined the free check, and the call now crosses a language boundary.
- Thread and kernel stacks (16 KiB) and the 4 KiB IST stacks are unaffected in practice: no PMM function runs on an IST stack.

**Image:** `vos.bin` shrinks by 3,120 B (GCC 13) and 3,080 B (GCC 15). Defined text shrinks by 36 B and 39 B. The load layout is unchanged (end `0x1ec000` / `0x1ed000`).

**Performance:** paired host benchmark, 10 alternating runs x 9 samples on one pinned CPU, `-O2 -fno-builtin -U_FORTIFY_SOURCE`. The C side is the frozen reference; the candidate is production `pmm.c` plus the kernel archive.

| Path | GCC 13 (C side) | GCC 15 (C side) |
| --- | --- | --- |
| `pmm_check_in`, full 1 GiB instance | 333.0 → 340.0 µs (+2.1%) | 363.5 → 345.0 µs (−5.1%) |
| `pmm_free_check_in` | 4.22 → 4.80 ns (**+13.8%**) | 3.59 → 4.81 ns (**+34.0%**) |
| `pmm_frame_state_in` | 3.30 → 3.92 ns (**+18.8%**) | 3.03 → 3.94 ns (**+29.8%**) |
| `pmm_free_in` + `pmm_alloc_in` | 12.27 → 14.98 ns (**+22.1%**) | 12.26 → 14.90 ns (**+21.5%**) |

The checks themselves are a handful of instructions. The extra 0.6–1.2 ns per call is the bounds validation required before forming slices (about 10 straight-line instructions) plus one slice bound check. On the free path, `pmm_free_in` now calls the check instead of inlining it. Investigated alternatives are listed in [Disclosed development iterations](#disclosed-development-iterations); dropping the validation is not an option under the safety boundary.

## Exceptions for the owner

Under the R-series policy, owner approval is required for each of these. Each would be for this exact source only.
1. **Stack:**
   - `pmm_free_in` entry frame 0 → 24 B, whole path 0 → 32 B;
   - `pmm_check_in` frame and whole path 16 → 32 B;
   - both on GCC 13 and GCC 15.
2. **Performance:** the per-call regressions in the table above.
   - `pmm_free_check_in` +13.8% / +34.0%;
   - `pmm_frame_state_in` +18.8% / +29.8%;
   - the free+alloc path +22%;
   - all are 0.6–2.7 ns absolute.
3. **Documented divergence:** the explicit refusal of READY instances outside the documented bounds (see [Safety boundary](#safety-boundary)). This is recorded for acknowledgement, since it applies only to instances `pmm_init_in` cannot produce.

## Not tested

- Physical hardware: **NOT TESTED**.
- `make test-uefi` (the UEFI matrix) was not run in full. UEFI coverage here is the native observer's two OVMF boots of the self-test image (128 MiB and 2 GiB), the OVMF production boot and screenshot, and the accepted UEFI network suites.
- The native observer samples the first 300/300/24 calls per boot, not every call. The remaining calls run unobserved to the self-tests' own verdict.
- The pre-existing `scripts/test-ps2-pointer-native.py` gdb-proof failure (recorded in R3-G1) is unrelated and is not reported as passing.

## Evidence

`docs/results/r4-m1-pmm-rust/`: the gate summary and mutation table, the native observation summary, stack/image/performance measurements, worker and CI-equivalence summaries, the UEFI screenshot, and `SHA256SUMS`. The complete worker receipts and retained outputs are in the maintainer ledger (campaign `r4-m1-pmm-v1`).
