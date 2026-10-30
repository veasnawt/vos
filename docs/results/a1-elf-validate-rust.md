# A1: Rust ELF validator

**Verified for PR review, not accepted or merged.** The production kernel's
`elf_validate()` and `elf_strerror()` are now freestanding `no_std` Rust inside
the existing kernel archive. The original C `arch/x86_64/elf.c` is byte-identical
and is kept only as the frozen differential reference; it is no longer linked into
the kernel. The ELF loader, process creation, mapping and every caller remain the
unchanged C code. Verification is QEMU- and host-only; physical hardware is NOT TESTED.

Baseline: verified `main` `d67e3cdbf50ab7a67521fb5cc8ea81e2e0091a65` (PR #40).
Branch `feat/r1-elf-validate-rust`. Scope comes from the
[Master Plan R-series](../plans/MASTER-PLAN.md#6-r-series-rust-first-kernel-migration).
This is a migration of one existing validator. It does not start A2, change USB or
add a loader feature, and the 85% Rust / 2,000-line C+asm figures remain candidate
metrics, not accepted R5 criteria.

## Implementation

| Part | Owner | Notes |
| --- | --- | --- |
| `kernel/rust/elf/validate.rs` | Safe Rust | Pure port of `elf_validate` and the 26 message strings. Every read uses `get()` and returns 0 out of range, so there is no indexing or panic path. The 26 outcomes keep their order, precedence and text. |
| `arch/x86_64/rust/elf_validate.rs` | Unsafe FFI only | Exports the unchanged `elf.h` symbols. Const-asserts the 40-byte `struct elf_segment` and 192-byte `struct elf_image` layouts. Rejects null, misaligned and overlapping pointers with `ELF_ESIZE`. Zeroes the image on every failure and writes output fields individually, never padding. |
| `arch/x86_64/arch.mk` | Build | Removes `elf.o` from the production object list. |
| `user/user.mk`, `scripts/elfcheck.c` | Build | The host `elfcheck` tool links the Rust archive instead of `elf.c`. Linking both would define `elf_validate` twice. |
| `arch/x86_64/elf.c`, `elf.h` | Unchanged | `elf.h` is the ABI. `elf.c` is the frozen reference. |
| `vos_rust_elf_inside` | Unchanged | The R1 helper and its [evidence](r1-elf-bounds.md) are kept. |

## Acceptance matrix

| Gate | Result | Evidence |
| --- | --- | --- |
| A. Equivalence with the frozen C | **PASS.** 1,185,934 inputs, zero mismatches, in both the archive build and an overflow-checked hosted build: 1,085,934 deterministic (field, truncation, alignment, segment, note and error-boundary sweeps plus structural builders) and 100,000 random over 16 bases and 37 built seeds. Compares return code, every image field, all 26 messages and a zeroed image after every failure. Guard pages around the input are never touched. Every one of the 26 outcomes occurs. | `tests/host/elf-differential.c`, `scripts/test-elf-bounds.py`, [result JSON](a1-elf-validate-rust/differential-result.json) |
| B. Random-phase coverage | The random phase stops on a plateau rule (at least 100,000 inputs and 50,000 without a new outcome/load-count/ABI signature). 44 signatures were seen, the last at input 1,085,004, so the plateau was reached immediately and the 400,000 cap was not needed. The random phase cannot reach structural cases such as `ELF_ELOADCOUNT` on its own; the deterministic builders cover them. | [result JSON](a1-elf-validate-rust/differential-result.json) |
| C. Negative controls | **19/19 broken references detected** (18 by mismatch, `size-minimum` by a guard-page fault, which counts as detection). Mutants include inclusive/exclusive user-range and entry edges, W^X check order, note walk, image-page boundary, overlap boundary, field and phnum limits, message text and failure-zeroing. | `differential_detects_broken_references` in the [result JSON](a1-elf-validate-rust/differential-result.json) |
| D. Existing checks | **PASS.** R1 bounds 497,127 cases and 283-file validator comparison against the shell tool agree. `test-file-abi` 20 comparisons pass. Built `elfcheck` agrees with the reference tool on all 283 files. | [result JSON](a1-elf-validate-rust/differential-result.json) |
| E. Runtime | **PASS (QEMU).** The self-test kernel passes `selftest: elf 35 cases, 0 failed` and the vmm audit at 128M and 2G with all suites at 0 failed. The production Legacy BIOS ISO passes `scripts/test.sh` with the arch-test markers (empty command line, `elf: hello: ET_EXEC entry 0x400000, 3 PT_LOAD, 3 pages, VOS ABI 0`, `vmm: audit passed`, `user[hello]: Hello from VOS!`, `boot: checks passed`). The production UEFI ISO passes `halt-test.sh uefi-boot` under OVMF with the same markers. | [BIOS serial](a1-elf-validate-rust/bios-serial.txt), [UEFI serial](a1-elf-validate-rust/uefi-serial.txt), [self-test 128M](a1-elf-validate-rust/selftest-128M.txt), [2G](a1-elf-validate-rust/selftest-2G.txt) |
| F. Production-link ownership | **PASS.** `vos.bin` defines `elf_validate` (0xa90 B) and `elf_strerror` (0x1a B) from `libvos_kernel.a(...cgu.0.rcgu.o)`. The link map has no `elf.o` entry. The three C call sites resolve to the Rust symbols. The panic/fmt symbol gate finds 0 forbidden symbols. | [a1-facts](a1-elf-validate-rust/a1-facts.txt), [baseline-facts](a1-elf-validate-rust/baseline-facts.txt) |
| G. ABI | **PASS.** `elf.h` and all userspace, syscall, scheduler, process and capability files are unchanged. Layout asserts: segment 40 B/align 8, image 192 B/align 8 with the offsets in `elf.h`. | const asserts in `arch/x86_64/rust/elf_validate.rs` |
| H. Resource ceilings | **Within the stated ceilings, one at the limit.** Stack frame is 488 B (`sub $0x1b8` plus pushes) against the 512 B ceiling; the C version used 296 B plus pushes. Loaded image growth is exactly **4,096 B**, the 4 KiB ceiling, not under it. The cause is page tiling: the text segment grew from 0x42000 to 0x43000 bytes. True symbol growth is +968 B (`elf_validate` 0x79a to 0xa90 and `elf_strerror` 0x18 to 0x1a, plus the message table). All five segment addresses move up by one page or less. | [a1-facts](a1-elf-validate-rust/a1-facts.txt), [baseline-facts](a1-elf-validate-rust/baseline-facts.txt) |

## Failed attempts and gaps found

The harness had compile and logic errors during development (an unused symbol, a
forward declaration, an unchecked mutation base, a guard-page read on unreadable
sizes) that were fixed before the recorded runs. The negative controls then found
real blind spots in the first sweeps: an inclusive `USER_END` mutant survived
because the exact `vaddr + memsz == USER_END` boundary was not swept, and one
W^X-order mutant was equivalent (its difference was unreachable). Boundary sweeps
were added for sizes and sums at the limits, image pages, entry edges, overlap
rounding, `phoff` against size and note walk sizes, and the equivalent mutant was
replaced with a real swap. The first random phase also hit its cap before the
plateau rule replaced the per-outcome rule. All fixes were made before the final
runs; no recorded run was retried to obtain a pass.

## Remaining limits

- Physical hardware is NOT TESTED. QEMU evidence does not establish firmware or
  hardware behavior.
- The Rust code is not ASan-instrumented. Memory safety rests on `get()`-based
  reads, the guard-page arena, the overflow-checked hosted build and the symbol gate.
- The loaded image is exactly at the 4 KiB growth ceiling because of segment
  page rounding. Any further kernel growth in the same segment is measured separately.
- The loader (`elf_load`, mapping and process setup) is still C.
