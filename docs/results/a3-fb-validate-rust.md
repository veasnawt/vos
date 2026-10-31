# A3: Rust framebuffer validation

**Verified for PR review, not accepted or merged.** The production kernel's `boot_framebuffer()` is now freestanding `no_std` Rust inside
the existing kernel archive, exported directly with the unchanged C ABI (no C shim). Its caller (`screen.c`), the self-tests, `struct
boot_fb`, `struct boot_info`, every constant and all 12 error strings are unchanged. The accepted C is kept only as the frozen differential
reference and is not linked into the kernel. Verification is hosted and QEMU only; physical hardware is NOT TESTED (physical VOS testing is
suspended).

Baseline: `main` at `fd881208d1988e18b0122d6eac73923196395aa7`, branch `feat/a3-fb-validate-rust`. Scope: the owner's A3 authorization, Tier 1 + 2.
`boot_mmap_entry`, `boot_reserve` and the reservation logic stay C; `bootinfo.h` is unchanged.

## Implementation

| Part | Owner | Notes |
| --- | --- | --- |
| `kernel/rust/mb2/framebuffer.rs` | Safe Rust, no `unsafe`, no allocation, no panic path | `precheck` (tag count and presence) and `validate` (the tag), in the C's order of checks; reads through `get`; the 64-bit products and the address wrap test are checked arithmetic. It is handed at most `min(size, 38)` bytes of the tag. The 12 messages are byte-identical NUL-terminated statics. |
| `arch/x86_64/rust/fb_validate.rs` | Unsafe FFI only | Exports `boot_framebuffer`. `BootFb` is `repr(C)` with const size and offset asserts (40 B, align 8; offsets 0, 8, 16, 24, 28, 32); the C harness asserts the same numbers, and the base tag (32 B) and RGB extension (6 B) layouts, against the C compiler. Does not read the tag unless exactly one was recorded, then reads the 8-byte header and no more than `min(size, 38)` bytes. Zeroes `*fb` with `write_bytes` and writes fields individually. |
| `arch/x86_64/rust/mb2_parse.rs` (A2) | Rust | Visibility only: `BootInfo.fb` and `BootInfo.fb_tags` became `pub(crate)` so the glue can read them; every other field stays private. |
| `arch/x86_64/bootinfo.c` | C | Loses only `boot_framebuffer` (a comment marks the spot). |
| `arch/x86_64/rust/kernel_probe.rs` | Build | One `mod fb_validate;` line. `arch.mk` is unchanged. |
| `tests/host/reference/bootinfo_ref.c` | Test only | **The A2 frozen reference, reused and unmodified** (SHA-256 `8a10854a27f94f5b4de9fdb2bdc9306beffa5149f556d48461e5fea7609251da`, checked by the gate); it already carries `boot_framebuffer` under the renamed symbol. |
| `tests/host/fb-differential.c`, `tests/rust/fb_validate_host.rs`, `scripts/test-fb-validate.py` | Tests | The differential, its overflow-checked host build, and the gate (one new step in the existing Build smoke job). |
| `scripts/test-fb-validate-native.py` | Tests | The execution proof in QEMU. Not a CI step. |

## Acceptance matrix

| Gate | Result | Evidence |
| --- | --- | --- |
| A. Differential against the frozen C | **PASS.** 1,396,996 inputs, zero mismatches, on the production archive and on the same sources built for the host with overflow checks, debug assertions and abort-on-panic: 1,296,996 deterministic over 12 bases (every value of `type`, `bpp`, `size` 0-256 and each of the six channel bytes; every width and height 0-9000 and every pitch 0-70000; boundary and wrap addresses of each base; field pairs that interact; every tag count) plus 100,000 seeded random. Compared: the message by content and every defined field of `struct boot_fb` (padding not compared; the Rust side is asserted to zero it), including the zeroed output after every failure. All 14 outcomes occur in the deterministic phase and at least 10 times in the random phase (the least frequent is "larger than 32 MiB", 107). | [differential](a3-fb-validate-rust/evidence/differential.txt), [gate](a3-fb-validate-rust/evidence/gate-results.json) |
| B. Stopping rule | **Converged.** The random phase ran its 100,000-input minimum and stopped on the plateau rule (no new (outcome, tag-size class) for 50,000 consecutive inputs, 21 classes) with every outcome seen at least 10 times; the 400,000 cap was not reached. | differential output |
| C. Negative controls | **20/20 broken references detected** (19 by mismatch, `duplicate-count` by a guard-page fault): width, height, pitch and size boundaries, address alignment, zero and wrap, bits per pixel, a channel field, the tag-size thresholds 32 and 38, the tag count, three EGA rules, the EGA size 4,000, the missing output zeroing, a message text. | gate |
| D. Memory safety of reads | Each tag is mapped so that exactly `max(8, min(size, 38))` bytes are readable, flush against a PROT_NONE guard page at the rear and at the front; a read outside it by either implementation faults. No fault occurred. When no tag may be read (none recorded, more than one) the pointer is a guard page. | differential |
| E. Hardening (Rust only) | **PASS.** A null `bi`, a null or misaligned `fb`, a misaligned `bi`, an `fb` inside the `boot_info`, and an `fb` that overlaps the tag are each refused with "no framebuffer information" **without writing** (the tag keeps its bytes); a valid call still works afterwards. The C reference is never run on any of them. | differential |
| F. Existing in-kernel tests | **PASS.** `selftest: bootinfo 183 cases, 0 failed` (including its 17 framebuffer cases built from synthetic tags) on the VOS_TEST kernel, BIOS at 128M and 2G and UEFI; `selftest: screen 13 cases, 0 failed, 1 not applicable`. | [128M](a3-fb-validate-rust/evidence/selftest-128M.txt), [2G](a3-fb-validate-rust/evidence/selftest-2G.txt), [UEFI](a3-fb-validate-rust/evidence/uefi-test.txt) |
| G. Production boots (Tier 1 and 2) | **PASS.** Legacy BIOS (`test.sh`, arch-test markers); UEFI 1280x800 (`halt-test.sh uefi-boot`, colour check on the screendump); UEFI **no-GOP** image (`uefi-nofb`: `screen: none, serial only`); UEFI **640x480** and **1920x1080** (colour check each); UEFI **self-test image** (`uefi-test`, exit status 33, 9 markers including `selftest: screen`). | [BIOS](a3-fb-validate-rust/evidence/bios-test-sh.txt), [UEFI](a3-fb-validate-rust/evidence/uefi-halt-test.txt), [no-GOP](a3-fb-validate-rust/evidence/uefi-nofb.txt), [640x480](a3-fb-validate-rust/evidence/uefi-640x480.txt), [1920x1080](a3-fb-validate-rust/evidence/uefi-1920x1080.txt) |
| H. Production boot enters Rust | **PASS in all five cases** (BIOS, UEFI, UEFI no-GOP, 640x480, 1920x1080). A QEMU gdbstub observer (the repo's `vos_gdbstub.py`, which never writes guest state) traps at the `vos.bin` address of `boot_framebuffer` with RIP equal to it, then at the return address, and reads the result: NULL and a BGRX32 `struct boot_fb` equal to the `screen: framebuffer WxH BGRX at ..., pitch P` line the kernel prints (1280x800 at `0xfd000000` on BIOS and `0x80000000` on UEFI, 640x480, 1920x1080); on no-GOP the static string "no framebuffer information" and an all-zero struct. The guest is then released to `boot: checks passed`. | [result](a3-fb-validate-rust/evidence/native-proof-result.json) |
| I. Symbol and link ownership | **PASS.** `vos.bin` defines `boot_framebuffer` (a 317 B wrapper) and one out-of-line `validate` (882 B) from `libvos_kernel.a`; the link map takes it from the archive; `bootinfo.o` does not define it; the one call site (`screen_init`) resolves to it; 0 `core::panicking`/`core::fmt` symbols. | [measurements](a3-fb-validate-rust/evidence/measurements.txt) |
| J. ABI and layout | **PASS.** Const asserts in Rust and `_Static_assert`s in the C harness pin the same sizes, alignments and member offsets for `boot_fb`, the 32-byte framebuffer tag, the 6-byte RGB extension and the two `boot_info` fields read; `bootinfo.h` is unchanged. | glue, harness |
| K. Resources | **PASS, all within the targets** (baseline: A2's kernel, whose kernel inputs are identical to main's). Net input-section growth **+642 B** (text +671, rodata -29, data 0; target 2,048 B). Loaded kernel growth **0 B**: `.text/.data/.bss` sizes and all five PT_LOAD segments are identical to the baseline (target 4,096 B); text slack to the next page fell from 1,827 B to 1,187 B and rodata slack is 3,208 B. Stack **80 B** along the Rust chain against 24 B for the C function in the kernel build: **+56 B** (targets 512 B and +256 B). No heap, no writable statics. | [measurements](a3-fb-validate-rust/evidence/measurements.txt) |

## Screenshots (authentic QEMU screendumps, not physical hardware)

[1280x800](a3-fb-validate-rust/evidence/qemu-uefi-1280x800.png), [640x480](a3-fb-validate-rust/evidence/qemu-uefi-640x480.png) and
[1920x1080](a3-fb-validate-rust/evidence/qemu-uefi-1920x1080.png) are the PNGs the existing `halt-test.sh` colour check produced for these
runs. The raw PPM screendumps, the OVMF variable stores and the other build outputs are kept untouched in the build tree.

**Comparison with the accepted A2 baseline.** The 1280x800 UEFI screenshot is **not byte-identical** to A2's `qemu-uefi-boot.png`, and that is expected:
the screen shows the boot log, and GRUB placed the multiboot2 information block and the module at different addresses in this ISO. Comparing the two raw
screendumps pixel by pixel ([script and output](a3-fb-validate-rust/evidence/screenshot-comparison.txt)): **428 of 1,024,000 pixels differ, in 7 text rows**. The text
of each row, decoded from the screendump with the kernel's own font table, is one of the seven log lines whose serial text also differs between the two boots:
`multiboot2 info at 0x91000-0x92ff0` versus `0x8c000-0x8df60`; the module range `0x8d000-0x90348` versus `0x88000-0x8b348` (twice, and on the `app: module` line);
the two `reserved` ranges; `efi: ... from 124 descriptors` versus `121`; and the Rust probe's function address `0x...80140730` versus `0x...801409b0`.
Everything else on the screen, including the line `screen: framebuffer 1280x800 BGRX at 0x80000000, pitch 5120`, is pixel-identical. **There are no unexplained pixel changes.**

## The test-only host unwinding stub

`tests/rust/fb_validate_host.rs` (the overflow-checked host build) defines a `rust_eh_personality` function that does nothing. The prebuilt host `core` carries unwinding
tables for some of its functions and names that personality routine, which only `std` defines; with `panic = abort` nothing ever unwinds, so the stub only satisfies the
linker and is never called. It exists only in that host root and is never part of the kernel, whose build has no such symbol (checked by the panic/format symbol gate).

## Intentional safety differences (not regressions)

Defensive handling of caller bugs that the C would fault on or corrupt itself on; none changes a valid call. All report the existing `"no framebuffer information"`
string and do not write `*fb`. They are checked for the Rust function only and never run through the C reference.

| Caller bug | C | Rust |
| --- | --- | --- |
| `bi` null or misaligned; `fb` null or misaligned | faults | refused without writing |
| `fb` overlaps the `boot_info` | zeroes the fields it is about to read | refused without writing |
| `fb` overlaps the tag | zeroes the tag it is about to validate | refused without writing |

The tag is read through bytes, so a misaligned tag still validates as before. The port cannot check that an arbitrary pointer is mapped: a readable `bi`, a writable
aligned `fb`, and a tag readable for its header and for `min(size, 38)` bytes remain the caller's requirement (as in the C).

## Storage incident and independent readmission

The guard (limits: 120 MB forecast, 139 MB sum of projections, 145 MB peak, 160 MB stop, 175 MB cap, floor 22,548,578,304 B, 1.25x per step) tripped once, at the step
`extra-gop-sizes`: it grew 8,295,018 B against a 2,500,000 B projection, 5,170,018 B over its 1.25x limit. My projection counted only the two PNGs and the logs; the
step also left two raw screendump PPMs (6,220,817 B and 921,615 B) and an OVMF variable store per run (2 x 540,672 B). Both UEFI cases had passed. The worktree
was 132,062,507 B (stop line 160 MB). `STOP-A3` was **not** self-released: Antigravity's independent audit accepted a controlled resumption, the original STOP file was
archived unchanged under its authorized name with its hash recorded, and a readmission record (cause, revised allowance, measurements, unchanged global limits) was appended to the
storage log; the original projection and the failed step record were not altered. Copies of the log and of the archived STOP are in
[storage/](a3-fb-validate-rust/evidence/storage/). The guard location is a new directory under the git-ignored `build/` of a protected worktree; a manifest of all 2,589
existing files there was identical before and after it was created. No evidence was deleted.

## Failed attempts and corrections

- The first production build failed `-D warnings` on an unused constant (`FB_NONE`; the zeroed output already is `BOOT_FB_NONE`); it was removed.
- The first gate run passed the archive differential but failed to link the host build (the unwinding personality above); the test-only stub was added and the gate run again (run 2, the one recorded).
- The storage trip above.

## Limits

- QEMU and hosted tests only; no physical hardware. Both firmwares in QEMU boot with a 1280x800 BGRX framebuffer, so the real EGA text path is covered by the synthetic
  self-test cases and the differential, not by a real EGA boot.
- Rust code is not ASan-instrumented; safety rests on `get`-based reads, the guard-page arena, the overflow-checked host build and the symbol gate.
- Text slack to the next page is now 1,187 B, so the next migration that grows the text segment by more than that adds a page (4,096 B).
- The accepted C remains in `tests/host/reference/bootinfo_ref.c` and must stay byte-identical (the gate checks its hash).

## Next

Recommended next R-Series target: **A6, the PCI configuration accessors** (assessed earlier; boundary option B with thin C shims and a simulated I/O backend). It needs its own
authorization and storage admission and is not started.
