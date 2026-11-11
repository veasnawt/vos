# R4-M2: Rust PMM allocation, free and reservation transitions

**These all PASS:**
- A whole-instance differential against the frozen hardened `pmm.c`, run over guard-paged arenas in seven memory layouts.
- All 48 mutation controls are detected.
- Static memory-model checks, ownership and panic-freedom checks pass.
- The unchanged R4-M1 checks and R4-M1H initialization still pass.
- Native QEMU observation: every sampled call replays through the frozen C to an identical post-state.
- BIOS/UEFI self-tests pass, as do the network and USB.2-C2 DMA-teardown suites.
- Worker runs r1 and r2 pass.

Physical hardware is **NOT TESTED**.

Two items need an owner decision under the R-series policy (see [Exceptions](#exceptions-for-the-owner)):
- `pmm_alloc_in`'s 16 B frame;
- reserve+free on GCC 15 at +2.9%.

No merge without owner approval.

The baseline is accepted main `74c2341bd46112705f7e4be99a410883748cb17c` (R4-M1H merged). The branch `codex/r4-m2-pmm-mutate` starts at `8f883a99`, whose tree `7ff0dbb7` is identical to main's. The source commit is `b15767d6` (tree `3bb8e489`).

## Scope

These functions move from `arch/x86_64/pmm.c` into safe, panic-free `no_std` Rust (`kernel/rust/mm/pmm_transition.rs`, `#![forbid(unsafe_code)]`):
- `pmm_alloc_in`;
- `pmm_free_in`, which calls the accepted R4-M1 `pmm_free_check_in`, then performs the mutation and state update;
- `pmm_reserve_in`.

The `pmm.h` C ABI, `struct pmm` and every return code are unchanged. The accepted semantics are preserved exactly:
- `*phys = 0` is written before anything else;
- the scan starts from the hint and takes the lowest free frame;
- the corruption transitions: ECORRUPT, plus state FAILED in alloc for `free == 0`, an allocatable frame below 1 MiB, or a count with no free frame;
- ENOMEM sets the hint to `PMM_WORDS(frames)`;
- the free-count changes, and freeing lowers the hint;
- reserve keeps its precedence: readiness, alignment, range, EPROTECTED, EINUSE, then ECORRUPT (without FAILED).

No allocation policy or recovery behavior is added.

**Unchanged in C:**
- `pmm_init_in` (including the R4-M1H bound);
- the instance and its backing storage;
- the kernel wrappers: `pmm_alloc_frame` zeroing, `pmm_free_frame` poisoning, and the panic-on-ECORRUPT policy;
- physical mapping and interrupts.

`pmm.c`'s `test_bit` helper is removed, because only the moved functions used it; it would otherwise be an unused static that fails `-Werror` hosted builds. The R4-M1 Rust checks are byte-identical to main's; their hashes are pinned in `tests/reference/pmm-r4m2.json`.

## Memory model and safety argument

The C contract allows aliasing that Rust references could not express. All of these are defined C, because they alias `uint64_t` objects with each other:
- `avail` and `alloc` may be the same array, or overlap by whole words;
- a bitmap may lie on a `uint64_t` field of `struct pmm`;
- `*phys` may be one of those fields or a bitmap word.

Any `&`/`&mut` reference or slice held across a write in such a layout would be unsound in Rust. The design therefore holds **no Rust reference to C memory at all.**

- **The core.** It accesses the instance only through the `Mem` trait (`kernel/rust/mm/pmm_transition.rs`). Every method is exactly one memory access: a field read or write, a bitmap word read or write, or the output write. The core calls them one per C access, in the accepted C's source order, including reading each field again where the C reads `p->field` again.
- **The adapter** (`arch/x86_64/rust/pmm_transition.rs`) implements `Mem` with raw pointers only:
  - It never forms `&`/`&mut` references, slices or `Cell`s over the instance, the bitmaps or the output word, and never dereferences into a place reference.
  - Field addresses come from `addr_of!` / `addr_of_mut!` on the raw instance pointer.
  - Each read and write is a single `ptr::read` / `ptr::write`.
  - Raw pointers carry no aliasing assumptions, so any overlap behaves as in the C, access for access. The gate checks this statically: the adapter contains no `&*`, `&mut *`, `from_raw_parts`, `Cell`, `as_ref` or `as_mut`.
- **Validity of each access:**
  - A bitmap word is accessed only when the bitmap pointer is non-null and 8-byte aligned, and the word index is below `PMM_WORDS(frames)` for the `frames` value read for that access. That is the instance's documented storage (pmm.h; `pmm_init_in` zeroes exactly those words, so they are initialized).
  - The instance pointer and `phys` carry the same preconditions as in the C, which dereferences them unconditionally: live, aligned, initialized, and (for `phys`) writable.
  - No access is held across another, so lifetime and exclusivity reduce to the absence of concurrent access. The kernel is single-CPU, and PMM code runs with interrupts disabled (IRQs only in the idle thread's `sti; hlt`).
- **Outside the documented storage:**
  - A NULL or misaligned bitmap, or a word at or past `PMM_WORDS(frames)`, would make the C read or write memory it does not own: undefined behavior in the C. There the transition stops with ECORRUPT, makes no further access, and leaves state unchanged.
  - Every access the C makes before that point has already happened in the same order, including alloc's `*phys = 0`.
  - The only such input that reaches a bitmap is a READY instance that `pmm_init_in` never produces.
  - Unlike R4-M1's checks, these transitions do **not** require `frames` to be a multiple of 64 or at most `PMM_MAX_FRAMES`: wherever the C stays within `PMM_WORDS(frames)` words, the Rust reproduces it exactly, which the differential tests.
- **`pmm_free_in`** first calls the accepted R4-M1 `pmm_free_check_in`, whose shared references end before it returns. Only then does it perform raw-pointer mutations.
- **No undefined behavior or panic path:**
  - The core uses only wrapping arithmetic and `Option` results.
  - The checked host build (overflow checks, debug assertions and UB checks) runs the full differential with no abort.
  - The linked kernel contains no panic machinery.
  - The three functions make no calls; `pmm_free_in`'s R4-M1 check is inlined.
- **Excluded from testing, and why:** layouts where a bitmap or `*phys` overlaps the pointer fields or the `int state` (or its padding) violate C's strict aliasing (undefined behavior in the C), and an output word on `frames` lets an allocation silently invalidate the storage contract. The differential never builds these.

## Verification

| Gate | Evidence |
| --- | --- |
| Frozen oracle | `tests/reference/pmm-r4m2.c` is byte-identical to the hardened main `pmm.c` (blob `6684f3b3`, manifest `pmm-r4m2.json`). Its `pmm_free_in` calls the accepted R4-M1 Rust check, so both sides use the same check. |
| Whole-instance differential (`scripts/test-pmm-transition.py`, new Build smoke step, core group) | `tests/host/pmm-transition-differential.c` gives each side its own arena, flush against PROT_NONE guard pages, in seven layouts:<br>• separate regions (instances built by the frozen `pmm_init_in` from 256 frames up to `PMM_MAX_FRAMES`, or written directly)<br>• identical bitmaps<br>• bitmaps overlapping by whole words<br>• `alloc` starting inside `avail`<br>• `*phys` on `usable`/`free`/`hint`<br>• `*phys` on a bitmap word<br>• a bitmap on the instance's fields<br>The operations are allocation, freeing (valid, double, unaligned, half-page, out of range, protected, bitmap-word edges, 2^64), reservation, exhaustion followed by giving frames back, and reuse. The corruptions cover counters, hint, state, avail and alloc words, and allocatable frames below 1 MiB. Instances with frames not a multiple of 64, and above `PMM_MAX_FRAMES`, are included with their documented storage. After every operation, the return value, the output word and every byte of both arenas must match.<br>**503,135 operations, 0 mismatches.** The digest `897cebc925a1aa2b` is identical in the release (kernel archive) and ASan/UBSan (overflow-checked, debug-asserted Rust) builds. Every result of every transition is reached:<br>• alloc: OK, ENOTREADY, ENOMEM, and ECORRUPT with FAILED (2,630)<br>• free: OK, ENOTREADY, EALIGN, ERANGE, EPROTECTED, ENOTALLOC, ECORRUPT<br>• reserve: OK, ENOTREADY, EALIGN, ERANGE, EPROTECTED, EINUSE, ECORRUPT<br>Outside the documented storage only the Rust runs (C undefined), and it must refuse without writing: 25 NULL/misaligned cases and 7 reserves past `PMM_WORDS(frames)`. |
| Mutation controls | **48/48 detected**, 44 by mismatches and 4 by a fault or checked-build abort. They cover every check, `*phys = 0` ordering, the hint start and update, the scan bound, the free mask and lowest-bit selection, the frame number, the FAILED transitions, the ENOMEM hint, the free count, freeing's hint lowering, skipping the R4-M1 check, reserve's precedence (EPROTECTED before EINUSE) and its non-FAILED corruption, and the adapter's null, alignment and storage bounds, word offset, field wiring and output write.<br>Excluded as unobservable, with reasons in the script:<br>• the order of the free and hint updates (distinct fields, never aliased by a defined layout)<br>• reusing the first-read alloc word (nothing is written between the two reads)<br>• a saturating reserve range product (differs only for frames ≥ 2^52) |
| R4-M1 and R4-M1H unchanged | `scripts/test-pmm-rust.py` passes with the same digest `a4f7a96ad9362c6b` (42/42). `scripts/test-pmm-init.py` passes with the same digest `aa6fef2bf2d8baac`, 3/3 negative control, 20/20 mutants. Its scope check now verifies that `pmm_init_in` and its helpers are byte-identical to the frozen hardened reference, because `pmm.c` legitimately changes again. The firmware-lease test (12/12) now links the transition module too. |
| Ownership, panic freedom | `vos.bin` takes the three transitions from `libvos_kernel.a` (link map). `pmm.o` defines none of them, and its wrappers call them. There is no panic machinery. The transitions make no calls. |
| Native observation (`scripts/test-pmm-transition-native.py`) | The VOS_TEST image boots under the QEMU gdbstub on SeaBIOS and OVMF at 128 MiB and 2 GiB (worker r1). Every trap lands on the Rust `pmm_alloc_in`/`pmm_free_in` taken from the archive. Recorded per boot: 16 sampled kernel-instance allocations and frees, with the complete 2 × 32 KiB bitmaps before and after, plus every allocation and free on the self-tests' synthetic instance. Of about 6,000 calls per boot, 50 are recorded.<br>Each recorded call is replayed from the guest's pre-state through the frozen C (`tests/host/pmm-transition-replay.c`, ASan/UBSan). The result, the output word and the full post-state (struct and both bitmaps) are **identical** for every call. The replayed results include allocation OK and ENOMEM, and free OK, EALIGN, ERANGE, EPROTECTED and ENOTALLOC.<br>Each image then runs to `VOS-TEST: PASS` (exit 33) with the pmm-synthetic, pmm-kernel, vmm, kmap and user self-tests. The R4-M1 observer also passes in the same job and captures the console screenshots ([UEFI](r4-m2-pmm-transition-rust/screen-uefi.png); BIOS `3f8d75c8…` retained). |
| Native suites and DMA teardown | Worker r1, with serial verdicts in [qemu-serial-excerpts.txt](r4-m2-pmm-transition-rust/qemu-serial-excerpts.txt):<br>• the pmm-exhaust image, and the mp and stress images under qemu64, 2 GiB and `-cpu max`: **7/7 PASS** (process creation, exhaustion and reclamation through the Rust transitions)<br>• the network DMA-teardown suites (ARP/IPv4/UDP, virtio-net, TCP): PASS<br>• the **USB.2-C2 native DMA matrix**, 8/8 on SeaBIOS/OVMF: its qualification mode reserves frames through `pmm_reserve_frame` → Rust `pmm_reserve_in`, and its exercise audits the PMM after teardown<br>• the B1 boot-reservation native check (r2 QEMU job): PASS |
| Worker campaign `r4-m2-pmm-mutate-v1` | **r1 PASS** (seal `d9d47c54…`), 12 jobs on source commit `b15767d6`:<br>• containment<br>• build and a byte-identical rebuild of kernel `09bd3b59…` and archive `7ac2cfce…` (GCC 15.2 in the sandbox)<br>• the R4-M2 gate (372 s; the same digest; 48/48)<br>• the R4-M1 and R4-M1H gates (unchanged digests)<br>• both native observers<br>• the self-test images<br>• the network suites<br>• the USB.2-C2 matrix<br>Retained: 3,075,205 B.<br>**r2 PASS** (seal `7ed2dad7…`): every Build smoke step of the three Freestanding groups (`ci-boot` 651 s with the new gate) and the QEMU job. The hosted-only C4.1/C4.2a/USB.3-A models (no kernel code) are covered by exact-head CI. Retained: 1,691,239 B.<br>The root steps ran exactly as approved for r1 and r2. The plan is `aff385ad…` and the root procedure `effb4b06…`; BOOT and `payload.py` are unchanged. |

## Resources

| Whole-path stack depth (linked kernel) | GCC 13 base → candidate | GCC 15 base → candidate |
| --- | --- | --- |
| `pmm_alloc_in` | 0 → **16** | 0 → **16** |
| `pmm_free_in` | 32 → 0 | 32 → 0 |
| `pmm_reserve_in` | 0 → 0 | 0 → 0 |
| `pmm_alloc_frame` / `pmm_free_frame` / `pmm_reserve_frame` | 272 / 256 / 256, unchanged | 288 / 272 / 272, unchanged |
| `kernel_main`, `exception_handler` | 11,936, 4,560, unchanged | 11,984, 4,592, unchanged |

- **Stack:** `pmm_alloc_in` is the only function whose whole path grows. Its 16 B are two callee-saved registers, because the scan keeps `frames`, both bitmap pointers and the frame offset live. `pmm_free_in` loses the 24 B frame and 32 B path that R4-M1 had added. The wrappers' C frames are unchanged (24/8/8 B), there is no recursion, and the deepest paths are unchanged.
- **Image:** the load layout is unchanged (end `0x1ec000` / `0x1ed000`). `vos.bin` shrinks by 2,304 / 2,096 B. Defined text grows by 211 / 210 B within the same pages.

**Performance** (paired, 10 alternating runs × 9 samples, one pinned CPU; frozen C vs the kernel archive):

| Path | GCC 13 C side | GCC 15 C side |
| --- | --- | --- |
| allocate + free | 12.66 → 11.87 ns (−6.2%) | 12.40 → 11.96 ns (−3.6%) |
| long scan (2,040 full words) + free | 1,383 → 1,210 ns (−12.5%) | 1,362 → 1,222 ns (−10.3%) |
| reserve + free | 12.35 → 12.33 ns (−0.2%) | 11.57 → 11.91 ns (**+2.9%**) |

## Exceptions for the owner

1. **Stack:** `pmm_alloc_in` frame and whole path 0 → 16 B (GCC 13 and 15). No caller's whole path changes.
2. **Performance:** reserve+free with the GCC 15 C side, +2.9% (0.34 ns). This is below the 5% investigation threshold, but listed because any regression needs approval. Every other path is faster.

No behavioral exception: every defined C behavior is reproduced, and only undefined C inputs (outside the documented storage) are refused.

## Not tested

- Physical hardware: **NOT TESTED**.
- Native `pmm_reserve_in` replay: the self-test image never reserves. Reservation runs natively only in the USB.2-C1 qualification mode, exercised by the USB.2-C2 native "both" cases; in the differential it is fully covered.
- The native observer samples calls; it does not record every call.
- `make test-uefi` in full.

## Evidence

`docs/results/r4-m2-pmm-transition-rust/`: the gate summary and mutation table, the native observation summary, stack/image/performance measurements, the worker summary, QEMU serial verdict excerpts, a UEFI screenshot, and `SHA256SUMS`. The complete receipts and retained outputs are in the maintainer ledger (campaign `r4-m2-pmm-mutate-v1`).
