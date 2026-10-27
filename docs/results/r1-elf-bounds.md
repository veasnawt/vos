# R1: migrate the real ELF bounds predicate to Rust

2026-10-27. Locally implemented and verified; pending owner review. No push,
PR or merge. Baseline: `0291b1ab8f7d7d491bd2f276efa59a46b638d400`, the owner's
merged PR #20. The separate `feat/r1-elf-bounds` checkout preserves USB.1,
the R0 checkouts and interrupted D1 work unchanged.

The complete [R0-B report](r0b-rust-kernel-foundation.md) and
[publication review](r0-publication-review.md) were reviewed before implementation.
Their original local/pending wording describes earlier checkpoints; R0 is now
merged. This report records the subsequent R1 candidate without rewriting them.

## Exact production change

The existing private C `inside(size, off, len)` function in
[elf.c](../../arch/x86_64/elf.c) now forwards to:

```rust
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_elf_inside(size: u64, off: u64, len: u64) -> i32 {
    (off <= size && len <= size - off) as i32
}
```

The private C declaration is:

```c
int vos_rust_elf_inside(uint64_t size, uint64_t off, uint64_t len);
```

The hosted fixture statically asserts a 32-bit C `int`. The result is exactly
0 or 1. Rust short-circuit evaluation does not evaluate `size - off` if
`off > size`. In particular, an invalid offset with zero length remains false;
saturating subtraction would not be an equivalent replacement on its own.
No pointer, allocation, mutable state, hardware operation or new unsafe block
is introduced by this function.

The kernel still calls `inside` at the real program-header-table bounds check
inside `elf_validate`. The `-ffreestanding` kernel always selects the Rust call;
it cannot select the hosted reference under the accepted compiler flags.
The rest of the parser, field limits, order/reasons of rejection, loader,
process lifecycle and executable ABI are unchanged.

This reuses R0's existing `no_std` static library, bare-metal target, kernel code
model, static relocation, no-red-zone flags, abort panic and C panic bridge.
There is no new crate, target, allocator, build framework or toolchain download.

## Host linking boundary

`make user-test` and the normal Make-built `elfcheck` define `VOS_ELF_USE_RUST`
and link **the same `libvos_kernel.a` used by the kernel** after the C objects.
Non-PIE host linking is explicit because the bare-metal archive uses static
kernel relocations. The archive's existing panic bridge resolves to a hosted
fatal stub in `scripts/elfcheck.c`: print the unexpected panic and exit 2,
never invent a validation result. File input/output and the existing hosted C
allocation are not kernel runtime dependencies.

Existing standalone host/compiler invocations also build `elf.c` directly in
the shell, app-catalog, bootstrap, GUI and pointer tests. Converting those
historical orchestration scripts would unnecessarily expand R1 and impose a
Rust-archive dependency on standalone C tools. Therefore, hosted builds without
`VOS_ELF_USE_RUST` retain the exact original C predicate as an explicitly
documented reference. They keep their existing command line and output format.
There is no duplicate C implementation in the freestanding kernel path.

This is a deliberate compatibility boundary, not evidence that a C-only checker
tests Rust. The new test driver rejects a C-reference checker supplied as the
primary tool. It independently compares:

1. The actual bare-metal Rust predicate, the original C expression and a wide
   `__uint128_t` addition oracle over the full unsigned domain.
2. The complete Rust-backed and reference validators under ASan/UBSan: return
   code and every `elf_image` field, ignoring unspecified structure padding.
3. Primary Make-built and standalone reference CLI exit status, stdout and stderr.

## Behavior and negative checks

| Check | Result |
| --- | --- |
| Boundary grids, exhaustive small triples, seeded and derived edge triples | 497,127 PASS; exact 0/1, zero length, exact end, one past end, off > size, UINT64_MAX and overflow-adjacent cases |
| Same vectors with Rust `-C overflow-checks=yes` | 497,127 PASS; no panic/underflow |
| Complete validator differential, ASan/UBSan | 283 files PASS: 197 accepted, 86 rejected; all result fields agree |
| Primary Rust vs standalone C checker output | 283/283 identical |
| Deliberate deny-all predicate, host-only negative fixtures | Both bounds vectors and valid-hello acceptance detect it; expected exit 1 |
| C-reference tool passed as primary Rust checker | Rejected before creating output; expected exit 2 |
| Existing `make user-test` | 220 PASS, 0 failures; 68/72 checker comparisons agree, with the same four expected build-time-only differences |
| Existing file ABI constants | 20 independent comparisons PASS |
| Existing kernel layout synthetic controls | 22/22 PASS |

ASan/UBSan instrument the hosted C fixture/validators, not the freestanding
Rust object. No sanitizer-wide Rust coverage is claimed. The hosted differential
includes existing suite/malformed files, explicit truncated/offset variants and
128 deterministic single-bit mutations; acceptance of payload-only mutations
is valid where the unchanged parser rules allow them.

See [host results](r1-elf-bounds/host-results.json),
[overflow-checked vectors](r1-elf-bounds/checked-bounds.txt),
[existing ELF tests](r1-elf-bounds/existing-elf-tests.txt) and
[mode rejection](r1-elf-bounds/reference-mode-rejection.txt).

## Real kernel execution and firmware checks

The C ELF object contains an `R_X86_64_PLT32` call relocation to
`vos_rust_elf_inside`; the final static link resolves it. See the
[call site](r1-elf-bounds/elf-call.txt) and
[function disassembly](r1-elf-bounds/rust-function.txt).
The production Rust function is 18 bytes at `0xffffffff8012fee0`, stackless and
integer-only. LLVM uses a branchless subtraction/carry test plus a masked
comparison; hardware wrap on a rejected offset does not evaluate an invalid
Rust-language subtraction or change the Boolean result. Overflow-checked tests
also pass. No expected range or rejection rule was changed to match codegen.

Fresh SeaBIOS and OVMF runs use q35/TCG, qemu64, 128 MiB, no network or host disk,
and a private fresh UEFI variable store:

| Native path | BIOS | UEFI |
| --- | --- | --- |
| Existing ELF selftest, including load/run/destroy, rollback and rejection | 35 assertions PASS; 69 corrupted images rejected for the intended reasons | Same PASS |
| Filtered TCG execution trace at the actual Rust predicate | 78 execution records at `0xffffffff8015f1b0` | 78 execution records at the same selftest symbol |
| Existing default selftest final exit | PASS, exit 33 | PASS, exit 33 |
| Normal pointer GUI a/b/q, apps, hello, Rixie help/exit, restored shell echo/exit | PASS | PASS |

The unchanged default selftest includes the existing boot prerequisites; no
storage disk, network stress, large-RAM campaign or special stress case was run.
The trace covers the predicate's execution, not just symbol presence or static
translation. Normal application runs restore console ownership twice, release
the GUI surface and reclaim vsh. No hang, leak assertion or lifecycle failure
was observed. Historical GUI/bootstrap/stress campaigns were not rerun or
reclassified as fresh R1 verification.

[Native outcomes](r1-elf-bounds/native-results.json),
[BIOS trace](r1-elf-bounds/trace-bios-rust.txt),
[UEFI trace](r1-elf-bounds/trace-uefi-rust.txt),
[BIOS ELF log](r1-elf-bounds/serial-bios-elf.txt) and
[UEFI ELF log](r1-elf-bounds/serial-uefi-elf.txt) preserve authentic evidence.
Serial/execution records are appropriate for this nonvisual kernel change;
there is no new graphical capability or screenshot claim.

## Binary, ABI and performance impact

Production and native-selftest images pass the original linker assertions and
five-segment validator. No unsupported relocation, undefined final symbol, GOT,
TLS, dynamic loader, loadable unwind table or FP/SIMD instruction was found.
No linker/boot/paging rule or application authority changed.

| Production measurement | R0-B | R1 |
| --- | ---:| ---:|
| Page-rounded text / data / BSS (`size`) | 212,992 / 220 / 618,304 bytes | unchanged |
| Physical reservation | `0x100000..0x1cc000` | unchanged |
| PT_LOAD count/permissions | 5; RX/RW/RX/R/RW | unchanged |
| ELF file including debug data | 1,022,640 bytes | 1,022,040 bytes |
| C `elf_validate` symbol | 1,903 bytes | 1,946 bytes |
| New Rust predicate | absent | 18 bytes |

The C call boundary adds 43 bytes to the validator, plus the 18-byte Rust helper;
existing padding absorbs them. The full ELF's 600-byte reduction includes
non-loaded debug/path differences and is not a runtime-memory optimization.
Two independently built R1 production kernels and Rust archives are byte-identical.
All 118 application/runtime/public-ABI files are unchanged; `user/user.mk` changes
only host-checker linking. ABI v0-v5 definitions and implementation contracts
remain frozen. [Binary audit](r1-elf-bounds/binary-audit.json).

An optimized, nonsanitized hosted whole-validator comparison used the same hello
input, GCC -O2, five paired rounds of 200,000 validations and alternating order.
Median C-reference time was 267.95 ns/validation; Rust-backed time was 250.82 ns.
No slowdown was measured in this sample; this is neither a kernel latency
measurement nor a general speedup claim. Conditions, raw samples and summary are
[recorded](r1-elf-bounds/benchmark-conditions.json). No optimization was added
based on this small sample. The benchmark fixture initially failed strict
compilation at a signed/unsigned size comparison; its failed build is preserved.
The fix casts only after rejecting negative/short lengths; warnings remain errors.
It changed no kernel code or acceptance criteria.

## Reproduction and review boundary

Use the existing build prerequisites and bare-metal Rust toolchain. All output
roots below must be new; no `make clean` or historical controller is involved:

```bash
rustc_path="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
make ARCH=x86_64 BUILD=build/r1-reproduce/kernel USER_BUILD=build/r1-reproduce/user \
  ELFCHECK=build/r1-reproduce/elfcheck RUSTC="$rustc_path" user-test
python3 -I -B scripts/test-elf-bounds.py \
  --archive build/r1-reproduce/kernel/rust/libvos_kernel.a \
  --elfcheck build/r1-reproduce/elfcheck --user build/r1-reproduce/user \
  --out build/r1-reproduce/focused
python3 -I -B scripts/test-elf-bounds-native.py --out build/r1-native-reproduce \
  --rustc "$rustc_path" --grub-efi-dir /path/to/verified/grub/x86_64-efi
```

The existing read-only GRUB EFI producer used locally is identified in the R0-B
report. QEMU traces have a 4 MiB per-file bound and a 120-second process timeout;
only owned QEMU processes are stopped. Actual commands remain in the build root.
The final image uses normal production flags; tracing is host test instrumentation.

Build smoke gains a focused host-check step after its existing `make user-test`;
it uses that job's actual kernel archive and primary tool. GitHub CI is **NOT RUN**
for this local-only candidate. No push, PR or merge is authorized by this report.

The [inventory](r1-elf-bounds/changed-files.txt),
[manifest](r1-elf-bounds/manifest.json),
[kernel/media/application identities](r1-elf-bounds/products.json),
[preservation audit](r1-elf-bounds/preservation.json) and
[resource measurements](r1-elf-bounds/resources.json) accompany the candidate.
Historical source/evidence, the retained D1 symlink, original references/index,
13 worktrees and two stashes remain preserved. No old reservation, ceiling or
funding record was changed; ordinary disk checks were used under owner policy.

Remaining limits: QEMU-only execution, a GNU/Linux x86_64 non-PIE primary host
checker, a deliberately retained hosted C reference requiring continued
equivalence checks, and compiler/sysroot-specific code-generation evidence.
This migrates one real bounds predicate, not the full ELF parser. No USB.2,
additional helper migration, loader redesign or new milestone was started.
