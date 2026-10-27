# R0-A/R0-B publication review

2026-10-27. Ready for PR review; **not owner-accepted or merged**.
Accepted main remains `67a525e2cbcb13215c4d17cdfd0e3740ad40ff16` (PR #19).
This review implements no additional kernel feature and starts neither R1 nor USB.2.

## Provenance and publication structure

One publication branch, `feat/r0-rust-kernel-publication`, retains these original
commits without rebasing, amending or duplicating either change:

- R0-A assessment: `689260f6c93c97e9d062e7829f078a13e92350be`, based on
  `cbe9f9202ca9a45aaafa57ec45fc01baeb347bc1`.
- R0-B implementation: `8e2f247dacf7a5d2a4f37db7c73df191f6c4cc1b`, based on
  accepted main `67a525e2cbcb13215c4d17cdfd0e3740ad40ff16`.

A history-preserving merge joins the assessment with R0-B and the publication
documentation/evidence. Their original branches and separate checkouts remain
untouched. The assessment branch adds only its report; it does not restore an
older source tree over main. Both original commits remain reachable in the PR.

The [R0-A assessment](r0a-rust-kernel-assessment.md) is now included in the same
published tree as the [R0-B report](r0b-rust-kernel-foundation.md). Its statement
that R0-B was not yet implemented is historical assessment-time wording, not the
current implementation state. All 30 R0-A source identities match both its
assessed commit and the accepted pre-R0-B code baseline. Original observations
are preserved under [r0a-observations](r0-publication-review/r0a-observations/).
Its PATH query failure and inconclusive PIC cfg query remain disclosed.

All 48 original R0-B files, including its dated report, manifest, failure records
and authentic serial evidence, remain byte-identical. This publication adds a
current review instead of rewriting that evidence or changing its original
inventory. README, STATUS and ROADMAP receive only current-state/build-prerequisite
corrections. The original master plan and historical milestone records are unchanged.

## Source review findings

No kernel correctness blocker was identified in the bounded R0-B implementation.

| Requirement | Actual source/binary finding |
| --- | --- |
| Real Rust execution | `#![no_std]`, externally visible `extern "C"` function; C supplies runtime values, actually calls it, checks the result, then logs PASS. Archive origin, C call relocation and 26-byte linked function agree. |
| C/Rust ABI | Two `uint64_t`/`u64` arguments and a same-width return; fixed unsigned wrapping/rotation semantics; no references, shared records, heap or mutable global state cross the boundary. |
| Higher-half code generation | Separate `x86_64-unknown-none` staticlib, kernel code model, static relocations, no red zone, abort panic, no host target or userspace runtime. |
| Panic | One handler forwards a static NUL-terminated message through a documented nonreturning C FFI declaration. Existing C panic disables interrupts and retains halt/debug-exit behavior. |
| Image and memory protection | Unchanged assembly entry, linker assertions and validator. Five page-tiled RX/RW/RX/R/RW loads; physical end `0x1cc000`, below 4 MiB. |
| Runtime dependencies | No undefined final symbols, allocator symbols, dynamic loader, TLS, loadable unwind tables or runtime relocations in normal and deliberate-panic images. No FP/SIMD instructions were found. |
| Test-only runtime detail | The deliberate `panic!` fixture extracts more prebuilt `core` code and a statically resolved GOT in RW/NX data. Production has no GOT. This disclosed test detail requires no dynamic linker or policy relaxation. |
| Compatibility | 119 files under `user/` and `docs/abi/` match accepted main. Existing syscall/loader/process/driver implementations, capability boundaries and ABI v0-v5 remain unchanged. |
| Make integration | Archive is a separate link input after C objects, not a `.d` include. Existing source/flag/no-op dependency and reproducibility evidence remains valid. |

The production probe is stackless; its Rust panic bridge adjusts the stack before
calling C. Normal computation has no implicit bounds/division/overflow panic.
The sole unsafe Rust block passes immutable static bytes to the existing C panic.
No assertion, linker permission or relocation validator was relaxed for publication.

## Final-tree verification

Fresh outputs use a new independent `vos-r0-publication` checkout under
`build/r0-publication-review/`; retained R0-A/R0-B outputs were not overwritten.
The installed compiler, sysroot and GRUB EFI producer were reused without installing
dependencies or reviving legacy admission/attestation code.

| Fresh check | Result |
| --- | --- |
| `make ARCH=x86_64 verify` with explicit existing bare-metal rustc | PASS: mixed C/Rust link, Multiboot2 and original layout validator |
| Existing layout-validator synthetic controls | 22/22 PASS, including intentional malformed-image rejection |
| Same bare-metal archive linked to hosted C under ASan/UBSan | 4,132 pairs + 2 fixed vectors PASS; Rust itself is not sanitizer-instrumented |
| Production archive compared with original R0-B | Byte-identical |
| Normal and panic PT_LOAD geometry/bytes compared with original R0-B | All five loads identical in each image |
| Normal/panic symbol, section, runtime and instruction audit | PASS |
| BIOS interactive pointer GUI, keyboard a/b, motion, primary down/up, q, apps, hello, Rixie help/exit, shell echo/exit | PASS; pointer released, two console restorations, vsh process reclaimed |
| UEFI equivalent interactive check | PASS |
| BIOS deliberate Rust panic | PASS: checked probe, actual Rust panic, C message, failure marker, exit 35 |
| UEFI deliberate Rust panic | PASS: same expected failure path and exit 35 |

Each native run independently checks the serial seed/salt/result against the
64-bit contract. QEMU uses q35/TCG, qemu64, 128 MiB, no network or host disks;
OVMF gets a private fresh variable store. Serial evidence represents actual VOS
execution, not generated output or a graphical mockup.

Different checkout paths change non-loaded C DWARF and therefore the full ELF
digest. They do not change any loaded byte or permission. The new production
ELF SHA-256 is `24a46322834c090a0c1da9d7631bdb8427a608d928f463859bcb44f85293019e`;
the original R0-B file hash remains valid for its original build. The archive is
identical across checkouts. Production load footprint and the original 1,416-byte
file-growth comparison remain unchanged.

The original ten-case R0-B evidence is reused for max-CPU, incorrect-result,
incremental rebuild and i386 checks because the relevant source is unchanged;
fresh native checks supplement it rather than replacing it. No unrelated
historical kernel/storage/network stress campaign was run. GitHub CI is a separate
publication result: inspect the live PR's build-smoke and secret-history checks;
local passing tests do not predict or replace those checks.

Evidence: [review checks](r0-publication-review/review-checks.json),
[source/binary review](r0-publication-review/source-binary-review.json),
[load identities](r0-publication-review/load-identity.json),
[native outcomes](r0-publication-review/native-results.json),
[BIOS interaction](r0-publication-review/serial-bios-interactive.txt),
[UEFI interaction](r0-publication-review/serial-uefi-interactive.txt),
[BIOS panic](r0-publication-review/serial-bios-panic.txt),
[UEFI panic](r0-publication-review/serial-uefi-panic.txt), and
[manifest](r0-publication-review/manifest.json).

## Preservation and limitations

The interrupted D1 draft, retained synthetic symlink, historical controllers,
failed/interrupted results, original worktrees/stashes and evidence remain
preserved. USB.1 is unchanged in its separate checkout. No legacy funding,
reservation retirement, cleanup, controller bootstrap or executor attestation
was performed. The new checkout uses ordinary disk checks, scoped Git changes
and focused verification under the owner's replacement policy. Preparation's
missing fresh-clone Git identity was resolved with the existing maintainer
identity for that command only; it caused no implementation or history change.

The [preservation audit](r0-publication-review/preservation.json) checks 3,856
historical entries across 13 evidence roots, the original 919-file workspace
inventory and 972-file USB inventory, original refs/index, 13 worktrees and two
stashes, plus both clean R0 feature checkouts. No differences were found.
Fresh build outputs total 138,112,663 logical bytes; Linux free space remained
1,008,513,716,224 bytes. See [resource measurements](r0-publication-review/resources.json).
These are ordinary measurements, not new legacy charges or funding.
The [documentation check](r0-publication-review/documentation-validation.json)
validates UTF-8, Markdown fences, internal destinations/anchors and the unchanged
original R0-B manifest; final counts are recorded there.

This is QEMU-only verification, not physical-hardware readiness, production
security or a substantial C-to-Rust migration. The nightly/sysroot is a pinned
reproduction input, not a promise that arbitrary future Rust code, stack use or
runtime dependencies fit the kernel. Future code still needs unsafe/FFI,
instruction, lifecycle and image-budget review.

## Recommended first R1 candidate — not implemented

Migrate the existing `inside(uint64_t size, uint64_t off, uint64_t len)` predicate
in [arch/x86_64/elf.c](../../arch/x86_64/elf.c), used by the real ELF validator:
`off <= size && len <= size - off`. It is a small pointer-free bounds helper,
not another demonstration probe or a rewrite of executable loading.

Preserve exact short-circuit behavior through a scalar C ABI. In particular,
`off > size, len == 0` must reject; saturating subtraction alone is not equivalent.
Test zero, exact-end, one-past-end, UINT64_MAX and overflow-adjacent triples against
the original C expression; run affected malformed-ELF tests and BIOS/UEFI launch
regressions, inspect the real migrated call and measure code/resource impact.
This helper is on a security-sensitive path despite its small size. It requires
separate R1 authorization and review; no R1 source was changed here.
