# R1 performance comparison and publication review

2026-10-27. Publication candidate, pending owner acceptance and merge.
Accepted main: `0291b1ab8f7d7d491bd2f276efa59a46b638d400` (merged PR #20).
Original R1: `f67ebd968169560979fc1d5eefbc7ca6402a6910`.

**Finding:** R1 is a genuine, correct Rust-backed ELF bounds check within its
tested scope. The local timing data does not establish a general performance
win or regression. Small whole-validator differences change direction across
runs and overlap observed variation. No performance acceptance threshold or
production optimization is introduced.

## Correctness and claim review

The production ELF validator calls `vos_rust_elf_inside(size, off, len)` in the
existing `no_std` archive. Its scalar C ABI is `uint64_t,uint64_t,uint64_t -> int`
matching Rust `u64,u64,u64 -> i32`, with exactly 0/1 results. Source short-circuiting
preserves `off <= size && len <= size - off`, including rejecting `off > size`
when `len == 0`. The optimized branchless Rust subtraction uses processor flags;
it is not a source-level underflow or a changed acceptance rule.

Review covered the actual C call relocation, linked Rust code, original native
execution traces, overflow-checked vectors, host checker linkage and hosted C
reference. No production correctness discrepancy was found. The normal Make-built
`elfcheck` uses the same kernel archive; standalone hosted C builds explicitly
retain the original reference. These modes are not interchangeable as evidence:
the focused test rejects a C-only tool supplied as the primary Rust-backed checker.

All 47 original R1 files, its commit, [report](r1-elf-bounds.md), manifest,
authentic logs, expected negative controls and failed benchmark-fixture record
are preserved byte-for-byte. The old five-pair valid-hello-only timing sample
remains an observation of that harness. It had no scalar-call control, narrower
inputs and fewer environment controls; it cannot establish a language-wide or
physical-VOS speed advantage. Its raw ELF-file reduction includes debug/path
differences and is not a runtime-memory saving. No such stronger claim is adopted
by this review.

## Environment and comparable builds

- Host: Intel Core i5-4570, nominal 3.20 GHz, exposed through WSL2/Hyper-V;
  Linux `6.18.54.1-microsoft-standard-WSL2`, x86_64, four available vCPUs.
- The dedicated benchmark driver/child alone was pinned to allowed CPU 3.
  The CPU was not exclusive. No affinity of another process, governor, power,
  frequency, host security or VM setting was changed.
- GCC 15.2.0, GNU ld 2.46; Rust 1.100.0-nightly
  (`2e2b193f8ada105f27608b7be81c293e0d7292cb`, LLVM 23.1.1), the existing
  `nightly-2026-09-03` bare-metal compiler/sysroot.
- No sanitizers in timing. ASan/UBSan correctness checks ran separately.
  No QEMU timing or physical VOS performance measurement is used.

The C objects use kernel-comparable optimization/architecture flags, identically
for both variants:

```text
-std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin
-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables
-mno-sse -mno-mmx -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only
-fcf-protection=none -fno-stack-clash-protection -fno-lto
```

Debug metadata is omitted. Non-PIE GNU linking (`-fno-pie -no-pie -fno-lto`)
permits the actual bare-metal Rust archive's static relocations at host addresses.
The Rust archive is unchanged from R1: `x86_64-unknown-none`, kernel code model,
static relocation, opt-level 2, abort panic, no red zone or vector/FP use, no LTO.
Full compiler/sysroot/flag identities are in [rust-inputs.txt](r1-performance/rust-inputs.txt).
The archive SHA-256 is `abb3773f1b21e291a2eb1943f9a5a7e697f151f1ca79bcf2113ae9bfbfeb692b`.

The original parser is compiled from the exact `arch/x86_64/elf.c` bytes in the
accepted pre-R1 commit, not a rewritten approximation. Current `elf.h` is checked
against that commit. The integrated variant compiles the actual R1 `elf.c` and
links the kernel archive. Only exported parser names are renamed with compiler
definitions to let both coexist in one hosted process. The old bounds expression
remains normally inlined inside the original parser; the R1 parser performs its
real direct Rust call. Both outer validator calls have the same harness overhead.

## Method and optimization controls

Two complete runs use a byte-identical benchmark executable (SHA-256
`b7d7a14d7ccd814fd3d1ce59312b4af4fda9ce8dce5fea96279740bdfe9545d2`).
A metadata-only driver addition records Rust build inputs in the second run;
timed machine code, flags, seed and inputs are unchanged. Both runs are retained.
Each has 15 samples per series, for **30 samples per reported series**.
Order rotates/alternates, with untimed warmup before each sample.

Comparison A uses the same runtime-seeded, shuffled 256-vector array (6 KiB;
108 valid, 148 invalid). It covers ordinary valid ranges, invalid ranges,
zero/exact-end/one-past cases, UINT64_MAX and overflow-adjacent cases.
Each timed sample executes **4,194,304 operations**. No input generation or
file I/O is timed. C and Rust results are checked against each other and an
independent 128-bit oracle before timing.

Both ABI variants use the same non-specialized indirect-call loop; disassembly
confirms the calls remain. The C helper is compiled separately and is noinline/
noclone; Rust cannot inline across the C boundary because LTO is off. An additional
inline-C loop preserves the old expression and shows the importance of call
context. Volatile scalar input reads prevent repeated vectors being hoisted;
checksums are consumed outside the timed loops and must agree. The call-control
function performs a small XOR expression, so it measures loop + call + minimal
work, **not pure call latency**. Neither it nor inline-C may be subtracted to
manufacture a precise standalone call cost: instruction scheduling and codegen
also differ. The indirect scalar loop is not the production direct-call path.

Comparison B executes the entire in-memory `elf_validate` call, including output
construction and rejection, **65,536 validations per sample**. It excludes file
I/O, process loading and allocation. Four eight-position corpora cover:

1. Valid native hello/vsh/Rixie/GUI/pointer ELFs: five distinct images, with three
   repeated positions to retain the same power-of-two indexing overhead.
2. Eight truncated/out-of-file program-header tables, verified to produce the
   bounds rejection that exercises the migrated predicate.
3. Eight other malformed cases: magic/class/machine/type, future ABI note, W+X,
   invalid entry and oversized field.
4. A mix of four valid, two bounds-rejected and two other-rejected positions.

Preflight compares exact error codes and every output-image field, ignoring
unspecified structure padding. Timed checksums must also agree. Both variants
read identical memory-resident files in each comparison.

Raw wall-clock (`CLOCK_MONOTONIC_RAW`) and thread CPU-clock durations are recorded,
along with context switches. Tables show pooled wall median and full min/max,
rounded to avoid implying excessive precision. Per-run medians, CPU summaries,+p10/p90, CPU model/load, corpus hashes, commands and raw integer durations are
retained. WSL scheduling, frequency changes, cache/layout effects and other host
work remain uncontrolled; affinity does not remove those limitations.

## A: individual predicate results

Nanoseconds per operation, including the common input/loop costs:

| Variant | Median | Min-max |
| --- | ---: | ---: |
| Original C, non-inlined ABI call | 2.230 | 2.093-2.953 |
| Actual Rust, non-inlined ABI call | 2.070 | 1.969-3.528 |
| Original expression inlined in C loop | 0.965 | 0.897-1.170 |
| Minimal C ABI call control | 1.775 | 1.691-2.758 |

The ABI boundary and common loop are a substantial fraction of this tiny
operation. Rust has a lower observed ABI median in this dataset, but this is
not a general language result and does not make the old inlined production
predicate comparable to a forced out-of-line C helper. Compiler/code-shape
differences and the varied sample ranges remain relevant.

## B: complete validator results

Nanoseconds per validation, median [min-max]:

| Corpus | Original C | Rust-integrated |
| --- | ---: | ---: |
| Valid applications | 262.45 [248.40-316.87] | 259.26 [243.82-473.23] |
| Bounds rejection | 45.60 [42.11-63.85] | 45.20 [42.04-65.55] |
| Other rejection | 90.83 [83.92-110.84] | 90.65 [83.71-105.90] |
| Mixed | 148.30 [139.45-180.80] | 149.57 [137.58-191.83] |

The valid-case per-run medians were C/Rust **252.90/247.11** then
**272.69/274.41** ns. Mixed medians were **146.67/144.64** then
**154.32/161.48** ns. The direction changes. CPU-clock summaries show similar
run-to-run movement; this harness does not identify its cause.
**The small whole-validator performance differences are inconclusive.** There
is no defensible general speedup/regression claim and no basis here to alter
production code, impose a performance gate or optimize this helper further.

The original R1 sample used different hosted C flags and a narrower corpus;
its numbers are not pooled with these measurements.

## Assembly and code size

| Code | Bytes | Finding |
| --- | ---: | --- |
| Out-of-line C predicate | 19 | Conditional branch, subtraction, comparison, 0/1 result |
| Actual Rust predicate | 18 | Branchless carry/comparison combination, 0/1 result |
| Minimal call control | 10 | Separate lower-bound context, not equivalent predicate work |
| Original full C validator | 1,903 | Original predicate inlined |
| R1 C validator caller | 1,946 | Actual direct call into Rust; plus the separate 18-byte helper |

See [C helper](r1-performance/bench_c_inside.asm.txt),
[Rust helper](r1-performance/vos_rust_elf_inside.asm.txt),
[ABI loop](r1-performance/bench_predicate_abi.asm.txt),
[inline loop](r1-performance/bench_predicate_inline.asm.txt),
[original validator](r1-performance/bench_elf_c.asm.txt) and
[Rust-integrated validator](r1-performance/bench_elf_rust.asm.txt).
Code sizes agree with the original R1 analysis; kernel padding absorbs the
43-byte caller increase and 18-byte helper. Page-rounded runtime footprint,
five segment permissions and physical end `0x1cc000` remain unchanged.

## Final publication-tree checks and preservation

- Original R1 source/evidence/manifest identities: all 47 files unchanged.
- Fresh Make build, Multiboot/layout verification and existing user tests:
  220 PASS; 20 ABI constant comparisons PASS; same four expected build-time-only
  parser differences among 72 comparisons.
- Fresh focused host test: 497,127 scalar cases; 289 validator/file-output
  comparisons (203 accepted, 86 rejected); both deny-all negative controls pass.
  Six additional valid native ELFs explain the larger corpus than the
  original 283-file run; no original case was removed.
- New BIOS/UEFI normal hello boot smokes pass, including reclamation and boot
  checks. All five loaded production segments match the original R1 kernel.
  Non-loaded C debug paths account for full-file digest differences.
- The original native 35 loader assertions/69 corrupt-image rejections and 78
  Rust execution records per firmware, plus GUI/shell/Rixie checks, remain valid
  evidence of their unchanged inputs. They were not repeated as a new campaign.
- No application ABI v0-v5, syscall, loader, capability, process or DMA behavior
  changes. R0 and USB.1 checkouts, interrupted D1 work, historical failures,
  protected evidence, original worktrees and stashes remain preserved.
- README/STATUS/ROADMAP receive only current-state synchronization: R0 merged,
  R1 under review. The master plan and original dated result reports are unchanged.

[Build summary](r1-performance/build-summary.txt),
[focused results](r1-performance/focused-results.json),
[load identities](r1-performance/load-identities.json),
[smoke results](r1-performance/smoke-results.json),
[preservation audit](r1-performance/preservation.json) and
[inventory](r1-performance/changed-files.txt) accompany the candidate.
The [separate build/resource record](r1-performance/resources.json) measures
this ordinary review's output; it does not change historical reservations or
revive the discontinued controller workflow.
GitHub CI is reported on the publication PR; prior local reports' NOT RUN wording
is preserved as history rather than retroactively edited.

## Reproduction and next scope

Use the documented installed bare-metal Rust toolchain and public build inputs:

```bash
rustc_path="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
make ARCH=x86_64 BUILD=build/r1-perf/kernel USER_BUILD=build/r1-perf/user \
  ELFCHECK=build/r1-perf/elfcheck RUSTC="$rustc_path" RUST_APP=rixie \
  user-test user-vsh user-rust user-gui user-pointer
python3 -I -B scripts/benchmark-elf-bounds.py \
  --archive build/r1-perf/kernel/rust/libvos_kernel.a \
  --user build/r1-perf/user --out build/r1-perf/comparison-1
# Repeat with a NEW output name to retain both runs.
```

The driver selects an allowed CPU and pins only itself/its child, refuses an
existing output directory, and records the exact executed compiler/linker commands.
It requires existing GCC/binutils, Python and Linux timing/affinity interfaces;
there is no new dependency installation, controller or attestation workflow.
The original Make/focused/native reproduction commands remain in the R1 report.

[Raw run 1](r1-performance/run-1-raw.csv),
[raw run 2](r1-performance/run-2-raw.csv),
[pooled summaries](r1-performance/pooled-summary.json),
[environment](r1-performance/run-2-environment.json),
[corpus identities](r1-performance/corpus.json) and
[manifest](r1-performance/manifest.json) permit independent review.

Remaining concerns are the nonexclusive WSL timing environment, the hosted
reference's ongoing equivalence obligation and QEMU-only VOS qualification.
The next step is owner review/acceptance of this R1 PR. A separately authorized
USB.2 increment can then assess Rust controller/DMA/IRQ ownership, with reviewed
integration of the preserved USB.1 discovery work, which is outside this main
baseline. No R2, USB.2 or additional helper is implemented or authorized here.
