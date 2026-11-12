# R4-V1: Rust virtual-memory and user-range validation decisions

**These all PASS:**
- A three-way differential (frozen C, production C forwarder, Rust export) over 273,257,460 cases, identical in a release build linked with the kernel archive and in an ASan/UBSan build.
- Every caller in `vmm.c` and `usercopy.c` is byte-identical to the frozen accepted files.
- All 52 mutation controls are detected.
- The static scalar-only, constant-binding, ownership and panic-freedom checks pass.
- The unchanged R4-M1, R4-M1H and R4-M2 PMM gates still pass.
- Native QEMU: the VMM, kmap, user and scheduler self-tests on BIOS and UEFI at 128 MiB and 2 GiB, the four VMM fault images, IPC, mp and stress, and the network and USB.2-C2 DMA-teardown suites.
- Worker runs r1 and r2 pass.

Three stack and performance items need an owner decision (see [Exceptions for the owner](#exceptions-for-the-owner)). Physical hardware is **NOT TESTED**.

## Scope

Four pure decisions move to safe Rust (`kernel/rust/mm/vmm_check.rs`, `#![forbid(unsafe_code)]`):

| C function | File | Decision |
| --- | --- | --- |
| `canonical(va)` | `vmm.c` | lower or upper canonical half |
| `check_va(va)` | `vmm.c` | non-canonical, upper half, alignment, then null guard and top page, in that order |
| `check_nonleaf(e, level)` | `vmm.c` | PS first (reserved in a PML4E, large page elsewhere), then any bit outside the non-leaf set |
| `user_range(uptr, len)` | `usercopy.c` | `[uptr, uptr + len)` inside the user range, without overflow |

The C functions keep their names and signatures and forward to four scalar exports (`vos_rust_vmm_canonical`, `vos_rust_vmm_check_va`, `vos_rust_vmm_check_nonleaf`, `vos_rust_user_range`, declared in `rust_kernel.h`). The accepted files are frozen byte-identical as `tests/reference/vmm-r4v1.c` and `usercopy-r4v1.c` (the blobs of main `61cc985c`).

**Unchanged in C:**
- page-table walks and modification, `vmm_map`, `vmm_unmap`, `vmm_lookup` and the audit;
- frame ownership and PMM integration;
- CR3 and INVLPG handling;
- `user_writable`, `copy_to_user`, `copy_from_user`, including their zero-length shortcuts;
- exception and interrupt handling.

No capability, paging policy or error code changes.

## Safety argument

- **Scalars only.** The core takes and returns integers. It has no pointer, reference, slice, cast, static or mutable state. The adapter (`arch/x86_64/rust/vmm_check.rs`) has no unsafe block; its only `unsafe` tokens are the four `no_mangle` attributes.
- **No panic path.** No indexing, no division by a variable, and the one subtraction is a wrapping subtraction that the preceding comparison keeps from wrapping. The linked kernel contains no panic machinery.
- **Widths.** `size_t` and `uint64_t` are both 64-bit (asserted at compile time); `level` is a C `int` and a Rust `i32`.
- **Constants.** The Rust constants repeat `memlayout.h` and `vmm.h`. The gate parses both headers and requires all 20 to match, and pins both headers by hash.
- **Callers.** The gate undoes the four forwarding bodies and the include line and requires the result to equal the frozen files exactly, so the order and behavior of every caller is the accepted C.

## Verification

`scripts/test-vmm-check.py` (CI step and worker job `vmmcheck`):

- **Differential.** `tests/host/vmm-check-differential.c` compares, for every case, the frozen C, the production C function and the Rust export:
  - every address within two pages of each layout boundary, every page of the null guard, every one- and two-bit address;
  - every combination of the low 12 entry bits, with and without NX, with none, each or all of bits 52-62, three address fields and nine levels including `INT_MIN` and `INT_MAX`;
  - user ranges around each boundary with fifteen length classes (zero, exact fit, one over, `2^63`, `2^64 - 1`);
  - 10,000,000 seeded iterations over the full 64-bit space, uniform and boundary-biased.
- **Callers.** `user_writable`, `copy_to_user` and `copy_from_user` are compared on their result and on the exact sequence of `vmm_lookup` calls, against a stub that refuses a chosen page.
- **Coverage.** Every result class of every decision and caller is reached.
- **Mutation controls.** 52 named mutants of the core, the adapter and the C forwarders (bounds, order, masks, results, truncation, swapped arguments, removed zero-length shortcuts) are each detected by a mismatch.
- **Linked kernel.** The four decisions come from the Rust archive, `vmm.o` and `usercopy.o` only reference them, `vmm_map`, `vmm_unmap`, `vmm_lookup`, `user_writable` and `copy_from_user` call them, and each is a leaf without calls or stack use.

**Worker** (isolated campaign `r4-v1-vmm-validate-v1`, source commit `d8dfc72b`, owner-approved root procedure):

- **r1, 12 jobs, PASS:** containment, build, reproducibility (kernel and Rust archive byte-identical on rebuild), this gate, the three PMM gates, both native PMM observers, the native VMM, user and process images, pmm-exhaust, mp and stress, the network suites and the USB.2-C2 DMA matrix.
- **r2, 9 jobs, PASS:** the CI workflow steps (three Freestanding groups and QEMU smoke), including the new step.
- **Disclosure:** the tree was clean at the commit for all of r1 and at r2's admission. Documentation drafts (`docs/` and `README.md` only) were added to the worktree while r2 was running; no source, test, script or workflow file differed from the commit.

Two mutations are equivalent and excluded: swapping the non-canonical and upper-half checks (no address is both), and a mask instead of `%` for alignment.

## Resources

**Stack** (every kernel function compared, base `61cc985c` tree against the candidate):

| | GCC 15.2 | GCC 13.4 |
| --- | --- | --- |
| Functions whose frame or path changed | `vmm_unmap` only | none |
| `vmm_unmap` entry frame | 168 -> 184 B | 168 -> 168 B |
| `vmm_unmap` whole path | 448 -> 464 B | unchanged |
| Deepest boot path (`kernel_main`) | 11,984 B, unchanged | 11,936 B, unchanged |
| Exception path | 4,592 B, unchanged | 4,560 B, unchanged |
| Deepest chain through `vmm_unmap` (conservative) | 3,032 -> 3,048 B of a 16 KiB stack | 3,016 B, unchanged |

The four Rust decisions use no stack. The loaded image layout and the text, data and bss sizes are identical with both compilers.

**Performance** (paired, 10 alternating runs x 9 samples, one pinned CPU, kernel code-generation flags; frozen C against production C plus the kernel archive; ns per operation). The page-table paths run on a process arena without INVLPG, so they measure the software path only.

| Path | GCC 15: C -> Rust | GCC 13: C -> Rust |
| --- | --- | --- |
| direct `canonical` | 2.19 -> 2.20 (+0.5%) | 2.21 -> 2.21 (-0.3%) |
| direct `check_va` | 3.41 -> 4.02 (+17.9%, +0.61 ns) | 3.42 -> 4.03 (+17.9%, +0.61 ns) |
| direct `check_nonleaf` | 1.92 -> 2.71 (+41.0%, +0.79 ns) | 1.93 -> 2.70 (+39.7%, +0.77 ns) |
| direct `user_range` | 2.65 -> 2.53 (-4.6%) | 2.70 -> 2.53 (-6.4%) |
| `vmm_map` + `vmm_unmap`, tables present | 71.84 -> 72.61 (+1.1%, +0.77 ns) | 66.21 -> 78.75 (+18.9%, +12.54 ns) |
| `vmm_map` + `vmm_unmap`, three tables created | 1000.8 -> 987.8 (-1.3%) | 977.3 -> 994.4 (+1.7%, +17.1 ns) |
| `vmm_lookup` | 68.26 -> 65.24 (-4.4%) | 66.56 -> 68.67 (+3.2%, +2.12 ns) |
| `vmm_map` refusals (two calls) | 6.09 -> 9.57 (+57.2%, +3.48 ns) | 7.30 -> 9.53 (+30.5%, +2.23 ns) |
| `user_writable`, 1 byte | 72.09 -> 70.16 (-2.7%) | 72.23 -> 72.68 (+0.6%, +0.45 ns) |
| `user_writable`, 16 pages | 1114.0 -> 1068.3 (-4.1%) | 1081.7 -> 1111.5 (+2.8%, +29.8 ns) |
| `user_writable` refusals (two calls) | 4.06 -> 5.92 (+46.1%, +1.87 ns) | 4.37 -> 5.68 (+30.0%, +1.31 ns) |
| `copy_from_user`, 64 bytes | 181.1 -> 177.9 (-1.8%) | 179.4 -> 182.5 (+1.7%, +3.10 ns) |
| `copy_to_user`, 64 bytes | 173.6 -> 170.3 (-1.9%) | 173.2 -> 176.1 (+1.7%, +2.97 ns) |
| `copy_from_user`, 4096 bytes | 227.4 -> 225.1 (-1.0%) | 226.0 -> 229.1 (+1.4%, +3.12 ns) |
| `copy_to_user`, 4096 bytes | 229.9 -> 225.0 (-2.1%) | 227.7 -> 230.6 (+1.3%, +2.93 ns) |

The cause is the call boundary: the C inlined these decisions, and each is now a call into the Rust archive (two `check_va` and six `check_nonleaf` calls per map and unmap pair). The GCC 13 map and unmap figure was investigated because it exceeds 5%: two isolated reruns reproduced it (+12.5 and +12.4 ns) while GCC 15 stayed at +0.2 and +1.1 ns, and both compilers emit the same calls. No safety check was removed or reordered to recover time.

## Exceptions for the owner

None is approved. Each is for this exact source and these toolchains.

1. **Stack, GCC 15:** `vmm_unmap` entry frame 168 -> 184 B and whole path 448 -> 464 B (two more callee-saved registers held across the new calls). The deepest chain through it grows from 3,032 to 3,048 B of a 16 KiB stack; the deepest boot and exception paths are unchanged. No change with GCC 13.
2. **Performance, direct decisions and refusal paths (both compilers):** `check_va` +17.9% (+0.61 ns); `check_nonleaf` +41.0% / +39.7% (+0.79 / +0.77 ns); `vmm_map` refusals +57.2% / +30.5% (+3.48 / +2.23 ns for two calls); `user_writable` refusals +46.1% / +30.0% (+1.87 / +1.31 ns for two calls); `canonical` +0.5% with GCC 15 (+0.01 ns).
3. **Performance, successful paths:** GCC 13 `vmm_map` + `vmm_unmap` with tables present +18.9% (+12.54 ns) and with table creation +1.7% (+17.1 ns), `vmm_lookup` +3.2% (+2.12 ns), `user_writable` +0.6% / +2.8% (+0.45 / +29.8 ns), user copies +1.3% to +1.7% (+2.9 to +3.1 ns); GCC 15 `vmm_map` + `vmm_unmap` with tables present +1.1% (+0.77 ns).

There is no behavioral exception.

## Not tested

- Physical hardware.
- Timing inside QEMU or with real TLB invalidation: the benchmark is hosted, and INVLPG is not executed in it.
- Indirect calls in the static stack analysis (a limit of the method, unchanged).

## Evidence

`docs/results/r4-v1-vmm-check-rust/`: the gate summary and mutation table, the native summary, stack, image and performance measurements, the worker summary, QEMU serial verdict excerpts, a UEFI screenshot of the candidate kernel, and `SHA256SUMS`. The complete receipts and retained outputs are in the maintainer ledger (campaign `r4-v1-vmm-validate-v1`).
