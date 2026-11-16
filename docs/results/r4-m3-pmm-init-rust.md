# R4-M3: Rust PMM initialization and memory-map classification

**These all PASS:**
- A whole-state differential against the frozen accepted `pmm.c` over eight memory layouts, comparing the result and every byte of the instance, both bitmaps and all inputs. It is identical in a release build linked with the kernel archive and in an ASan/UBSan build.
- All 74 mutation controls are detected.
- Five classes of undefined input are refused with a documented code and without a stray write.
- Static memory-model, constant-binding, ownership, reachability and panic-freedom checks pass.
- The accepted R4-M1, R4-M1H, R4-M2 and R4-V1 gates reproduce their accepted digests.
- A native QEMU observer shows that every `pmm_init_in` call on BIOS and UEFI at 128 MiB and 2 GiB is the Rust code, and that each replays through the frozen C to an identical state.
- The PMM, VMM, kmap, user, process, IPC, mp and stress self-tests pass, as do the network and USB.2-C2 DMA-teardown suites.
- Worker run r3 passes (r1 stopped on a job-definition defect; see Verification).

No stack, performance or behavioral exception is requested. Physical hardware is **NOT TESTED**.

## Scope

`pmm_init_in` and its four helpers (`count_bits`, `set_frames`, `clear_frames`, `protect`) move from `arch/x86_64/pmm.c` into safe Rust (`kernel/rust/mm/pmm_init.rs`, `#![forbid(unsafe_code)]`) behind the unchanged `pmm.h` ABI. Every defined behavior is preserved:

- **Error precedence:** `EINVAL` (bitmaps, frame count) before `EMMAP` (map, entry version and size).
- **Free pass:** whole frames inside available entries, the 1 MiB clamp, inward rounding, clipping at the limit, and per-entry ignored bytes above it.
- **Protect pass:** every non-available entry wins, rounded outward and clipped; an overflowing entry extends to 2^64.
- **Reservation pass:** the table is a union; overlaps are harmless.
- **R4-M1H:** the bounded low-memory clear is unchanged.
- **Final state:** the usable and free counts, the hint and the `READY` state are written last.

The accepted file is frozen as `tests/reference/pmm-r4m3.c` (the blob of main `5af23469`).

**Unchanged in C:** the kernel instance and its `.bss` bitmaps, `pmm_init` and the other wrappers, zeroing, poisoning and physical memory. No allocation policy, ownership rule or recovery behavior changes.

## Safety argument

- **No reference over C memory.** The adapter (`arch/x86_64/rust/pmm_init.rs`) forms no reference, slice or `Cell` over C memory. Each access to the instance, bitmaps, boot information, map entries or reservation table is one raw read or write, issued when the core asks, in the C's order. A value is read again wherever a write could have changed it, so every overlap the C defines has the C's result.
- **One simplification.** A run of frames is set or cleared a word at a time instead of bit by bit. Nothing is read between the bit operations of one run, so the stored words are the same.
- **Where the C is undefined, the Rust refuses**, and writes nothing outside the instance and its bitmaps:
  - a NULL or misaligned instance or bitmap: `EINVAL`;
  - a bitmap clear that zeroes the map pointer: `EMMAP`;
  - an instance whose `avail` field is overwritten before a protected range is cleared: `ECORRUPT`;
  - a reservation count past the table: `EINVAL`.
- **Bounds and layouts.** A bitmap word is touched only below `frames / 64`. Loader data is read without alignment assumptions. The 20 byte offsets and 11 constants are compiled against the C headers.
- **No panic machinery** is linked. `pmm_init_in` reaches only `boot_mmap_entry`; the core's helpers are inlined.

## Verification

`scripts/test-pmm-init-rust.py` (CI step and worker job `pmminitrust`):

- **Differential.** `tests/host/pmm-init-differential.c` places all objects in one guard-paged world at a fixed address:
  - eight layouts: separate objects; identical bitmaps; overlapping bitmaps; a bitmap on the instance's counters; the map inside a bitmap; the table inside a bitmap; the instance inside its inputs; the instance on the boot information;
  - garbage starting state, random and boundary memory maps (overflowing, empty, unaligned, around 1 MiB, the limit and 2^64), malformed headers and reservation tables;
  - 22,905 cases, digest `bb798065b3e127e4` in both builds, zero mismatches.
- **Mutation controls.** 74 named mutants of the core and the adapter (precedence, every pass, rounding, clipping, ignored bytes, counts, state, offsets, bounds) are each detected:
  - 64 by a mismatch;
  - 10 by a fault or an overflow abort (for example an instance that is not zeroed). A build failure never counts as detection.
- **Equivalent mutants, excluded and documented:** the two redundant low-memory protections dropped one at a time; an empty entry not skipped; `>=` for `>` at the ignored-bytes and clip tests; the `start >= limit` test in `protect` (the clipped end makes such a range empty).
- **Native.** `scripts/test-pmm-init-native.py` uses gdbstub breakpoints to record each of the 22 calls per boot: the kernel's own initialization from the firmware map, and every self-test call (16 successful, 3 `EMMAP`, 3 `EINVAL`). Each replays through the frozen C to the same result, instance and bitmaps.
- **Accepted gates.** The R4-M1H gate now checks the bounded clear in the Rust core and runs its regression and its 20 mutation controls against the Rust. It reproduces its accepted digest `aa6fef2bf2d8baac` with 3/3 negative-control faults.
- **Host test.** The firmware-lease host test links the init module.

**Worker** (campaign `r4-m3-pmm-init-v1`, source commit `9c1b0daa`, owner-approved root procedure):

- **r1, STOP.** My derived job file gave the R4-M3 gate the output directory of the R4-M1H gate in the same run, which refused it. The R4-M3, R4-M1 and R4-M2 gates had passed. The STOP is sealed and preserved.
- **r3, PASS.** This is the owner-approved repair: the same jobs with unique output directories. All 12 jobs passed; the kernel and Rust archive rebuilt byte-identical.
- **r2 (CI equivalence)** was not run, by owner decision; the exact-head GitHub CI covers it.

## Resources

**Stack** (every kernel function compared against main):

| | GCC 15.2 | GCC 13.4 |
| --- | --- | --- |
| Functions whose frame or path changed | `pmm_init`, `pmm_init_in` only | the same |
| `pmm_init_in` frame / whole path | 72 / 80 -> 72 / 72 B | 88 / 96 -> 72 / 72 B |
| Deepest chain through it | 1,920 -> 1,912 B | 1,936 -> 1,912 B |
| `kernel_main` / exception paths | unchanged | unchanged |

Out of line, the two core helpers made the Rust frame 120 B, so they are marked `#[inline(always)]`. The loaded image layout and the text, data and bss sizes are identical with both compilers.

**Initialization time** (paired, 10 alternating runs x 9 samples, one pinned CPU, frozen C against the kernel archive of this commit):

| Map | GCC 15: C -> Rust | GCC 13: C -> Rust |
| --- | --- | --- |
| BIOS, 128 MiB | 77.2 -> 27.4 us (-64.5%) | 76.9 -> 27.4 us (-64.3%) |
| BIOS, 2 GiB | 554.7 -> 186.2 us (-66.4%) | 555.6 -> 186.3 us (-66.5%) |
| 64 entries, 80 reservations | 522.3 -> 142.6 us (-72.7%) | 522.7 -> 142.6 us (-72.7%) |
| Self-test instance, 16,384 frames | 34.8 -> 11.4 us (-67.2%) | 34.7 -> 11.4 us (-67.1%) |

The difference is the word-at-a-time runs. It runs once per boot.

## Not tested

- Physical hardware and real firmware maps.
- Indirect calls in the static stack analysis (a limit of the method, unchanged).

## Evidence

`docs/results/r4-m3-pmm-init-rust/` contains the gate summary and mutation table, the native summary, the stack, image and performance measurements, the worker summary (r1 and r3), QEMU serial excerpts, a UEFI screenshot of the candidate kernel, and `SHA256SUMS`. The complete receipts and retained outputs are in the maintainer ledger (campaign `r4-m3-pmm-init-v1`).
