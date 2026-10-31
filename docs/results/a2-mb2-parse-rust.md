# A2: Rust Multiboot2 parser

**Verified for PR review, not accepted or merged.** The production kernel's `mb2_parse()` is now freestanding `no_std` Rust inside
the existing kernel archive. Its caller (`kernel.c`), the self-tests, `struct boot_info`, every constant and every error string are the
unchanged C ABI. The accepted C parser is kept only as a frozen differential reference and is not linked into the kernel.
Verification is hosted and QEMU only; physical hardware is NOT TESTED (physical VOS testing is suspended).

Baseline: `main` at `c77dabb2d84228120109c1e0f547264d99e6485e`, branch `feat/a2-mb2-parse-rust`. Scope: the owner's A2
authorization. Preflight and decisions: `mb2_parse` and its pure helper only; `boot_framebuffer`, `boot_mmap_entry`,
`boot_reserve` and the reservation logic stay C.

## Implementation

| Part | Owner | Notes |
| --- | --- | --- |
| `kernel/rust/mb2/parse.rs` | Safe Rust, no `unsafe`, no allocation, no panic path | The tag walk and every validation, in the C's order. Reads go through `get`. It reports each result to an `Out` trait instead of building the 1.6 KB output structure, so nothing large sits on the early-boot stack. The 18 error strings are byte-identical NUL-terminated statics. |
| `arch/x86_64/rust/mb2_parse.rs` | Unsafe FFI only | Exports the unchanged `mb2_parse` symbol. `BootInfo`/`BootModule` are `repr(C)` with const size and offset asserts (1,624 B, align 8, offsets 0 to 1,620); the C harness asserts the same numbers against the C compiler. Writes the caller's fields individually after zeroing the structure with `write_bytes` (the kernel's `memset`). |
| `arch/x86_64/bootinfo.c` | C | Loses only `mb2_parse` and its `terminated` helper (a comment marks the spot). `boot_framebuffer`, `boot_mmap_entry`, `boot_reserve` and helpers are unchanged. |
| `arch/x86_64/rust/kernel_probe.rs` | Build | One `mod mb2_parse;` line. `arch.mk` is unchanged: `bootinfo.o` stays linked. |
| `tests/host/reference/bootinfo_ref.c` | Test only | Verbatim copy of the accepted `bootinfo.c`, SHA-256 `8a10854a27f94f5b4de9fdb2bdc9306beffa5149f556d48461e5fea7609251da` (checked by the gate). Not part of any production link. |
| `tests/host/mb2-differential.c`, `tests/rust/mb2_parse_host.rs`, `scripts/test-mb2-parse.py` | Tests | The differential, its overflow-checked host build, and the gate (one new step in the existing Build smoke job). |
| `scripts/test-mb2-parse-native.py` | Tests | Execution proof in QEMU (below). Not a CI step. |

## Acceptance matrix

| Gate | Result | Evidence |
| --- | --- | --- |
| A. Differential against the frozen C | **PASS.** 1,528,066 inputs, zero mismatches, on the production archive and on the same sources built for the host with overflow checks, debug assertions and abort-on-panic: 1,128,066 deterministic (every aligned 32-bit word of 8 structurally valid bases set to 39 boundary values; every tag-header pair; every byte; every `total_size` from 0 past the block and at its limits; physical addresses at 0, 4, 8, near 1 GiB and wrapping; memory-map entry size x version x payload length; 0 to 66 modules; module range products around 0 and 1 GiB; string tags of every length and termination; duplicate and misordered tags; END tag sizes and positions) plus 400,000 seeded random. Compared: the error string, and every defined field of `struct boot_info` including the partial state after an error (padding is not compared; the Rust side is asserted to zero it). Pointers are compared as identical pointers into the same block. All 19 outcomes (success and 18 errors) occur. | [differential](a2-mb2-parse-rust/evidence/differential.txt), [gate](a2-mb2-parse-rust/evidence/gate-results.json) |
| B. Stopping rule | The random phase ended at its **400,000-input cap, not at the plateau rule**: new structural classes (outcome x module bucket x framebuffer x EFI flags) were still appearing slowly (561 classes, the last at input 1,521,335 of 1,528,066). Coverage rests on the deterministic phase, which produces every outcome; the rare ones are few (outcome "too many modules" 2 inputs, "duplicate memory map" 25). | differential output |
| C. Negative controls | **20/20 broken references detected** (18 by mismatch, 2 by guard-page fault): size and boundary comparisons, END and module and memory-map rules, tag alignment, first-framebuffer and first-EFI-map selection, address alignment and limit, duplicate command line, missing memory map, string termination, message text, output not zeroed. | gate |
| D. Memory safety of reads | Each block is mapped flush against a PROT_NONE guard page at the rear and at the front; a read outside `[info, info + total_size)` by either implementation faults. Invalid physical addresses get a guard-page pointer. No fault occurred. | differential |
| E. Existing in-kernel self-tests | **PASS.** `selftest: bootinfo 183 cases, 0 failed` on the VOS_TEST kernel, BIOS, 128M and 2G, which now run against the Rust parser, with all other suites at 0 failed and `vmm: audit passed`. | [128M](a2-mb2-parse-rust/evidence/selftest-128M.txt), [2G](a2-mb2-parse-rust/evidence/selftest-2G.txt) |
| F. Production BIOS and UEFI boots | **PASS.** Production Legacy BIOS ISO under `scripts/test.sh` with the arch-test markers; production UEFI ISO under `halt-test.sh uefi-boot` (OVMF) with the same markers. | [BIOS](a2-mb2-parse-rust/evidence/bios-test-sh.txt), [UEFI](a2-mb2-parse-rust/evidence/uefi-halt-test.txt) |
| G. Production boot path enters Rust | **PASS on both firmwares.** A QEMU gdbstub observer (the repo's `vos_gdbstub.py`, which never writes guest state) traps at the `vos.bin` address of `mb2_parse` with RIP equal to it, records the arguments, traps again at the return address (return value NULL), reads the `struct boot_info` the Rust code filled and checks it against the block and the kernel's own report (BIOS: 9 map entries, 1 module; UEFI: 21 entries, 1 module, an EFI map tag), then releases the guest to `boot: checks passed`. | [result](a2-mb2-parse-rust/evidence/native-proof-result.json) |
| H. Symbol and link ownership | **PASS.** `vos.bin` defines `mb2_parse` (a 276 B wrapper plus a 1,197 B `walk`) from `libvos_kernel.a`; the link map takes it from the archive; `bootinfo.o` does not define it; the one call site (`kernel.c`) resolves to it; 0 `core::panicking`/`core::fmt` symbols. The only call out of the wrapper is the kernel's `memset` for the 1,624 B zeroing. | [measurements](a2-mb2-parse-rust/evidence/measurements.txt) |
| I. ABI and layout | **PASS.** Const asserts in Rust and `_Static_assert`s in the C harness pin the same size, alignment and member offsets; the Rust-owned `bi` layout is compared field by field. `bootinfo.h` is unchanged. | glue, harness |
| J. Resources | **PASS, all within the targets.** Net input-section growth **+557 B** (text +593, rodata -36, data 0; target 2,048 B). Loaded kernel growth **0 B**: `.text/.data/.bss` sizes and all five PT_LOAD segments are identical to the baseline (target 4,096 B); text slack to the next page fell from 2,483 B to 1,827 B. Stack **112 B** along the Rust chain (wrapper 56 B plus `walk` 56 B) against 24 B for the C function in the kernel build: **+88 B** (targets 512 B and +256 B). No heap, no added writable statics. | [measurements](a2-mb2-parse-rust/evidence/measurements.txt) |
| K. Hosted harness that calls the real function | **PASS.** `tests/host/usb2c3a2-efi.c` (`#include`s `bootinfo.c`, links the archive) now runs the Rust `mb2_parse` on fixture blocks: 262,450 checks. | [run](a2-mb2-parse-rust/evidence/c3a2-harness-run.txt) |

The baseline for sizes is the kernel of the Dell survey image build (`602a1dc7`): its build inputs are byte-identical to
`c77dabb2` apart from the unlinked hosted EHCI crate, with the same make and rustc. Hashes of the A2 QEMU build:
[image-hashes](a2-mb2-parse-rust/evidence/image-hashes.txt). A QEMU screenshot of the production UEFI boot is kept as virtual-machine
evidence ([png](a2-mb2-parse-rust/evidence/qemu-uefi-boot.png)); it is not physical-hardware evidence.

## Intentional safety differences (not regressions)

Defensive handling of caller bugs that the C would fault on or corrupt itself on; none changes a valid call. All report the
existing `"info block address invalid"` string. They are checked for the Rust function only and never run through the C reference.

| Caller bug | C | Rust |
| --- | --- | --- |
| `bi` null or misaligned | faults, or misaligned stores | refused without writing |
| `info` block overlaps `bi` | zeroes the block it is about to parse and then reports a size error | refused without writing (Rust must not hold a shared slice over memory it writes) |
| `info` null with a valid physical address | faults | `bi` zeroed, then refused (as for an invalid address) |

`info` itself is read through bytes, so a misaligned block still parses as before. The parser cannot validate that an arbitrary
pointer is mapped: a valid writable `bi` and a readable block of `total_size` bytes remain the caller's requirement.

## Failed attempts and corrections

- The first production build failed `-D warnings` (`private-interfaces`: a private struct in a public signature); the structures were made `pub`.
- Self-review before verification found that the glue zeroed `bi` before reading the block header, so an overlapping block would have
  been destroyed and misreported as a size error; the glue now reads the header first.
- Gate run 1 failed on a bug in my test harness (a 136 B buffer read 138 B deep when building a block), not in the parser. Run 2 got to the
  negative controls and two broken-reference spellings tripped `-Werror` (an unsigned comparison); they were respelled. Run 3 passed but measured
  the stack of the 276 B wrapper only, understating the footprint because the compiler kept `walk` out of line; the gate now sums the frames along
  the call chain (run 4, the one recorded). Failed and superseded runs were kept in the build tree.
- The plateau stopping rule was not reached (see B). I narrowed the plateau measure to the structural class, as A1 did, and report the
  broader count separately; it still ran to the cap.

## Limits

- QEMU and hosted tests only; no physical hardware and no Dell. QEMU's firmware (SeaBIOS and OVMF with GRUB) is the only loader tested.
- The parser relies on the caller's guarantee that the block is mapped and readable for `total_size` bytes; the differential and the guard pages
  check that neither implementation reads outside it, not that the loader keeps its promise.
- Rust code is not ASan-instrumented; safety rests on `get`-based reads, the guard-page arena, the overflow-checked host build and the symbol gate.
- Text slack to the next page is now 1,827 B, so the next migration that grows the text segment by more than that adds a page (4,096 B).
- The parser is the first Rust code to run this early in boot (after `cpu_init_protections`, before the PMM); the execution proof covers it on both firmwares in QEMU.

## Next

Recommended next R-Series target: **A6, the PCI configuration accessors**, which are small, pure at their core and used by every driver
(the survey, xHCI and the future EHCI work all depend on them). `boot_framebuffer` (523 B, pure validation) is the small follow-up inside
this file if you want the display path moved first; it adds the `struct boot_fb` ABI. Neither starts without your authorization.
