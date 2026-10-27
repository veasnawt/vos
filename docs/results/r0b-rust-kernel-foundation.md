# R0-B: minimal freestanding Rust kernel integration

Local implementation ready for owner review, 2026-10-27. Not published, merged,
or accepted as a completed kernel migration. Parent: `67a525e2cbcb13215c4d17cdfd0e3740ad40ff16`.
The [Master Plan](../plans/MASTER-PLAN.md) R-series remains unchanged.

## Baseline and scope

The complete R0-A assessment was read from the preserved `vos-r0a` checkout at
`689260f6c93c97e9d062e7829f078a13e92350be`. Its 30 input identities matched the
accepted main before implementation. Work uses a separate `vos-r0b` clone and
`feat/r0b-rust-kernel-foundation` branch. R0-A, USB.1 and the interrupted D1
workspace were not repurposed. The owner's discontinued controller/attestation
workflow was not restarted; no old funding or reservation was applied.

This adds one pure Rust function and its panic bridge. Boot assembly, linker
script, paging, allocator, interrupts, scheduler, drivers and application ABI
definitions remain unchanged. No R1 helper or USB.2 driver is implemented.

## Actual integration

`arch/x86_64/rust/kernel_probe.rs` is a `#![no_std]` static library. Its private
kernel interface is:

```c
uint64_t vos_rust_probe(uint64_t seed, uint64_t salt);
```

```rust
pub extern "C" fn vos_rust_probe(seed: u64, salt: u64) -> u64
```

The result is `(rotate_left(seed,17) + rotate_right(salt,9)) XOR
0x564f535255535430`, with addition modulo 2^64. There are no pointers, mutable
globals, allocation, hardware operations or implicit arithmetic panics in this
function. C supplies runtime boot values derived from the Multiboot magic, RIP,
RSP and CR3, calls Rust, and checks a separate unsigned-C oracle before emitting
`rust: kernel probe PASS`. The check runs after CPU protections, kernel CR3,
identity-map removal, W^X and the VMM audit, before IRQ/scheduler initialization.
It does not change interrupt state or stack ownership.

The [linked disassembly](r0b-rust-kernel/normal-rust-linked.txt) shows the real
26-byte function at `0xffffffff8012fea0`: two rotates, `lea`, constant load,
XOR and return. The [C call relocation](r0b-rust-kernel/c-call.txt) and
[link-map excerpt](r0b-rust-kernel/link-origin.txt) establish its archive origin
and call path. This is not a C-only success message.

## Build and toolchain

The existing installed `nightly-2026-09-03` compiler was used: Rust
`1.100.0-nightly`, commit `2e2b193f8ada105f27608b7be81c293e0d7292cb`, LLVM 23.1.1.
GCC 15.2.0 and GNU ld 2.46 remain the C compiler and final linker.
No Cargo, crate download, custom target, allocator, standard library, LTO or
host runtime was added. The CI preparation step installs this named compiler
and bare-metal target before its existing x86_64 build; local CI execution and
GitHub checks are NOT RUN because publication is not authorized.

Kernel-specific flags, separate from Rust userspace flags:

```text
--edition=2024 --target=x86_64-unknown-none --crate-name vos_kernel
--crate-type=staticlib -D warnings -C panic=abort -C opt-level=2
-C target-cpu=x86-64 -C code-model=kernel -C relocation-model=static
-C no-redzone=yes -C force-unwind-tables=no -C lto=off
-C embed-bitcode=no -C codegen-units=1 -C debuginfo=0
--remap-path-prefix=<checkout>=.
```

The [official target contract](https://doc.rust-lang.org/rustc/platform-support/x86_64-unknown-none.html)
specifies a bare-metal System V C ABI without a red zone or default vector/FP
register use. Actual emitted objects and final instructions were also inspected;
target defaults alone were not treated as evidence.

`ARCH_LIBS`/`LIBS` carries the archive separately from `ARCH_OBJS`; object `.d`
inclusion therefore never attempts to parse an archive. Rust emits its own
dependency file. A content-stable input fingerprint includes source path,
flags, compiler version/hash and target `core`/`compiler_builtins` hashes.
Unchanged invocations preserve the archive mtime; source or flag changes rebuild.
GNU ld consumes C/assembly objects followed by the archive and emits a link map.

Two isolated clean builds produced byte-identical archives and kernel ELFs:

- Kernel SHA-256: `66bf6ee698ab2739ab1ad793cba14c0a0db83178f5bce1821def74bf31838182`.
- Archive SHA-256: `c76ef87940e3470a12769c17e9aa3b175a9aab72e6b9ea1917a379c907aae4e1`.

Reproducibility is established for these inputs/toolchain and checkout. ISO
byte reproducibility and cross-version compiler reproducibility are not claimed.

## Panic bridge and binary constraints

The single Rust `#[panic_handler]` ignores formatting metadata and forwards
static `b"Rust kernel panic\0"` to existing `panic(const char *) -> !` through
one documented unsafe FFI call. The bytes are immutable, NUL-terminated and
have static lifetime. C retains interrupt-disable, diagnostics and halt/test-exit
behavior. No unwind crosses the ABI and no formatting engine is used by the
bridge. See the [Rust panic-handler contract](https://doc.rust-lang.org/nomicon/panic-handler.html).

The Rust object has `R_X86_64_32S` for the static message and `R_X86_64_PLT32`
for C `panic`; final linkage resolves both. There are no unresolved final
symbols, dynamic loader, TLS, loadable unwind tables or FP/SIMD instructions in
the production, deliberate-panic or corrupted-result kernel. The production
Rust function is stackless; its panic bridge adjusts RSP before calling C and
uses no red zone. The target's prebuilt soft-float `core` is retained as a
build input, not a host runtime.

The 11,100,422-byte static archive includes target `core` and compiler builtins;
it is a build artifact, not the size loaded by VOS. The ordinary final kernel
does not extract the unused `core` panic/formatting implementation. The
test-only `panic!` trigger does pull additional `core` code and a statically
resolved GOT into the existing RW/NX data segment. It requires no runtime
relocations or dynamic linker, passes the unchanged layout validator, and is
absent from normal boot. Its larger 3,071,208-byte ELF is not the production
image. Rust's internal handler symbol name `rust_begin_unwind` does not imply
unwinding; panic mode is abort and the handler calls nonreturning C panic.

## Layout and image size

The accepted C-only parent was rebuilt from `git archive` in a fresh directory
with the same C toolchain. Production comparison:

| Measurement | Accepted parent | R0-B |
|---|---:|---:|
| ELF file, including C debug information | 1,021,224 bytes | 1,022,640 bytes |
| `size`: text / data / BSS | 212,992 / 220 / 618,304 | unchanged |
| Physical reservation | `0x100000..0x1cc000` | unchanged |
| Program entry | `0x100038` | unchanged |
| PT_LOAD count | 5 | 5 |

File growth is 1,416 bytes; existing page padding absorbs this small component.
The 816 KiB page-rounded reservation leaves 2,310,144 bytes below the 4 MiB end
limit. Segment permissions remain RX, RW, RX, R, RW; no writable executable
segment or load gap/tail was introduced. The linker assertions and
`scripts/check-kernel-layout.py` were not changed.

## Focused verification

All ten native cases passed using QEMU 10.2.1, q35/TCG, 128 MiB, no network,
no host disks, SeaBIOS or OVMF with a fresh private variable store.

| Case | BIOS | UEFI |
|---|---|---|
| Production boot, `qemu64`: Rust result, hello, reclamation, boot checks | PASS | PASS |
| Same production image, `-cpu max` | PASS | PASS |
| Pointer GUI, a/b, movement, primary down/up, q, apps, hello, Rixie help/exit, restored shell | PASS | PASS |
| Selected actual Rust `panic!` reaches C panic and debug-exit 35 | PASS | PASS |
| Selected C result corruption after Rust return fails closed, no Rust PASS, exit 35 | PASS | PASS |

The harness independently decodes the serial seed/salt/result and computes the
64-bit contract. Normal serial function addresses match the linked symbol.
The interactive logs show pointer release, two child-console restorations and
`native: vsh PASS, process reclaimed`. Every normal application ELF extracted
from both interactive ISOs matches its source build byte-for-byte (5/5 each).
ABI v0 hello, v3 Rust applications, v4 foreground execution and v5 pointer GUI
are exercised; v1/v2 definition and implementation files remain byte-identical
but their complete historical campaigns were not rerun.

The C host fixture links the exact bare-metal Rust archive and passes 4,132
edge/pseudorandom pairs plus two independent fixed vectors under ASan/UBSan.
The sanitizer instruments the hosted C harness, not the freestanding Rust code;
it is not evidence of Rust-wide sanitizer coverage. Incremental source/flags
checks, no-op checks, clean-build identity, Multiboot/layout validation and an
i386 build/header check also pass. No historical stress campaigns were rerun.

Raw serial, host output, binary inspection, image hashes and the independent
audit are indexed by [results.json](r0b-rust-kernel/results.json) and
[manifest.json](r0b-rust-kernel/manifest.json). Serial evidence is sufficient
for this nonvisual integration; no new graphical capability or screenshot is
claimed. Full build outputs remain in local `build/r0b-1` and `build/r0b-check-1`.

The original executed verifier is retained with the build evidence. Two final
verifier refinements pin the accepted baseline instead of moving HEAD and wait
for the explicit vsh reclamation marker. The recorded baseline and both logs
independently satisfy those stronger checks. The final explicit test-only cfg
guard rebuilt normal/panic ELFs byte-identically to native-tested inputs, so
native tests were not repeated. A follow-up negative fixture initially treated
Make's correct rejection of `VOS_TEST_CASE` without `VOS_TEST=1` as an unexpected
exception; the original stop and corrected expected-rejection check are retained.

## Reproduction and ordinary preview

From a checkout with the documented build prerequisites, select the installed
bare-metal compiler explicitly. Commands never run `make clean`:

```bash
rustc_path="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
# Choose a NEW output path. The harness refuses an existing directory.
python3 -I -B scripts/test-rust-kernel.py --out build/r0b-reproduce-1 \
  --rustc "$rustc_path" --grub-efi-dir /path/to/verified/grub/x86_64-efi
```

The local verification used the existing read-only GRUB EFI producer at
`~/vos-evidence/n36-stage3-gatea-cont-20261021T220119Z/efi-tools/usr/lib/grub/x86_64-efi`;
ordinary contributors can supply their installed GRUB EFI directory instead.
No historical controller or private budget file is required.

Normal interactive media already prepared in this checkout:

```bash
cd ~/src/vos-r0b
qemu-system-x86_64 -machine q35 -accel tcg -cpu qemu64 -m 128M \
  -nic none -no-reboot -serial stdio \
  -cdrom build/r0b-check-1/media/interactive-bios.iso
```

UEFI, preserving the original variable template:

```bash
# -snapshot keeps firmware variable writes in temporary emulator storage.
qemu-system-x86_64 -machine q35 -accel tcg -cpu qemu64 -m 128M \
  -nic none -no-reboot -serial stdio -snapshot \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
  -cdrom build/r0b-check-1/media/interactive-uefi.iso
```

Observe the Rust diagnostic in serial, then use a/b, move/click normally, q to
return to vsh, and `apps`, `hello`, `rixie`, `help`, `exit`. There is no timed
manual stress requirement. Do not use the separate panic/result-control media
as the ordinary preview.

## Preservation, resources and limitations

The original workspace's refs, index, five-file draft and retained symlink
match the saved state. Its 13 worktrees and two stashes remain intact. The
audit rechecked 3,856 historical entries across 13 evidence roots, 919 original
workspace source files and 972 USB checkout source files. R0-A and Master Plan
checkouts retain their original commits and clean state. The initial read-only
audit had a schema error on the symlink record; its failure is preserved. A
second audit's status difference was only Git's collapsed untracked-directory
display; exact `-uall` comparison confirmed the unchanged five-file draft.
No historical evidence, controller or accounting file was written or executed.

New local build artifacts occupy approximately 285 MiB of logical file bytes,
including independent comparison builds and retained images; exact final
measurements are in the evidence resource record. C:, E: and Linux free-space
checks passed before creating the checkout; Linux remained above 939 GiB free
after verification. These are ordinary task measurements, not new legacy
allocations or retirement of old reservations.

This remains QEMU-only evidence. Physical firmware/hardware, concurrency,
Rust-owned interrupts/allocator/drivers, hostile-input parsers and broad Rust
runtime support are not established by a scalar proof. Future Rust components
must audit unsafe/FFI boundaries, stack use, compiler builtins and the 4 MiB
budget again; the prebuilt target is not permission to use arbitrary `core`
features. No performance improvement is claimed.

The next proposed R-series increment is **R1**, one pure low-risk helper with
behavioral-equivalence, bounds and resource tests, after owner review. Rust
USB.2 work needs separate authorization and explicit MMIO/DMA/IRQ/ownership
contracts; this scalar component does not make a driver safe automatically.
