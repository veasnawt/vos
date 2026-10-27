# R0-A: Rust kernel integration assessment

Assessment complete; **R0-B is proposed, not implemented or tested**.
Source baseline: accepted main
`cbe9f9202ca9a45aaafa57ec45fc01baeb347bc1`, in the separate
`docs/r0a-rust-kernel-assessment` checkout. The owner's authorization permits
read-only architectural investigation and this report. It does not permit a
Rust kernel implementation, driver migration or device operation.

The [Master Plan PR #19](https://github.com/veasnawt/vos/pull/19) records the
new priority; this assessment neither depends on its merge nor changes its branch.
USB.1 remains in its separate local checkout and is not part of this main baseline.
No discontinued D1 controller, executor attestation or bootstrap was used.

## Findings and recommendation

The installed bare-metal Rust target is a plausible fit for the kernel's machine
ABI and higher-half layout. Recommend **one small Rust `staticlib`, called through
an integer-only C ABI and linked by the current GNU ld recipe**. Keep C/assembly
boot, paging, process management, interrupts and applications intact. Use a pure,
bounded arithmetic probe first, not a driver or memory-management replacement.

This is source/metadata evidence, not a successful mixed-kernel build. Actual
Rust relocation, runtime dependency, section, instruction and execution checks
must pass in separately authorized R0-B. No compilation, kernel link, QEMU boot,
sanitizer test or physical-hardware experiment was performed in R0-A.

## Current implementation contracts

| Area | Observed implementation | Consequence for Rust |
|---|---|---|
| Build | [Makefile](../../Makefile), [arch.mk](../../arch/x86_64/arch.mk): freestanding GCC objects, explicit GNU ld final link, no host CRT/libc | Retain linker ownership; use separate kernel Rust flags |
| C machine flags | `-m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only`, static/non-PIE, no stack protector or asynchronous unwind tables | Userspace Rust's `code-model=small` is unsuitable as a kernel template |
| Boot | [boot.S](../../arch/x86_64/boot.S): physical 32-bit Multiboot2 stub, long-mode transition, higher-half C entry | No Rust entry point, `_start`, constructors or replacement boot path |
| Addressing | [memlayout.h](../../arch/x86_64/memlayout.h): `KERNEL_VMA=0xffffffff80000000`, initial physical alias below 1 GiB | Compile for the kernel code model, not host/Linux or user addresses |
| Layout | [linker.ld](../../arch/x86_64/linker.ld): load at 1 MiB, higher sections at VMA+physical address, physical end <=4 MiB | Keep five page-tiled PT_LOADs; measure actual image growth |
| Execution | [kernel.c](../../arch/x86_64/kernel.c): GDT/IDT and CPU protections, PMM/VMM, identity removal, W^X map, device map/display, IRQ/scheduler, applications | Put the first checked Rust call after W^X/audit and before IRQ/scheduler setup |
| Stack/entry | 16 KiB guarded boot/thread stacks; [entry.S](../../arch/x86_64/entry.S) clears DF and aligns before C; [thread_switch.S](../../arch/x86_64/thread_switch.S) saves SysV callee-saved GPRs | No red zone, SIMD state or new stack convention; keep the probe's frame small |
| CPU state | [cpu.c](../../arch/x86_64/cpu.c): CR0.EM set, OSFXSR/OSXSAVE cleared, no FPU/SIMD context save | Neither compiler-generated vector copies nor floating-point instructions are acceptable |
| Allocation | [PMM](../../arch/x86_64/pmm.h): owned, zeroed 4 KiB frames below 1 GiB; fixed bounded kernel structures, no general heap | No `alloc`, global allocator or heap-backed collections in R0-B |

### Linker details that must survive

The five load regions are boot text R-X, boot data RW-, higher text R-X,
rodata R-- and data/BSS RW-. Physical ranges must tile the entire page-rounded
kernel reservation without holes or shared loader tails. Text uses trap padding;
boot and kernel BSS have explicit zeroing boundaries. Standard Rust
`.text.*`, `.rodata.*`, `.data.*` and `.bss.*` match existing wildcards.

Do not weaken the linker assertions or
[kernel-layout validator](../../scripts/check-kernel-layout.py). Inspect Rust's
actual output for unexpected allocated/orphan sections, GOT/PLT, TLS, dynamic
relocation, constructors and unwind requirements. Do not hide a required runtime
section by discarding it. Non-loaded debug metadata is distinct from runtime data.
R0-A has not established how many additional loaded bytes R0-B will need.

## Installed toolchain and reusable infrastructure

Read-only queries used the existing compiler at
`~/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc`:

| Query | Observation |
|---|---|
| `rustc -vV` | `1.100.0-nightly`, commit `2e2b193f8ada105f27608b7be81c293e0d7292cb`, LLVM 23.1.1 |
| Built-in target | `x86_64-unknown-none`, ELF, 64-bit pointers, soft-float Rust ABI, no `std` |
| Target specification | Kernel code model; red zone disabled; abort panic; x86-64 baseline; vector features disabled; inline stack probes |
| Installed target libraries | `core` 3,307,980 bytes; `compiler_builtins` 7,827,478 bytes; `alloc` also installed but not required/approved |
| Default `--print cfg` | Reports `relocation_model="pic"`; explicit static relocation belongs in the proposed kernel flags |
| Candidate flag query | Recognized without compiling; still printed the target's PIC cfg, so this query does **not** verify emitted relocations |

The official [target description](https://doc.rust-lang.org/rustc/platform-support/x86_64-unknown-none.html)
documents a freestanding, general-register-oriented target with a SysV C boundary.
The installed compiler's printed metadata is the local observation; online
documentation is not proof that a particular VOS object is safe. In particular,
cfg output advertises `x87`/`fxsr` availability despite the soft-float policy.
Inspect instructions rather than inferring safety from cfg strings.

Stable is the machine's default toolchain; the previously working nightly is
installed alongside it. The session PATH did not expose `rustup`; its explicit
`~/.cargo/bin/rustup` path worked. Pin the actual compiler and target libraries
for reproduction instead of changing defaults or downloading another toolchain.

Reuse the repository's direct-rustc/Make discipline, dependency files, C ABI
conventions, ABI layout assertions and focused testing patterns. The
[native SDK guide](../rust-apps.md) and [user.mk](../../user/user.mk) demonstrate
`no_std` applications, GNU ld integration and separate output roots. Pointer
applications already use rustc's final-link path to avoid duplicated runtime
archives; that is useful experience, not permission to replace the kernel linker.

**Do not link the userspace SDK/runtime into the kernel.** Its small code model,
0x400000 linker script, application entry point and `int 0x80` stubs are different
contracts. Its panic handler exits a process with status 99; it cannot serve as
kernel panic handling. A Rust kernel helper adds no userspace syscall or authority.
Preserve [ABI v0](../abi/syscall-v0.md) through [ABI v5](../abi/syscall-v5.md),
application ELF notes, process isolation and cross-language interoperability.

## R0-B build choices

| Approach | Benefit | Cost/risk | Recommendation |
|---|---|---|---|
| One `no_std` staticlib, final GNU ld | Standard non-Rust integration boundary; rustc packages Rust dependencies; leaves current kernel layout/entry in charge | Archive can include substantial upstream objects; audit selected code and link order | **First choice** |
| Emit a standalone Rust object | Compact and fits the current object-list shape | Final C link must account manually for every Rust/core/builtin dependency and panic symbol; easy to prove too little with an artificially trivial object | Possible later optimization, not automatic fallback |
| Rustc final-link of C/assembly archives | Lets rustc manage target rlibs directly; analogous to pointer app builds | Changes current final-link ownership and validation plumbing | Defer beyond the first proof |

Rust's [linkage reference](https://doc.rust-lang.org/reference/linkage.html)
describes staticlib as the non-Rust-consumer format and notes its dependency
contents. Keep one archive, avoid `--whole-archive`, and retain existing C memory
support. Host archive size is not loaded kernel size. Do not introduce Cargo
dependencies, a custom target, `build-std`, cross-language LTO or a second runtime
without evidence that the first proof needs them.

### Proposed build integration, not executed

Use dedicated `KERNEL_RUST_*` variables. Candidate options, accepted by the
read-only compiler query but **not code-generation verified**, are:

```text
--edition=2024 --target=x86_64-unknown-none --crate-type=staticlib
-C target-cpu=x86-64 -C code-model=kernel -C relocation-model=static
-C no-redzone=yes -C panic=abort -C opt-level=2
-C force-unwind-tables=no -C lto=off -C embed-bitcode=no -C codegen-units=1
```

Never use `target-cpu=native` for this portable guest baseline. Emit the archive
and dep-info into an isolated `BUILD` tree. Pin the sysroot inputs as well as
the crate flags; precompiled dependencies do not acquire new flags merely
because the top-level crate has them. R0-B must verify reachable runtime code.

Add an explicit kernel-library dependency/link variable, passing the archive
**after** the C objects. Do not append `.a` to `ARCH_OBJS`: the current
`-include $(OBJS:.o=.d)` would leave that filename unchanged and attempt to parse
the archive as a Makefile. Keep the Rust dep-info include separate, track source,
flags and compiler identity for rebuilds, and preserve the existing post-link
layout check. Existing linker wildcards should suffice; no linker-script change
is proposed unless an actual, understood output requires separate review.

## Smallest useful Rust execution proof

Propose a private C signature such as:

```c
uint64_t vos_rust_probe(uint64_t seed, uint64_t salt);
```

Implement a fixed, pure integer computation with explicit wrapping and rotation;
no pointers, allocation, global mutable state, MMIO, I/O or user calls. Use two
runtime inputs and an independent unsigned-C expected result. The C boot caller
checks the result before printing a trusted success marker; no cross-language
LTO should erase the call. Include edge vectors in focused host tests, clearly
separate from evidence of kernel execution.

Prove origin and execution together: archive/member and unique-symbol records,
link map, disassembly of the C call into higher-half Rust text, then actual
BIOS/UEFI result checks after the call. A linked unused object or a user-printed
marker is insufficient. C/assembly must continue through normal boot and native
application interaction. This is an integration proof, not migration of a real
subsystem or evidence of physical-hardware readiness.

### Panic and runtime contract

Supply exactly one kernel-side `#[panic_handler]`. A minimal reviewed handler
can ignore formatting and call existing C [panic](../../arch/x86_64/panic.h)
with a fixed NUL-terminated static message and a non-returning declaration.
The existing implementation disables interrupts, reports, and halts; its test
build reports failure through the established QEMU exit path. No unwinding
across C/assembly and no process-exit syscall are allowed. See the official
[panic-handler contract](https://doc.rust-lang.org/nomicon/panic-handler.html)
and [FFI unwinding rules](https://doc.rust-lang.org/nomicon/ffi.html#ffi-and-unwinding).

The first computation should avoid formatting, indexing, division and wide
arithmetic that obscure dependency/stack costs. Audit unresolved references
and compiler-builtins selection. [Kernel lib.c](../../arch/x86_64/lib.c) provides
`memcpy`, `memset`, `memcmp` and `strlen`, but **not `memmove`**; the separate
application runtime provides it. Do not import that runtime or fabricate stubs
to satisfy unexpected helpers. Resolve the real requirement explicitly or stop.
Inline stack probes and any emitted personality/probestack symbols also need
inspection; do not disable guards simply to make linking succeed.

## Ownership and future driver boundaries

- Use scalar C types initially. Future shared records require `repr(C)` plus
  independent C/Rust size, alignment and offset assertions. Rust references,
  slices, enums and `Result` are not an unversioned C wire ABI. Edition 2024's
  [unsafe export attributes](https://doc.rust-lang.org/edition-guide/rust-2024/unsafe-attributes.html)
  require reviewing symbol uniqueness and ABI, not just spelling `no_mangle`.
- The kernel is single-CPU and normally IF=0/cooperative, with an idle interrupt
  exception; user threads have their own address spaces. This does not justify
  arbitrary `static mut`, aliasing or blanket `Send`/`Sync`. No Rust borrow may
  outlive a C resource, survive invalidating callbacks or cross a yield while
  pretending exclusive ownership is retained.
- [Process destruction](../../arch/x86_64/process.c) requires the child thread
  gone, kernel CR3 active and file operations unwound. Rust ownership wrappers
  must preserve this order; `Drop` is not permission to reclaim in any context.
- [IRQ top halves](../../arch/x86_64/irq.h) cannot allocate, block, yield or wake
  threads. Keep the first Rust call in boot thread context; do not move interrupt
  entry or scheduler code into Rust in R0-B.
- [Device mappings](../../arch/x86_64/devmap.h) are bounded supervisor UC/WC,
  NX mappings with guarded ranges, maximum eight mappings/64 MiB and no general
  unmap. Future Rust MMIO wrappers must validate width, alignment, mapping and
  lifetime, using raw volatile accesses rather than ordinary Rust references.
  [Volatile access](https://doc.rust-lang.org/std/ptr/fn.read_volatile.html)
  does not provide atomic synchronization or DMA ordering by itself.
- Existing virtio drivers allocate owned PMM pools, publish with explicit
  barriers, and free only after confirmed reset; otherwise they quarantine
  frames. Rust drivers must preserve that state machine. Hardware may retain
  DMA access beyond an object scope: ordinary RAII cannot safely free it then.
  No IOMMU isolation or new application device authority is implied.

## Proposed focused R0-B acceptance

These are future checks, **NOT RUN in this assessment**:

1. Review/pin the selected compiler, target libraries, kernel flags and dependency
   rules. Build twice in fresh bounded roots and compare Rust object/archive
   contents and final kernel bytes, accounting explicitly for debug paths.
2. Inspect archive/map/ELF/relocations/disassembly: unique Rust-owned entry,
   no undefined runtime symbols, hosted libraries, dynamic loader, TLS or
   unreviewed allocated sections; no x87/MMX/SSE/AVX, red-zone use or unsupported
   CPU instructions in reachable Rust/runtime code. Verify stack use separately.
3. Require existing linker assertions, `grub-file` and the unmodified
   [layout validator](../../scripts/check-kernel-layout.py), including its
   [focused synthetic controls](../../scripts/test-check-kernel-layout.py).
   No page-tail, W^X or 4 MiB limit relaxation.
4. Host-test the arithmetic and C ABI vectors; make an incorrect-result negative
   fixture fail the caller check. This does not replace native execution proof.
5. Boot both BIOS and UEFI with the same verified kernel/application payload
   hashes; establish the actual Rust return check and continued existing boot.
   Include qemu64/default and a relevant max-CPU control to catch instruction
   assumptions, keeping machine/memory options recorded and bounded.
6. Use focused normal GUI/pointer-to-vsh, `apps`, `hello` and Rixie interaction
   checks, kernel/user page audits and clean process reclamation. Preserve
   ABI/header and application identities; keep the unchanged i386 build working.
7. Exercise a separately selected, deliberate Rust-panic fixture through the
   non-returning C panic path; require the expected diagnostic/test exit. Do not
   hide a panic, hang, bad relocation or lifecycle defect with retries.

Use public image producers with isolated `BUILD`, `USER_BUILD` and validator
outputs; never `make clean` over retained evidence. System GRUB EFI modules are
absent here, but OVMF and an existing unpacked GRUB producer are available via
the documented `GRUB_EFI_DIR` override. Verify producer identities read-only or
provision normal public prerequisites separately. Some historical GUI scripts
depend on private retained paths: do not execute them blindly or revive their
controller chain. Adapt only the focused public/QMP test mechanism when R0-B is
authorized. No new N3/N4-style campaign or physical boot experiment is proposed.

## Proposed scope, risks and owner decisions

Expected R0-B source scope, subject to review: a private Rust probe crate and C
header; minimal `Makefile`/`arch.mk` library/dependency rules; one checked call in
`kernel.c`; focused tests and a result report. No application ABI change,
memory allocator, driver, interrupt subsystem or linker-policy rewrite.

Main risks are mismatched code model/relocation, orphan sections, accidental
SIMD, unresolved or oversized runtime dependencies, incorrect panic semantics
and unsafe lifetime assumptions. The checks above address each; R0-A has not
measured their compiled outcome. The installed target's runtime libraries are
about 10.6 MiB before packaging; this is a host-artifact observation, not an
allocation or a loaded-image estimate. Forecast and measure one archive, C
objects, producer scratch, both media and evidence before R0-B builds, using
ordinary disk checks and preserving historical reservations/evidence.

Owner decisions needed before implementation:

1. Approve the staticlib/GNU-ld bridge, scalar probe and bounded source scope,
   or choose the object-only alternative with its extra dependency obligations.
2. Approve the kernel panic bridge and deliberate negative fixture; establish
   the exact added image/stack/resource bounds from the first bounded build.
3. Approve the focused BIOS/UEFI regression selection and a reproducible EFI
   producer configuration; no installations or physical-media writes are
   authorized by this report.
4. Review the completed R0-A result and separately authorize R0-B. USB.2 should
   evaluate Rust after that proof, with its DMA/input architecture separately
   designed; neither the whole Rust migration nor the stopped D1 transition is
   a prerequisite for unrelated bug fixes, boot-media work or desktop planning.

## Evidence and limits

Local read-only observations are retained under `build/r0a-assessment/`: compiler
version/commit, target specification/cfg, recognized candidate flags, installed
target-library sizes and hashes of 30 inspected source/ABI inputs. They are
assessment metadata, not executable test results. The initial bare `rustup`
query failed because PATH lacked it; the explicit existing binary succeeded.
The PIC cfg observation remains disclosed rather than being relabeled as a
successful static-relocation test.

Only this report is a repository change. Kernel, application, build and test
source, the Master Plan PR branch, local USB.1, the Windows original and the
interrupted D1 workspace remain unchanged. No R0-B object, archive, kernel,
image or screenshot was generated. Existing evidence and owner-reported partial
physical boot do not establish Rust execution or functional hardware support.
