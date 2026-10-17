# N1 — First Native User-Space Application (Implementation Plan)

| | |
|---|---|
| Status | **Approved direction; finalized plan.** Only stage N1.0 is authorized for implementation. Every later stage needs explicit approval. |
| Date | 2026-10-17 |
| Baseline | commit `828e5a1` (`docs: propose VOS native software architecture`) |
| Target | `ARCH=x86_64` only. `ARCH=i386` stays the default build and keeps its regression tests. |
| Related | [`docs/architecture/native-software-platform.md`](../architecture/native-software-platform.md) (long-term architecture) |

Labels used below: **[Implemented]** exists in the repository; **[Planned]** is part of this plan
and not yet implemented; **[Verify]** must be checked against a primary source or by experiment
when the stage is implemented. Nothing marked [Planned] is claimed to work.

**Numbering.** This milestone, "N1", covers rows N1–N8 of the architecture document's roadmap
(section 17). Its stages are numbered N1.0–N1.10 to avoid confusion with those rows.

---

## 1. Current kernel capabilities [Implemented, verified by reading the code at `828e5a1`]

| Area | State |
|---|---|
| Boot | GRUB loads the ELF64 kernel via Multiboot2. `arch/x86_64/boot.S` checks the magic and CPUID long-mode support, builds boot page tables, enables long mode and jumps to the higher half. |
| Page tables | One PML4: entry 0 (identity) and entry 511 (kernel alias via PDPT entry 510) share one page directory of 512 × 2 MiB pages with flags `0x83` (present, writable, supervisor-only, executable). Physical 0–1 GiB is visible at both `0x0` and `0xFFFFFFFF80000000`. |
| Segments | Boot GDT only (null, 0x08 code, 0x10 data) in identity-mapped `.boot.data`. No user segments, no TSS. |
| Interrupts | Disabled since `_start` (`cli`). No IDT, no exception handlers, no timer. |
| CPU state | Stage 0 observed `CR0 = 0x80000011` (CR0.WP clear) and `CR4 = 0x20` (only PAE; OSFXSR clear, so SSE instructions raise #UD). EFER.NXE is not set. |
| C kernel | `kernel.c`: serial (COM1) and VGA text output, bounds-checked Multiboot2 parsing (memory map, loader name), a 2-entry reservation table (kernel image, Multiboot2 info), self-checks, `VOS_TEST` exit through `isa-debug-exit`. |
| Linker layout | Low boot area at 1 MiB (VMA = LMA), higher-half kernel, explicit program headers, no RWX segment. |
| Tests | `make test-all`: i386 smoke, x86_64 smoke, strict test image at 128 MiB and 2 GiB, `-cpu qemu32` negative. |

**Missing infrastructure for N1:** kernel GDT/TSS, IDT and exception handling, panic path,
`memset`/`memcpy`, Multiboot2 module parsing, physical page allocator, page-table management,
identity-map removal, user address spaces, ring-3 entry and return, system calls, user-pointer
validation, ELF loading, a user-space build, user-space tests.

**Environment [Implemented/observed]:** WSL2 VM with 3.8 GiB RAM, 4 vCPUs; QEMU 10.2.1 in TCG
(KVM not usable); `qemu64` CPU model has NX but no SMEP/SMAP/UMIP; `-cpu max` has all four (QMP
query); GRUB `multiboot2.mod` provides `module2`; host gcc 15.2 can produce static `ET_EXEC`
ELF64 with a custom note (probe). GDB is not installed.

---

## 2. Objectives and non-goals

**Objective.** A separately built, freestanding x86_64 ELF64 program (`hello`), delivered as a
GRUB module, loaded by the kernel, executed in ring 3, prints `Hello from VOS!` through a system
call and exits through a system call, with the kernel surviving and verifying isolation.

**Non-goals for N1.** Timer and interrupt controller, scheduler, more than one process at a
time, SYSCALL/SYSRET, PIE/ASLR, dynamic linking, a shared `libvos`, kernel W^X with 4 KiB pages,
memory above 1 GiB, demand paging, FPU/SSE state, IPC, app/service managers, packages,
graphics, networking, compatibility layers, UEFI, SMP.

---

## 3. Final architecture decisions

| # | Decision | Rationale |
|---|---|---|
| F1 | System calls enter through `int $0x80` (interrupt gate, DPL 3) in ABI v0. SYSCALL is deferred. | The CPU switches to the TSS stack and `iretq` returns; no `swapgs`, no MSR setup, and no SYSRET return hazard (Xen XSA-7 / CVE-2012-0217: SYSRET to a non-canonical address made "Intel CPUs deliver the resulting exception in an undesirable processor state"). Arguments use SYSCALL-compatible registers so the switch later only changes private stubs. |
| F2 | Interrupts stay disabled for the whole of N1, including in ring 3 (RFLAGS.IF = 0, IOPL = 0). | No timer or scheduler is needed for one synchronous process. A user infinite loop is caught only by the test timeout (documented limitation). |
| F3 | Static non-PIE `ET_EXEC` ELF64, no relocations, with a mandatory VOS ABI note found through `PT_NOTE`. | Simplest correct loader; deterministic addresses for tests; no custom format (no blocker found). |
| F4 | The program is delivered as a GRUB `module2` and copied into fresh zeroed frames. Module memory is never mapped into user space. | Separately built; exercises reservations; no aliasing of loader-owned memory. |
| F5 | Bitmap physical page allocator for frames below 1 GiB, with conservative memory-map normalization (section 7). | Freeing is required (teardown, leak checks); all frames are reachable through the existing kernel alias, so N1 needs no new direct map. |
| F6 | Identity map removed before any user address space exists; user space owns the whole lower half. | The identity map occupies PML4 entry 0, which user mappings need. |
| F7 | NX is mandatory for user mode (refuse otherwise); CR0.WP is set; SMEP and UMIP are enabled when CPUID reports them. **SMAP is not enabled until the prerequisites in section 9.4 are implemented and tested.** | NX is required for W^X user mappings; SMEP/UMIP have no kernel prerequisites; SMAP needs safe user-copy and entry-path handling first. |
| F8 | User programs in ABI v0 may not use FPU/SSE (built with `-mgeneral-regs-only`; CR4.OSFXSR stays clear, so any use raises #UD). | The kernel saves no FPU state in N1. |
| F9 | Kernel fault-injection test cases are selected at **compile time** (`VOS_TEST_CASE=<name>`, introduced in N1.0), not through the kernel command line. | Command-line parsing arrives only in N1.2; earlier stages need injection too. |
| F10 | Ring-3 isolation is verified primarily with **CPU-generated evidence** (section 10). QEMU interrupt logs are optional diagnostics and never a pass criterion. | QEMU's log format is not a documented, stable interface. |
| F11 | Kernel code for N1 stays in `arch/x86_64/`; user programs live under `user/`. | No second user-space architecture exists yet. |

---

## 4. Stages and dependency order

```
N1.0 -> N1.1 -> N1.2 -> N1.3 -> N1.4 -> N1.5 -> N1.6 -> N1.8 -> N1.9 -> N1.10
N1.0 -> N1.7 (user build; no kernel dependency) -> N1.8
```

Each stage ends with `make test-all` green and the i386 kernel binary byte-identical to the
pre-stage build (all N1 work is x86_64-only). File names for new files are proposals until the
stage is implemented.

### N1.0 — Kernel infrastructure preparation [Planned]
- Move port I/O and MSR helpers into `arch/x86_64/cpu.h`; address-layout constants into
  `arch/x86_64/memlayout.h`; serial/VGA code into `arch/x86_64/console.{c,h}`.
- Add freestanding `memset`, `memcpy`, `memcmp`, `strlen` (`arch/x86_64/lib.{c,h}`).
- Add `panic()` (`arch/x86_64/panic.{c,h}`): disables interrupts, reports on serial and VGA,
  halts forever in production; in `VOS_TEST` builds prints `VOS-TEST: FAIL` and exits QEMU with
  status 35. Recursive panics halt immediately.
- Compile-time test cases (`VOS_TEST_CASE`), first case `panic`.
- `scripts/exit-test.sh`: several markers (`MARKER... --` form), optional `DIAG=1` diagnostics,
  unchanged behaviour for existing calls.
- **Acceptance:** existing serial output unchanged apart from layout-dependent addresses; the
  `panic` case exits 35 with `panic: test case: deliberate panic` and `VOS-TEST: FAIL`; i386
  binary byte-identical.

### N1.1 — CPU protections, kernel GDT/TSS, IDT, exceptions (kernel mode only) [Planned]
- CR0.WP = 1; require NX (CPUID 0x80000001 EDX bit 20) and set EFER.NXE; enable SMEP and UMIP
  when present. Log the resulting feature set.
- GDT in `.data`: see section 6.2. TSS with RSP0 stack (16 KiB), IST1 stack for #DF (4 KiB), I/O
  map base beyond the TSS limit (no I/O permission bitmap).
- IDT: vectors 0–31 with DPL 0 (so `int n` from ring 3 raises #GP instead of forging an
  exception frame); vector 0x80 stays not-present until N1.5. Entry stubs: push a dummy error
  code for vectors without one, save all registers, `cld`, align RSP to 16 before calling C.
- Kernel-mode exceptions panic with vector, error code, RIP, CS, RFLAGS, CR2.
- **Tests:** compile-time cases `kernel-ud` (exit 35, `panic: #UD`, saved CS 0x08), optional
  `kernel-df` (IST1 used); `-cpu qemu64,-nx` refuses to continue (exit 35, `CPU lacks NX`).

### N1.2 — Multiboot2 modules, command line, reservations [Planned]
- Parse module tags (type 3: `mod_start`, `mod_end`, NUL-terminated command line) and the
  command-line tag (type 1) with the same bounds checks as today's parser.
- Enlarge the reservation table; reserve every module and physical 0–1 MiB.
- Reject modules outside usable RAM or at or above 1 GiB.
- Makefile/ISO hooks: per-architecture extra ISO files and GRUB configuration (defaults keep
  today's behaviour).

### N1.3 — Physical page allocator [Planned]
- Bitmap over frames below 1 GiB, normalization rules of section 7, zeroed frames on
  allocation, double-free and range checks, free counter, poisoning in test builds.
- **Tests:** synthetic memory-map self-test (section 7.3); 128 MiB and 2 GiB boots;
  allocate/free round trip restores the free count; no allocation intersects a reservation.

### N1.4 — Page tables, layout, identity-map removal [Planned]
- Map/unmap/translate with intermediate-table allocation; `invlpg` on unmap; audit function.
- Move to the kernel GDT (N1.1) first, then clear PML4 entry 0 and reload CR3.
- **Tests:** case `kernel-lowaccess` (low address after removal → #PF panic, exit 35); mapper
  self-tests; audit passes.

### N1.5 — Process object and first ring-3 entry [Planned]
- Address space = new PML4 with only entry 511 copied from the kernel PML4.
- `enter_user()`: save callee-saved registers and RSP, set TSS.RSP0, load CR3, zero all GPRs,
  `iretq` with SS 0x1B, RSP per section 8.3, RFLAGS 0x2, CS 0x23, RIP = entry.
- Vector 0x80 present with DPL 3; `exit` only. Trap classification by the saved CS RPL: RPL 3 →
  kill process, load kernel CR3, free user memory, return to `enter_user()`'s caller; RPL 0 →
  panic.
- First user code: a few bytes copied from a kernel template into a fresh user page (no ELF yet).

### N1.6 — System-call ABI v0 and user-memory access [Planned]
- `exit`, `debug_write` (section 8.1). `copy_from_user` per section 9.3, written so that SMAP
  bracketing and an exception fixup can be added without changing callers. SMAP stays disabled.

### N1.7 — User runtime and `hello` (build only) [Planned]
- `user/` tree: `crt0.S` (section 8.3), system-call stubs (the only place `int $0x80` appears),
  ABI note, linker script with explicit page alignment, `hello.c`, build-time ELF checks
  (`readelf`, already installed).

### N1.8 — Static ELF64 loader, `hello` runs [Planned]
- Validation of section 8.4 before any allocation; copy segments; map with section 6.1
  permissions; map stack; enter. Production GRUB configuration adds `module2 /boot/hello.elf hello`.

### N1.9 — Isolation and fault suite, cleanup, SMAP enablement [Planned]
- Test ISO with one module per scenario, expectations in module command lines, kernel-side
  evaluation, final `hello` rerun and leak check (section 11).
- SMAP enablement only after section 9.4 prerequisites are met; exercised with `-cpu max`.

### N1.10 — Documentation [Planned]
- ABI v0 reference, architecture document roadmap/status update, README.

---

## 5. Kernel/user boundary responsibilities

| Kernel (N1) | User runtime (N1: `crt0`, stubs, `strlen`) | Later `libvos` |
|---|---|---|
| CPU setup, GDT/TSS/IDT, exception dispatch, page allocator, page tables, ELF loading of the module, system-call dispatch, user-pointer validation, process teardown, test verdicts | Process entry, calling `main`, calling `exit` with `main`'s result, system-call stubs | Memory allocation, formatted output, error helpers, IPC, launching |

Permission decisions never live in user-space code.

---

## 6. Memory layout and segments

### 6.1 Virtual memory layout [Planned]

| Range | Use | Permissions |
|---|---|---|
| `0x0000000000000000`–`0x00000000003FFFFF` | Never mapped (null-pointer guard) | — |
| `0x0000000000400000`–`0x000000003FFFFFFF` | User image (`PT_LOAD` segments), total ≤ 1 MiB in N1 | user; RW only if `PF_W`; NX unless `PF_X`; never W+X |
| `0x00007FFFFFFFA000`–`0x00007FFFFFFFAFFF` | Stack guard page | unmapped |
| `0x00007FFFFFFFB000`–`0x00007FFFFFFFEFFF` | User stack, 16 KiB | user, RW, NX |
| `0x00007FFFFFFFF000`–`0x00007FFFFFFFFFFF` | Highest canonical lower-half page | unmapped (defensive) |
| `0x0000800000000000`–`0xFFFF7FFFFFFFFFFF` | Non-canonical | — |
| `0xFFFFFFFF80000000`–`0xFFFFFFFFBFFFFFFF` | Kernel alias of physical 0–1 GiB [Implemented] | supervisor only; RW; executable (kernel W^X deferred) |

- User PML4: entries 0–255 user-owned; entry 511 copied **by value** from the kernel PML4 and
  never given the U/S bit. Any future kernel PML4 entry must exist before user address spaces
  are created, or those spaces will not see it (record for later stages).
- Every user frame and page-table frame is zeroed on allocation.
- The kernel accesses page-table frames and user frames only through the kernel alias, which
  is why the allocator is limited to frames below 1 GiB.

### 6.2 GDT and TSS [Planned]

| Offset | Descriptor | Selector used |
|---|---|---|
| 0x00 | null | — |
| 0x08 | kernel code, 64-bit, DPL 0 | 0x08 |
| 0x10 | kernel data, DPL 0 | 0x10 |
| 0x18 | user data, DPL 3 | 0x1B |
| 0x20 | user code, 64-bit, DPL 3 | 0x23 |
| 0x28 | TSS (16-byte system descriptor, available 64-bit TSS) | 0x28 |

- Order chosen so a later SYSCALL/SYSRET setup can use STAR without reshuffling. [Verify]
  STAR selector arithmetic against the Intel SDM when SYSCALL is adopted.
- TSS: RSP0 = top of a dedicated 16 KiB kernel entry stack; IST1 = 4 KiB #DF stack; I/O map base
  set beyond the TSS limit so there is no I/O permission bitmap. [Verify] that with IOPL 0 and no
  bitmap every `in`/`out` at CPL 3 raises #GP.
- Stack canaries at the bottom of every kernel stack, checked after each user run and on panic
  (no guard pages are possible inside 2 MiB kernel pages).

---

## 7. Physical memory normalization (concern B)

### 7.1 Inputs and threats
The Multiboot2 memory map may be unsorted, contain overlapping entries, give contradictory types
for the same range, contain zero-length entries, or have `base + length` overflow. Additional
"in use" information comes from the reservation table (kernel image, Multiboot2 info, modules,
physical 0–1 MiB).

### 7.2 Algorithm [Planned]
The allocator bitmap covers frames `[0, 1 GiB)`. Initialization is order-independent and
"unusable wins":

1. **Validate the tag.** `entry_size >= 24` and a multiple of 8 [Implemented]; `entry_version`
   must be 0, otherwise the allocator refuses to initialize (panic) rather than guess.
2. **Mark every frame used.**
3. **Free pass.** For each entry with type 1 (available): skip `length == 0`; if `base + length`
   overflows, skip the entry; clip to `[1 MiB, 1 GiB)`; round the start **up** and the end **down**
   to 4 KiB; clear the bits for the remaining whole frames.
4. **Protect pass.** For each entry with any other type (reserved, ACPI reclaimable, ACPI NVS,
   bad memory, unknown): if `base + length` overflows, treat the end as `2^64`; round the start
   **down** and the end **up**; set the bits for every intersecting frame below 1 GiB.
5. **Reservation pass.** Set the bits for every reservation, rounded outward.
6. **Count and report** free frames; in test builds assert that no free frame intersects any
   non-available entry or reservation.

Because steps 4 and 5 run after step 3 and only ever set bits, overlapping or contradictory
entries always resolve to "not allocatable", regardless of order. ACPI-reclaimable memory is
treated as unusable in N1.

### 7.3 Tests [Planned]
- A `VOS_TEST` self-test runs the same normalization code on synthetic maps into a small test
  bitmap: unsorted entries, available range overlapping a reserved one (both orders), duplicate
  entries, zero length, overflow (`base = 2^64 - 4096`), unaligned bounds, an unknown type, and a
  reservation inside an available range. Expected bitmaps are fixed in the test.
- Boot with 128 MiB and 2 GiB: the free count equals the computed expectation; allocations never
  intersect reservations; RAM above 1 GiB is reported as ignored.

---

## 8. ABI v0 and executable requirements

### 8.1 System calls [Planned]
- Entry: `int $0x80`. Number in `rax`; arguments in `rdi, rsi, rdx, r10, r8, r9`; result in
  `rax` (non-negative = success, negative = VOS error).
- **Clobbers:** `rax`, `rcx`, `r11` (reserving `rcx`/`r11` now keeps stubs valid when SYSCALL,
  which overwrites them, replaces `int $0x80`). All other general-purpose registers and RFLAGS
  are preserved.
- Unused argument registers are ignored.

| Number | Name | Arguments | Result |
|---|---|---|---|
| 0 | `exit` | `rdi`: status (low 32 bits, signed) | does not return |
| 1 | `debug_write` | `rdi`: buffer, `rsi`: length (≤ 256) | bytes written, or `-VOS_EFAULT` / `-VOS_EINVAL` |
| other | — | — | `-VOS_ENOSYS` |

Error values are VOS-specific (`VOS_ENOSYS = 1`, `VOS_EINVAL = 2`, `VOS_EFAULT = 3`, returned
negated), not Linux numbers. Output from `debug_write` is sanitized (printable ASCII and newline)
and prefixed by the kernel with `user[<name>]: `.

ABI v0 is internal and unstable. Applications call only the stubs (`vos_exit`,
`vos_debug_write`); the stubs become part of `libvos` later, and the ABI version in the ELF note
lets a later loader reject v0 binaries.

### 8.2 Process entry state [Planned]
| Item | Value |
|---|---|
| RIP | `e_entry` |
| RSP | `0x00007FFFFFFFEFF0` (16-byte aligned; the 16 bytes at RSP are zero, i.e. `argc = 0`, no `argv`) |
| General-purpose registers | all zero, including `rbp` and `rdx` (no `atexit` function) |
| RFLAGS | `0x2` (IF = 0, IOPL = 0, DF = 0, AC = 0) |
| CS / SS | `0x23` / `0x1B` |
| FS/GS base, TLS | none |
| FPU/SSE | unavailable (#UD) |

### 8.3 Stack alignment (concern A)
**[Fact]** The System V AMD64 psABI requires the stack to be "16 (32 or 64) byte aligned
immediately before the call instruction is executed", so that at function entry `(%rsp + 8)` is a
multiple of 16; it guarantees `%rsp` is 16-byte aligned at process entry; the direction flag
"must be clear" on function entry and return; and user code "should mark the deepest stack frame
by setting the frame pointer to zero". The 128 bytes below `%rsp` (red zone) "shall not be
modified by signal or interrupt handlers". (Source: psABI `low-level-sys-info.tex`, see
References.)

**[Planned] `crt0` (`_start`):**
```
_start:
    xor  %ebp, %ebp        # deepest frame marker
    and  $-16, %rsp        # defensive: already aligned by the kernel
    call main              # RSP 16-aligned before call -> (RSP + 8) % 16 == 0 in main
    mov  %eax, %edi
    call vos_exit          # still aligned; does not return
    ud2
```
- System-call stubs are leaf functions that do not touch the stack; `int $0x80` from CPL 3
  switches to RSP0, so the CPU never writes to the user stack and the red zone stays intact.
- Kernel side: the existing boot path already calls `kernel_main` with an aligned stack
  [Implemented]. Exception and system-call entry stubs will explicitly align RSP to 16 before
  calling C instead of relying on frame-size arithmetic. [Verify] that 64-bit interrupt delivery
  aligns RSP to 16 before pushing the frame; the explicit alignment makes the design independent
  of it.
- **Test:** `t_stack_align` declares a 16-byte-aligned local array in `main` and in a nested
  call and checks its address; GCC relies on the incoming alignment, so a misaligned entry fails.

### 8.4 ELF64 requirements (concern E) [Planned]
All arithmetic is overflow-checked, and validation finishes before any allocation. Section
headers are ignored entirely; nothing is read through `e_shoff`.

| Item | Requirement |
|---|---|
| File | size ≤ 1 MiB and ≥ 64 bytes |
| `e_ident` | `\x7fELF`; `ELFCLASS64`; `ELFDATA2LSB`; `EV_CURRENT`; `EI_OSABI = 0`; `EI_ABIVERSION = 0` |
| Header | `e_type = ET_EXEC` (ET_DYN and others rejected); `e_machine = EM_X86_64`; `e_version = 1`; `e_flags = 0`; `e_ehsize = 64`; `e_phentsize = 56`; `1 ≤ e_phnum ≤ 8`; program-header table inside the file |
| `PT_LOAD` | 1–4 segments; `p_align = 0x1000`; `p_vaddr` and `p_offset` both page-aligned; `0 < p_memsz`; `p_filesz ≤ p_memsz`; file range inside the file; `[p_vaddr, p_vaddr + p_memsz)` inside `[0x400000, 0x40000000)`; sorted by `p_vaddr` and page ranges disjoint; flags ⊆ {R, W, X}, R required, never W and X together; total memory ≤ 1 MiB; at least one executable segment |
| Entry | inside the file-backed part of an executable `PT_LOAD` |
| `PT_NOTE` | file range inside the file; `p_align = 4`; parsed by walking notes through the program header (not sections) |
| `PT_GNU_STACK` | accepted only without `PF_X`; otherwise rejected |
| Rejected types | `PT_INTERP`, `PT_DYNAMIC`, `PT_TLS`, `PT_PHDR`, `PT_SHLIB`, `PT_GNU_RELRO`, `PT_GNU_PROPERTY`, `PT_GNU_EH_FRAME`, any unknown type |

**VOS ABI note format.** The gABI text describes 8-byte note words for 64-bit objects, but the
GNU toolchain and glibc's `Elf64_Nhdr` use three 32-bit words. VOS defines its note explicitly in
the toolchain's form:

| Field | Value |
|---|---|
| `n_namesz` (u32) | 4 |
| `n_descsz` (u32) | 4 |
| `n_type` (u32) | 1 (`VOS_NOTE_ABI`; note types are scoped by the owner name) |
| name | `"VOS\0"`, padded to 4 bytes |
| desc | u32 ABI version, `0` for ABI v0 |

**Note walk:** while at least 12 bytes remain in the `PT_NOTE` data: read the header; compute
`align4(namesz)` and `align4(descsz)` with overflow checks; reject if they exceed the remaining
bytes; if name is `"VOS\0"` and type is 1: require `descsz = 4` and version 0, and reject a second
VOS ABI note; other notes are skipped but must be well-formed. Trailing bytes that cannot hold a
header are rejected. A file without exactly one valid VOS ABI note is rejected.

**Loading:** for each `PT_LOAD`, allocate zeroed frames, copy `p_filesz` bytes from the module,
leave the rest zero, map user-accessible with W only for `PF_W` and NX unless `PF_X`.

---

## 9. Security boundaries

### 9.1 Properties N1 is designed to establish [Planned — none verified yet]

| Property | Mechanism |
|---|---|
| User code cannot read or write kernel pages | Kernel mappings never carry U/S; PML4 entry 511 copied without U/S; audit in test builds |
| User code cannot execute privileged instructions or do port I/O | CPL 3, IOPL 0, no I/O bitmap |
| User code cannot forge exception frames | Only vector 0x80 has DPL 3; `int n` for any other vector raises #GP |
| Entry/return use a correct stack | TSS.RSP0 for CPL 3 → CPL 0; IST1 for #DF; interrupts off throughout |
| Kernel never runs with user-controlled DF/AC | Every entry stub executes `cld` (and `clac` once SMAP is enabled) |
| No kernel data leaks into user space | GPRs zeroed before entering user mode; all user frames zeroed |
| No bypass of the system-call interface | User address spaces contain only copied segments and the stack: no I/O, VGA, MMIO or kernel mappings |
| User pointers validated | Section 9.3 |
| User faults never corrupt kernel state | RPL-3 traps kill the process and tear down its address space; RPL-0 traps panic |

### 9.2 Additional review findings incorporated
- **User-controlled RFLAGS bits on entry.** A user can set DF (and AC) before `int $0x80`; the
  kernel's C code requires DF = 0 (psABI). Entry stubs clear them unconditionally, so correctness
  does not depend on what interrupt delivery clears. [Verify] the exact flags cleared by 64-bit
  interrupt delivery in the Intel SDM.
- **Return path.** `iretq` frames for user entry use only kernel constants for CS/SS. On return
  from a system call or exception, the saved user frame is reused unmodified except for `rax`;
  test builds assert saved RFLAGS has IOPL 0 and IF 0 before `iretq`.
- **Traps raised while running kernel code on behalf of a process** (RPL 0) always panic; they
  are never treated as user faults.
- **NMI and machine check** run on the current stack in N1 (no IST). Acceptable with interrupts
  disabled and a single process; IST stacks for them are deferred.
- **Single-thread assumption.** Validation of user memory followed by a copy is race-free only
  because there is one thread, interrupts are off and there is no demand paging.

### 9.3 `copy_from_user` [Planned]
1. `len > limit` → `-VOS_EINVAL`; `len == 0` → success.
2. `end = ptr + len` must not overflow; `[ptr, end)` must lie in `[0x400000, 0x0000800000000000)`.
3. Walk the current user page tables for every page in the range: present and U/S at every
   level (and writable for future `copy_to_user`); otherwise `-VOS_EFAULT`.
4. Copy once into a kernel buffer; later code uses only the kernel copy.
5. Written as a single routine so SMAP bracketing and an exception fixup can be added without
   touching callers.

### 9.4 SMAP prerequisites and tests (concern D)
SMAP stays **disabled** until all of these exist:
1. All user-memory access goes through `copy_from_user`/`copy_to_user`; a grep-based check
   rejects direct dereferences of user addresses in kernel code.
2. Those routines execute `stac` immediately before and `clac` immediately after the access.
3. An exception fixup table: a fault inside the copy routine resumes at a fixup that executes
   `clac` and returns `-VOS_EFAULT`.
4. Every kernel entry path (exceptions, system call) executes `clac`, because RFLAGS.AC on entry
   is user-controlled.
5. `stac`/`clac` run only when CPUID leaf 7 reports SMAP, since they raise #UD otherwise.
6. CR4.SMAP is set only after 1–5.

Tests (with `-cpu max`; `qemu64` has no SMAP): a `VOS_TEST_CASE` that reads a mapped user page
without `stac` must panic with a supervisor page fault; a user program that sets AC with
`popf` before a valid `debug_write` must still see correct behaviour and the injected kernel
access must still fault; an invalid pointer must return `-VOS_EFAULT` with AC clear afterwards.

### 9.5 Not guaranteed by N1
Kernel W^X; SMEP/SMAP/UMIP on the default `qemu64` model (absent there); protection against
speculative-execution leaks (kernel mapped supervisor-only in every user address space, no
page-table isolation; untestable in TCG); ASLR/KASLR; kernel stack guard pages; preemption (an
infinite user loop hangs the system); use of RAM above 1 GiB; thread-safe user-memory access.

---

## 10. Ring-3 verification (concern C)

Primary evidence is generated by the CPU and checked by the kernel's test logic and the
harness:

| Evidence | Why it proves privilege isolation |
|---|---|
| Saved CS = 0x23 and SS = 0x1B in the hardware-pushed frame of every system call and user exception | The CPU pushes the interrupted CS/SS; ring-0 code would show 0x08/0x10 |
| `cli`, `hlt`, `mov %cr3`, `wrmsr` from the program raise #GP with saved CS 0x23 | At CPL 0 these succeed; a #GP can only come from CPL > 0 |
| `int $14` from the program raises #GP with error code `0x72` (vector 14, IDT bit) | Proves the DPL check on exception gates |
| User write to a kernel address raises #PF with error code P=1, W=1, U=1 | P=1 shows the page exists and protection, not absence, blocked the access |
| Instruction fetch from the user stack raises #PF with P=1, I/D=1 | NX enforced |
| After each fault the kernel runs `hello` again, the free-frame count equals the baseline and stack canaries are intact | Recovery without corrupted state |

The `user[<name>]: ` prefix is added only in the system-call path after checking the saved CS
RPL is 3, so kernel-mode printing cannot produce it. QEMU's `-d int` log (`DIAG=1`) may be kept
as supplemental evidence for humans; no test depends on its format.

---

## 11. Testing and acceptance

All runs are headless and sequential, 128 MiB unless stated. No test runs QEMU instances in
parallel.

| ID | Stage | Test | Pass criterion |
|---|---|---|---|
| R1–R5 | all | Existing suite | Unchanged markers and statuses; i386 binary byte-identical to the pre-stage build |
| P0 | N1.0 | `VOS_TEST_CASE=panic` | Exit 35, `panic: test case: deliberate panic`, `VOS-TEST: FAIL` |
| K1 | N1.1 | `kernel-ud` | Exit 35, `panic: #UD`, saved CS 0x08 |
| K2 | N1.1 | `-cpu qemu64,-nx` | Exit 35, `CPU lacks NX` |
| M1 | N1.3 | Synthetic memory maps | All fixed expectations match |
| K3 | N1.4 | `kernel-lowaccess` | Exit 35, #PF panic at the low address |
| U1 | N1.8 | `hello` | `user[hello]: Hello from VOS!` and `exited status 0` |
| U2 | N1.5/N1.8 | Ring-3 frame | Saved CS 0x23, SS 0x1B, RIP inside the program's executable segment |
| U3 | N1.6 | Invalid arguments | NULL, kernel address, unmapped, partly mapped, overflowing, non-canonical, oversize → expected errors, logged by the kernel; unknown number → `-VOS_ENOSYS` |
| U4 | N1.9 | Kernel write / read | #PF P=1,W=1,U=1 / P=1,U=1; target bytes unchanged; process killed |
| U5 | N1.9 | Privileged instructions | #GP, saved CS 0x23 |
| U6 | N1.9 | `outb` to COM1 | #GP; no byte reaches the serial log |
| U7 | N1.9 | Execute stack/data; write VGA | #PF P=1,I/D=1; #PF P=0 |
| U8 | N1.9 | Write own text | #PF P=1,W=1,U=1 |
| U9 | N1.9 | Stack overflow | #PF inside the guard page |
| U10 | N1.9 | `int $14` | #GP error code 0x72 |
| U11 | N1.9 | DF set before `debug_write` | Correct output (proves `cld` on entry) |
| U12 | N1.9 | Stack alignment | `t_stack_align` exits 0 |
| U13 | N1.9 | Malformed ELF (header, segments, overlap, W+X, kernel-half address, missing/duplicate/malformed VOS note, wrong ABI version, `PT_INTERP`) | Each rejected with its reason; no frames leaked |
| U14 | N1.9 | Cleanup | Final `hello` rerun passes; free count equals baseline; canaries intact |
| U15 | N1.9 (optional) | `-cpu max` | SMEP/UMIP on; SMAP on only if section 9.4 is complete; suite passes; user `sgdt` raises #GP |

**N1 is complete when:** `make test-all` runs R1–R5, P0, K1–K3, M1 and U1–U14 (and U15 if
approved) sequentially in TCG and all pass; `make ARCH=x86_64 run` shows the separately built
`hello` printing through `debug_write` from ring 3; the i386 binary is unchanged; section 9.5 and
ABI v0 are documented.

---

## 12. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Error-code mismatch in exception stubs corrupts frames | Table-driven stubs; tests with and without error codes; DPL 0 gates |
| TSS/`ltr` mistakes → triple fault | `-d cpu_reset` logs, QEMU monitor `info registers`; GDB if approved |
| Kernel entry stack overflow or overlap | Separate static stacks; canaries |
| U/S leaks onto a kernel entry | PML4 entry copied by value; audit in test builds; U4 |
| Allocator returns a protected frame | Section 7 rules; test-build assertions; M1 |
| Stale TLB entries after teardown | Switch CR3 before freeing; `invlpg`; no global pages |
| ELF parser overflow or confusion | Checked arithmetic; strict tables; no section headers; U13 |
| Kernel dereferences user pointers directly (no SMAP on `qemu64`) | Single copy routine; grep check; SMAP tests on `-cpu max` |
| User-controlled DF/AC on entry | `cld`/`clac` in every entry stub; U11 |
| Kernel register leakage | GPRs zeroed on entry to user mode |
| Serial output injection | Sanitized, kernel-prefixed output |
| False-positive tests | CPU-generated evidence (section 10); validity rules |
| Host gcc defaults (default PIE, CET) | Explicit flags; build-time `readelf` checks |
| Memory/time in WSL2 + TCG | 128 MiB guests, sequential runs, one suite ISO |

---

## 13. Commit sequence

| # | Commit | Stage |
|---|---|---|
| 1 | `docs: finalize N1 user-space implementation plan` | plan |
| 2 | `test: improve x86_64 exit-test diagnostics` | N1.0 |
| 3 | `refactor(x86_64): prepare kernel infrastructure for user space` | N1.0 |
| 4 | `feat(x86_64): kernel GDT/TSS, IDT and exception handling` | N1.1 |
| 5 | `feat(x86_64): parse Multiboot2 modules and reserve them` | N1.2 |
| 6 | `feat(x86_64): add bitmap physical page allocator` | N1.3 |
| 7 | `feat(x86_64): add page-table manager and remove the identity map` | N1.4 |
| 8 | `feat(x86_64): enter ring 3 with a built-in test page` | N1.5 |
| 9 | `feat(x86_64): add system-call ABI v0 with user-pointer validation` | N1.6 |
| 10 | `feat(user): add separately built user runtime and hello` | N1.7 |
| 11 | `feat(x86_64): load static ELF64 modules and run hello` | N1.8 |
| 12 | `test(x86_64): add user isolation and fault test suite` | N1.9 |
| 13 | `docs: document N1, ABI v0 and update the roadmap` | N1.10 |

The test-script commit precedes the refactor because the new `panic` test uses the
multiple-marker form.

---

## 14. Deferred features and later milestones
Timer and interrupt controller; preemptive scheduler and multiple processes; SYSCALL/SYSRET with
`swapgs` and a canonical-address check before SYSRET; IST stacks for NMI/#MC; PIE, ASLR and a
user-space loader; shared `libvos` and API levels; kernel remap with 4 KiB W^X pages and guard
pages; direct map of all RAM; FPU/SSE state management; SMAP (once section 9.4 is met); IPC
objects, app and service managers, packages (architecture document sections 10–13).

---

## 15. Open decisions (unchanged, pending)
- Licence: `MIT OR Apache-2.0` remains the provisional recommendation; no licence file is added
  until an explicit decision.
- GDB installation (recommended before N1.1; not installed).
- Whether U15 (`-cpu max`, SMAP) is part of N1 or a follow-up.

## References
- System V AMD64 psABI, `low-level-sys-info.tex` —
  https://gitlab.com/x86-psABIs/x86-64-ABI/-/raw/master/x86-64-ABI/low-level-sys-info.tex
  (stack alignment, red zone, process entry, DF; fetched 2026-10-17)
- System V gABI, program header and note section —
  https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.pheader.html (note layout, `PT_LOAD`
  congruence and ordering; fetched 2026-10-17)
- glibc `<elf.h>` on the development host: `Elf64_Nhdr` uses `Elf64_Word` (`uint32_t`) fields
  (checked locally 2026-10-17)
- GRUB `include/multiboot2.h` (public source mirror) —
  https://raw.githubusercontent.com/rhboot/grub2/master/include/multiboot2.h (module tag type 3
  layout; fetched 2026-10-17)
- Xen Security Advisory XSA-7 (CVE-2012-0217) — https://xenbits.xen.org/xsa/advisory-7.html
- Intel 64 and IA-32 Architectures Software Developer's Manual —
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html (items
  marked [Verify] must be checked here; the manual was not text-searchable in this session)
