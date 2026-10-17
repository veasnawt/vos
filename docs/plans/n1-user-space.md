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

### N1.0 — Kernel infrastructure preparation [Implemented and verified]
- Move port I/O and MSR helpers into `arch/x86_64/cpu.h`; address-layout constants into
  `arch/x86_64/memlayout.h`; serial/VGA code into `arch/x86_64/console.{c,h}`.
- Add freestanding `memset`, `memcpy`, `memcmp`, `strlen` (`arch/x86_64/lib.{c,h}`).
- Add `panic()` (`arch/x86_64/panic.{c,h}`): disables interrupts, reports on serial and VGA,
  halts forever in production; in `VOS_TEST` builds prints `VOS-TEST: FAIL` and exits QEMU with
  status 35. Recursive panics halt immediately.
- Compile-time test cases (`VOS_TEST_CASE`), first case `panic`.
- `scripts/exit-test.sh`: several markers (`MARKER... --` form), optional `DIAG=1` diagnostics,
  unchanged behaviour for existing calls.
- **Status:** implemented as described, verified 2026-10-17 with `make test-all` (all passing,
  including P0). `VOS_TEST_CASE=<name>` requires `VOS_TEST=1` and builds into
  `build/x86_64-test-<name>/`; `exit-test.sh` takes `MARKER... --` (the single-marker form is
  unchanged). Production serial output differs from `828e5a1` only in `rip` and the
  Multiboot2 info address. i386 `vos.bin` SHA-256 `0760e7eb…9dd` is identical to a clean build of
  `828e5a1`. Not covered: the VGA panic text was not inspected.
- **Acceptance:** existing serial output unchanged apart from layout-dependent addresses; the
  `panic` case exits 35 with `panic: test case: deliberate panic` and `VOS-TEST: FAIL`; i386
  binary byte-identical.

### N1.1 — CPU protections, kernel GDT/TSS, IDT, exceptions (kernel mode only) [Implemented and verified]
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
- **Status (verified 2026-10-17):** implemented in `cpu.c`, `gdt.c`, `idt.c`, `exception.c`
  and `entry.S`; all of `make test-all` passes. Differences from the text above and results:
  - `VOS_TEST_CASE=kernel-ud` / `kernel-df` map `-` to `_` in the `-DVOS_TEST_CASE_<name>`
    macro (hyphens are not valid in C identifiers).
  - `kernel-df` was implemented: the #GP gate is made not-present and a bad `mov %ds` forces
    #GP -> #DF; the report shows `stack=ist1` (the frame lies in the IST1 stack).
  - SMEP/UMIP were observed on only under `-cpu max` (`cr4=0x100820`); `qemu64` has neither.
    Default CPU: `cr0=0x80010011 efer=0xd00`.
  - GDT/TSS evidence: `tr=0x28`, descriptor type becomes busy after `ltr`, IST1 and RSP0
    printed, IOPB base 104 > limit 103.
  - Not done: a user-mode `sgdt` to prove UMIP (needs ring 3, N1.9); SMAP stays disabled;
    NMI/#MC still use the normal stack (IST for them is deferred); kernel W^X is not provided
    (2 MiB executable-writable kernel alias).

### N1.2 — Multiboot2 modules, command line, reservations [Implemented and verified]
- Parse module tags (type 3: `mod_start`, `mod_end`, NUL-terminated command line) and the
  command-line tag (type 1) with the same bounds checks as today's parser.
- Enlarge the reservation table; reserve every module and physical 0–1 MiB.
- Reject modules outside usable RAM or at or above 1 GiB.
- Makefile/ISO hooks: per-architecture extra ISO files and GRUB configuration (defaults keep
  today's behaviour).
- **Status (verified 2026-10-17):** `bootinfo.c` (`mb2_parse`, `boot_reserve`), `resv.c`, and
  `selftest.c` (VOS_TEST only). `make test-all` passes, including 78 synthetic parser/reservation
  cases (mutation-checked: disabling the non-available-entry check, the module range, 1 GiB,
  count and NUL checks each makes the self-test fail) and the `modules` image.
  - Parser: tags need `size >= 8` and `<= remaining`; the end tag must be exactly 8 bytes;
    command-line and module strings must end in NUL inside the tag; a duplicate command-line or
    memory-map tag, a missing end tag or memory map, an empty/reversed module, a module end
    above 1 GiB and more than 4 modules are rejected. An absent command line is `NULL`.
  - Reservations (`resv.h`): 16 entries `[start, end)`, rounded outward, overlaps allowed and
    meaning their union; a full table or a bad range returns an error and changes nothing. Added:
    0–1 MiB, kernel image, info block, each module. Any error is a panic with the reason.
  - A module (and the kernel and info ranges) must lie wholly in one available entry and touch no
    non-available entry of any type; modules must not overlap the kernel's page span, the info
    block (bytes) or each other. The kernel's span contains holes in which GRUB puts the info
    block, so kernel/info reservations overlap at page level by design.
  - `module2` delivery: the header now carries a module-alignment tag. Observed with GRUB 2.14:
    modules are page-aligned (e.g. `0x119000`, `0x11a000`), directly after the kernel span; the
    kernel command line contains only the arguments (`vos.test=1`), the module command line only
    its own (`mod.arg=first`), and the production entry reports an empty command line. The parser
    does not require alignment; the `modules` test does.
  - Fixture: `tests/fixtures/n12-module.bin` (21 harmless bytes) and
    `tests/x86_64/modules.{mk,grub.cfg}`; the Makefile gained `GRUB_CFG` / `ISO_FILES` hooks whose
    defaults leave the production ISO unchanged.
  - Not done: the allocator itself (N1.3); `entry_version` of the memory map is not yet checked
    (N1.3 refuses version != 0); modules above 1 GiB are rejected, not supported.

### N1.3 — Physical page allocator [Implemented and verified]
- Bitmap over frames below 1 GiB, normalization rules of section 7, zeroed frames on
  allocation, double-free and range checks, free counter, poisoning in test builds.
- **Tests:** synthetic memory-map self-test (section 7.3); 128 MiB and 2 GiB boots;
  allocate/free round trip restores the free count; no allocation intersects a reservation.
- **Status (verified 2026-10-17):** `pmm.{c,h}`, tests in `selftest_pmm.c`; `make test-all`
  passes (14 runs). Implementation notes and deviations:
  - **Two bitmaps, not one.** `avail` (fixed after init: proven allocatable) and `alloc`
    (currently handed out), 32 KiB each, static in `.bss`. One bit per frame cannot tell an
    allocated frame from a protected one, so a single bitmap could not reject a free of a kernel
    or module frame. Both bitmaps lie inside the reserved kernel image (checked by test).
  - Memory-map validation lives in the parser (`d8a5327`): `entry_version` must be 0, at least
    one entry, no trailing partial entry; `pmm_init_in` re-checks version and entry size.
  - API: `pmm_init`, `pmm_alloc_frame(&phys)` (zeroed frame, `*phys = 0` on failure),
    `pmm_free_frame` (rejects unaligned, out-of-range, protected and not-allocated frames with
    no state change), `pmm_free_count`, `pmm_kernel` (read-only view), `pmm_check_in`
    (recounts and checks invariants). Allocation is deterministic first-fit with a word hint.
    A broken invariant panics; invalid frees return errors (callers must treat them as bugs).
  - Observed (QEMU `pc`, GRUB 2.14): 128 MiB → 32443 usable frames in production (available
    `[1 MiB, 0x7fe0000)` = 32480 frames minus 37 kernel frames); 2 GiB → 261830 in the test image
    (261888 frames below 1 GiB minus 58), 1073610752 bytes above 1 GiB reported as ignored.
  - Tests: 41 synthetic cases (hand-computed counts plus a per-frame oracle sharing no code with
    `pmm.c`), 43 on the real allocator (whole bitmap equals the oracle on the real map; CR3 page
    tables, stack, GDT/IDT, bitmaps, info block and modules are unavailable and unfreeable; 1000
    unique zeroed allocations and an exact round trip; zeroing proven over a poisoned frame),
    `pmm-exhaust` (drains all 32423 frames, ENOMEM with address 0, full restore). In the
    `modules` image the 1000 allocations straddle the two module frames and skip exactly them.
    Mutation check: removing the protect pass, reservation pass, inward rounding, zeroing,
    double-free, protection, alignment and version checks, hint lowering, or the 2^64 overflow
    rule each fails a test; the 1 MiB clip is backed by a second defensive pass (removing both
    fails).
  - Not verified / limits: frames are reachable only through the 1 GiB boot alias; RAM above
    1 GiB is unused; single CPU, no locking (interrupts are off); poisoning only in test builds;
    no protection against a caller that keeps using a freed frame; free-count accounting is
    O(frames) to verify, allocation is O(words) worst case.

### N1.4 — Page tables, layout, identity-map removal [Implemented and verified]
- Map/unmap/translate with intermediate-table allocation; `invlpg` on unmap; audit function.
- Move to the kernel GDT (N1.1) first, then clear PML4 entry 0 and reload CR3.
- **Tests:** case `kernel-lowaccess` (low address after removal → #PF panic, exit 35); mapper
  self-tests; audit passes.
- **Status (verified 2026-10-17):** `vmm.{c,h}`, tests in `selftest_vmm.c`; `make test-all`
  passes (18 QEMU runs, including the four new #PF images).
  - API: `vmm_map(root, va, pa, flags)`, `vmm_unmap(root, va, &pa)`, `vmm_lookup`
    (effective W/X/U, page size), `vmm_audit_space`, `vmm_init`, `vmm_remove_identity`. A space
    is named by its PML4 frame. Rules are in `vmm.h`: lower-half, canonical, aligned, outside
    `[0, 4 MiB)` and the top lower-half page; frames in `[1 MiB, 1 GiB)`; no W+X; no overwrite;
    no descent into large pages; the upper half is never modified. Intermediate entries are
    P+W(+U in the lower half), so the leaf decides. Unmap returns the data frame and frees only
    emptied, PMM-owned, lower-half tables, after INVLPG. Tables are dereferenced only when
    aligned, below 1 GiB, and inside the kernel image or PMM-allocated.
  - Removal: after `pmm_init`/`vmm_init`, RIP, RSP, GDT, IDT, TSS (+RSP0/IST1) and the boot
    information pointers are checked to be higher-half and PML4[1..255] empty; then PML4[0] is
    cleared and CR3 reloaded. The boot tables (including the page directory shared with the
    alias) stay; the kernel then audits its hierarchy and panics on any error.
  - Evidence: before removal a test build reads `0xe85250d6` at `0x100000`; `kernel-lowaccess`
    (the plan's K3; called "K2" in the N1.4 request) reads it again immediately after removal
    and gets `#PF err=0x0 cr2=0x100000`. `vmm-stale-tlb` (`err=0x0`), `vmm-nx` (`err=0x11`, RIP
    = CR2 = the NX page, after an executable control page returned) and `vmm-readonly`
    (`err=0x3`, CR0.WP) are real CPU faults. 76 self-test cases cover mapping, permissions,
    refusals, unmapping, rollback, large pages, malformed tables, audit detection and kernel
    survival; the PMM free count returns to its baseline.
  - Mutation check (15 mutations of `vmm.c`, all detected): missing INVLPG on unmap (only by
    `vmm-stale-tlb`), missing CR3 reload (only by `kernel-lowaccess`, once it ran before any
    later INVLPG: QEMU flushes its whole TLB on INVLPG inside a tracked large-page region), NX,
    write permission, rollback, ownership check, large-page check, table validation, EEXIST,
    intermediate U, W+X, upper-half refusal, table release, frame range, audit U check.
  - Limits: the 2 MiB kernel alias stays writable **and** executable for all of physical
    0–1 GiB (kernel W^X deferred), so every frame — including future user code and page tables —
    is also writable and executable by the kernel. U/S enforcement, SMEP/SMAP behaviour and
    user-page faults are not exercised (no ring 3 yet). No locking (single CPU, interrupts
    off). Frames above 1 GiB cannot be mapped or used as tables.

### N1.5 — Process object and first ring-3 entry [Implemented and verified]
- Address space = new PML4 with only entry 511 copied from the kernel PML4.
- `enter_user()`: save callee-saved registers and RSP, set TSS.RSP0, load CR3, zero all GPRs,
  `iretq` with SS 0x1B, RSP per section 8.3, RFLAGS 0x2, CS 0x23, RIP = entry.
- Vector 0x80 present with DPL 3; `exit` only. Trap classification by the saved CS RPL: RPL 3 →
  kill process, load kernel CR3, free user memory, return to `enter_user()`'s caller; RPL 0 →
  panic.
- First user code: a few bytes copied from a kernel template into a fresh user page (no ELF yet).
- **Status (verified 2026-10-17):** `process.{c,h}`, `usermode.S`, 256-entry IDT, trap
  classification in `exception.c`; tests in `selftest_user.c` with programs in `user_progs.S`.
  `make test-all` passes (19 QEMU runs); 88 user cases run in boot-128M, boot-2G, modules and
  cpu-max.
  - **Address space:** fresh PML4 with only PML4[511] copied; code page `0x400000` user R+X;
    stack `0x7FFFFFFFB000–0x7FFFFFFFEFFF` user RW NX; guard page, null guard and top page
    unmapped; audited before every entry. Holds root + 5 pages + 6 tables (12 frames).
  - **Entry:** `enter_user` saves rbx, rbp, r12–r15 and RSP, loads the process CR3, nulls
    DS/ES/FS/GS, zeroes all GPRs, IRETQs with SS 0x1B, RSP `0x7FFFFFFFEFF0`, RFLAGS 0x2, CS 0x23.
    A user program verifies this entry state itself.
  - **Return:** traps from ring 3 arrive on TSS.RSP0 (the saved frame ends exactly at RSP0 for
    every exit and fault); the kernel records the frame, reloads its CR3 and `user_return`
    restores the saved context (and SS/DS/ES = 0x10: SS is null after a CPL 3→0 transition). No
    IRETQ ever returns to an ended process; `proc_run` refuses to run a process twice.
  - **Deviation:** an unsupported system-call number kills the process (ABI v0's `-VOS_ENOSYS`
    return needs the resume path planned for N1.6).
  - **Evidence of CPL 3** (CPU-generated): CS 0x23/SS 0x1B in every saved frame; RIP right after
    the `int $0x80` bytes in the user page; `cli`, `hlt`, `mov %cr3`, `out` → #GP(0); `int $14` →
    #GP(0x72), `int $0x81` → #GP(0x40a); user write/read of a present kernel page → #PF 0x7/0x5
    (U bit set by the CPU), CR2 = target, target unchanged; null read #PF 0x4; write to own code
    #PF 0x7; jump to stack #PF 0x15 (NX); push into the guard page #PF 0x6; under `-cpu max`
    `sgdt` → #GP(0) (UMIP). Supplemental only: QEMU `-d int` logs `cpl=3` with `0023:`/`001b:` for
    all 42 user traps of a test boot.
  - **Isolation of faults:** every user fault kills only the process; a normal program runs
    afterwards; kernel-mode traps still panic (`kernel-ud`, `kernel-df`, `kernel-int80`).
  - **Teardown:** only on the kernel CR3; frees pages, emptied tables and the root, never the
    PML4[511] subtree; free count equals the baseline after 50 create/destroy cycles, 20+ runs
    and every fault; callee-saved registers preserved across `proc_run`; RSP0/IST1 canaries
    intact (and their check proven to detect corruption).
  - **Mutation check:** 15 mutations (gate DPL, user IF, IOPL, register clearing, callee-saved
    restore, `cld`, dispatch, CR3 switch-back, frame release, U bit on PML4[511], code/stack
    permissions, fault state, re-run guard) were each detected. With user IF=1, an IRQ from the
    unprogrammed PIC arrives on vector 8 and is caught only by the selector check.
  - **Open (resolved in N1.6):** x87 was usable from ring 3 (CR0.EM = 0); N1.6 sets CR0.EM.
  - **Not tested:** SMEP (kernel execution of a user page), SMAP (disabled), NMI/#MC during user
    mode, real hardware.

### N1.6 — System-call ABI v0 and user-memory access [Implemented and verified]
- `exit`, `debug_write` (section 8.1). `copy_from_user` per section 9.3, written so that SMAP
  bracketing and an exception fixup can be added without changing callers. SMAP stays disabled.
- **Status (verified 2026-10-17):** `syscall.{c,h}`, `usercopy.{c,h}`, resume path in `entry.S`
  and `process.c`, CR0.EM in `cpu.c`; tests in `selftest_user.c` / `user_progs.S`. `make
  test-all` passes (19 QEMU runs plus the negative `forge-check` boot); 122 user cases run in
  boot-128M, boot-2G, modules and cpu-max.
  - **Resume:** a non-terminating `int $0x80` is dispatched, the result stored in the saved rax,
    the frame re-validated (CS 0x23, SS 0x1B, IF 0, IOPL 0, lower-half RIP/RSP, process CR3,
    frame at the top of TSS.RSP0, canaries) and `TRAP_RESUME` returned; only then does the stub
    restore all registers from the frame and IRETQ the CPU-pushed frame, back to the instruction
    after `int $0x80` in the same process. Exit and exceptions still end the process.
  - **Registers:** every register except rax, and RFLAGS (CF and DF tested), are preserved; this
    deviates from section 8.1's reservation of rcx/r11 (per the N1.6 request); a SYSCALL-based
    ABI will need a new ABI version. Test builds poison all live registers after the frame is
    saved, so a register the resume path forgot would be visible (this caught a vacuous test).
  - **`copy_from_user`:** section 9.3 as written, except that the bytes are read through the
    kernel alias of the validated frames instead of through the user address: the kernel never
    dereferences a user pointer, the copy cannot fault, and it needs no `stac`/`clac` when SMAP
    is enabled. All pages are validated before the first byte is copied.
  - **`debug_write`:** length > 256 → `-VOS_EINVAL` (checked first), invalid bytes →
    `-VOS_EFAULT`, length 0 → 0 without examining the pointer. Output: every line prefixed by
    the kernel with `user[<name>]: ` (also after embedded newlines), every byte outside 0x20–0x7E
    except newline printed as `?`, always ending at a line end. `exit-test.sh` matches kernel
    markers only outside `user[` lines; `forge-check` proves a user-printed marker is not found.
  - **x87:** CR0.EM = 1 with MP = TS = 0 (read back). The kernel `.text` of every build contains no
    x87/MMX/SSE instruction (disassembly check). User `fld1` → #NM, `pxor %mm0` and `xorps` →
    #UD; the process is killed, the kernel continues. QEMU TCG does not trap `emms` (an emulator
    deviation from the SDM); it only resets the tag word, unreadable from ring 3 without x87.
  - **Fault injection (all detected):** lost register restores (r11, rcx, rdi, rbx, r15, rax), no
    length limit, no sanitizing, prefix on the first line only, reading the user pointer directly
    (kernel #PF), positive ENOSYS, no CR0.EM, no user-line filter in the harness, and in
    `copy_from_user` no U check, a weakened range check, first-page-only validation.
  - **Not done / not tested:** SMAP (still disabled; section 9.4 prerequisites 2–6 remain for any
    future direct user access), `copy_to_user`, real hardware.

### N1.7 — User runtime and `hello` (build only) [Implemented and verified]
- `user/` tree: `crt0.S` (section 8.3), system-call stubs (the only place `int $0x80` appears),
  ABI note, linker script with explicit page alignment, `hello.c`, build-time ELF checks
  (`readelf`, already installed).
- **Status (verified 2026-10-17; static validation only, nothing is loaded or run):**
  `user/` (`include/vos/syscall.h`, `lib/{crt0.S,syscall.S,abi_note.S,user.ld}`, `hello/hello.c`,
  `tests/` fixtures, `user.mk`), `scripts/check-user-elf.sh`, `scripts/test-user-elf.sh`.
  `make ARCH=x86_64 user` builds `build/x86_64/user/hello.elf`; `user-test` runs as part of the
  x86_64 `arch-test`, so `make test-all` covers it (108 checks).
  - **Runtime:** `vos_exit` and `vos_debug_write` are out-of-line System V functions in
    `syscall.S`, the only `int $0x80` in user space (checked by disassembly); numbers and error
    values come from `vos/syscall.h`, checked equal to the kernel's by the tests. `crt0` records
    the entry RSP (`__vos_entry_rsp`, the runtime's only writable state), clears RBP, keeps RSP
    16-byte aligned, calls `main(void)` and passes its result to `vos_exit` (then `ud2`). Not
    provided: argc/argv/envp, auxiliary vector, TLS, atexit, stdio, heap, FPU.
  - **Build:** `-ffreestanding -nostdinc` (only GCC's own headers), non-PIC, `-mgeneral-regs-only`,
    no stack protector, `-fcf-protection=none`, no unwind tables; `ld -nostdlib -static
    -z noexecstack -z max-page-size=0x1000 -z norelro --build-id=none -T user/lib/user.ld`. Two
    clean builds give the same SHA-256 (`c6c64161…cd06`).
  - **Layout of `hello.elf` (13,128 bytes; the commit message of `99c43f8` says 13,240, which was
    an earlier build):** `PT_LOAD` R-X `0x400000` (0x52 bytes, file offset 0x1000), R `0x401000`
    (0x28), RW `0x402000` (0x8); `PT_NOTE` at `0x401014` (20 bytes, align 4); `PT_GNU_STACK` RW;
    entry `0x400000`. Every executable has exactly these three loads: crt0 guarantees the RW
    segment is non-empty (ld would otherwise emit an empty `PT_LOAD`), the note keeps R non-empty.
  - **Linker refusals:** `user.ld` fails the link for TLS (`.tdata`/`.tbss`) and unless there is
    exactly one 20-byte VOS note. Observed: `ld -pie` together with `-static` and this script
    yields `ET_EXEC`, not a PIE; the PIE negative test uses ld's default script.
  - **Validator additions to section 8.4** (N1.8's loader should enforce the same): `EI_PAD`
    zero; `e_phoff >= 64` and 8-aligned; `PT_NULL` rejected like any other unlisted type; every
    64-bit field below 2^40. Build-time only (section headers, disassembly): no relocation,
    dynamic or TLS sections, no `%fs`/`%gs` operands, no x87/MMX/SSE instructions.
  - **Tests:** hello accepted with the expected profile; no kernel image (production and all test
    images) contains the runtime symbols or hello's code bytes; 58 single-field and 3 file-size
    corruptions each rejected for their own reason; toolchain-built negatives (missing/duplicate
    note refused at link time and rejected when linked without the assertions, W+X, executable
    stack, `--emit-relocs`, a real PIE, x87 code, TLS). Disabling any of 18 validator rules makes
    at least one test fail. Two gaps found by the tests were fixed (TLS was accepted; a missing
    note gave an empty `PT_NOTE`).

### N1.8 — Static ELF64 loader, `hello` runs [Implemented and verified]
- Validation of section 8.4 before any allocation; copy segments; map with section 6.1
  permissions; map stack; enter. Production GRUB configuration adds `module2 /boot/hello.elf hello`.
- **Status (verified 2026-10-17):** `elf.{c,h}` (validator), `loader.{c,h}`, `app.{c,h}`,
  generalized `process.c`; tests in `selftest_elf.c`, `scripts/elfcheck.c`, two isolated images.
  `make test-all` passes from a clean tree (21 QEMU runs, forge check, 182 user/ELF checks).
  - **Production boot (actual output):**
    ```
    app: module "hello" at 0x12c000-0x12f348, 13128 bytes
    elf: hello: ET_EXEC entry 0x400000, 3 PT_LOAD, 3 pages, VOS ABI 0
    user[hello]: Hello from VOS!
    proc: exit status 0 (cs=0x23 rip=0x40002a)
    proc[hello]: exited status 0, all frames reclaimed
    ```
    `0x40002a` is the instruction after `int $0x80` in hello's own `vos_exit`.
  - **Delivery and selection:** the ISO carries `hello.elf` as a separate file (built first);
    `grub.cfg` has `module2 /boot/hello.elf hello`. The application is the module whose command
    line is exactly `hello`: none → nothing runs; several → ambiguous, none runs, boot fails.
  - **Validation:** `elf_validate` implements section 8.4 plus the N1.7 additions, with every
    64-bit field below 2^40 and every read bounds-checked, before anything is allocated; one error
    code per rule group. Built unchanged as a host tool, it agrees with `check-user-elf.sh` on 67
    of 70 test files; the other 3 are rejected only by build-time checks. **Trust boundary:**
    relocation/dynamic/TLS sections, `%fs`/`%gs` operands and x87/SSE code are build-time checks
    only; a file that evades them is still confined (x87/SSE trap, FS/GS base 0).
  - **Loading:** each `PT_LOAD` page is a fresh zeroed frame receiving exactly its file bytes,
    mapped user with W only for `PF_W`, X only for `PF_X`; the module is read through the kernel
    alias, never mapped into the process, and stays reserved. Stack and guard as in N1.5; every
    page is verified against its recorded frame and permissions before entry; any failure
    destroys the partial process.
  - **Trusted report (deviation):** the request asked for the kernel's exit report as
    `user[hello]: exited status 0`, but a program can print exactly that line with
    `debug_write`. The kernel reports `proc[hello]: exited status 0, all frames reclaimed`; the
    harnesses (`exit-test.sh`, and now `test.sh`) match `user[` markers only in user lines and all
    other markers and failure patterns only in kernel lines. The N1.6 built-in program formerly
    named `hello` is now `hello-asm`.
  - **Evidence:** exit frame CS 0x23/SS 0x1B (CPU-pushed), RIP after `int $0x80` in hello's code,
    entry = `e_entry`, one returning system call; pages compared byte by byte with the module;
    69 corrupted executables rejected through `load_elf` for their own reason with no frame
    allocated; exhaustion at every step rolls back; app-ambiguous and app-reject refuse to run.
  - **Mutation check:** 20 rules of `elf.c` disabled one at a time, all detected (17 by the host
    comparison; the bounds, 2^40 and duplicate-note rules are backed by later checks and caught
    by the in-kernel reason checks).
  - **Not tested:** real hardware; more than one application; images larger than the 32 KiB
    buffer used by the mutation tests (the 1 MiB limit itself is tested).

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

### 6.1 Virtual memory layout [Rules enforced by `vmm_map` since N1.4; user regions unused]

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

### 7.2 Algorithm [Implemented in N1.3]
The allocator bitmap covers frames `[0, 1 GiB)`. Initialization is order-independent and
"unusable wins":

1. **Validate the tag.** `entry_size >= 24` and a multiple of 8, `entry_version` 0, at least one
   entry, no partial entry — checked by the parser (boot panics otherwise) and again by
   `pmm_init_in` (`PMM_EMMAP`).
2. **Mark every frame used.**
3. **Free pass.** For each entry with type 1 (available): skip `length == 0`; if `base + length`
   overflows, skip the entry; clip to `[1 MiB, 1 GiB)`; round the start **up** and the end **down**
   to 4 KiB; clear the bits for the remaining whole frames.
4. **Protect pass.** For each entry with any other type (reserved, ACPI reclaimable, ACPI NVS,
   bad memory, unknown): if `base + length` overflows, treat the end as `2^64`; round the start
   **down** and the end **up**; set the bits for every intersecting frame below 1 GiB.
5. **Reservation pass.** Set the bits for every reservation, rounded outward. The implementation
   then clears everything below 1 MiB once more (defence in depth).
6. **Count and report** free frames; in test builds assert that no free frame intersects any
   non-available entry or reservation. (Implemented with inverted polarity: the `avail` bitmap
   starts all-zero and "used" means `avail` clear or `alloc` set.)

Because steps 4 and 5 run after step 3 and only ever set bits, overlapping or contradictory
entries always resolve to "not allocatable", regardless of order. ACPI-reclaimable memory is
treated as unusable in N1.

### 7.2a Inputs provided by N1.2 [Implemented]
`boot_resv` (`resv.h`) is a union of page-aligned ranges that may overlap; step 5 sets the bits
for every entry. Modules, the kernel image and the info block have already been proven to lie in
one available memory-map entry with no non-available entry intersecting them, but this does not
replace steps 3–4: the allocator must still normalize the whole map. The existing guards
treat a non-available entry whose end overflows as extending to 2^64, and ignore available
entries that overflow.

### 7.3 Tests [Implemented in N1.3]
- A `VOS_TEST` self-test runs the same normalization code on synthetic maps into a small test
  bitmap: unsorted entries, available range overlapping a reserved one (both orders), duplicate
  entries, zero length, overflow (`base = 2^64 - 4096`), unaligned bounds, an unknown type, and a
  reservation inside an available range. Expected bitmaps are fixed in the test.
- Boot with 128 MiB and 2 GiB: the free count equals the computed expectation; allocations never
  intersect reservations; RAM above 1 GiB is reported as ignored.

---

## 8. ABI v0 and executable requirements

### 8.1 System calls [Implemented in N1.6]
- Entry: `int $0x80`. Number in `rax`; arguments in `rdi, rsi, rdx, r10, r8, r9`; result in
  `rax` (non-negative = success, negative = VOS error).
- **Clobbers:** `rax` only (implemented in N1.6, as requested then). All other general-purpose
  registers, RFLAGS and the user stack, including the red zone, are preserved. (The original
  plan reserved `rcx`/`r11` for a later SYSCALL entry; such an entry will need a new ABI
  version.)
- No floating point: x87 instructions raise #NM, MMX/SSE #UD (CR0.EM = 1).
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
| FPU/SSE | unavailable: x87 → #NM, MMX/SSE → #UD (CR0.EM = 1 since N1.6) |

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

### 8.4 ELF64 requirements (concern E) [Build-time validator in N1.7; kernel loader in N1.8]
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

### 9.1 Properties N1 is designed to establish [partly verified in N1.5, see N1.5 status]

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

### 9.3 `copy_from_user` [Implemented in N1.6; reads through the kernel alias, see N1.6 status]
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
| U1 | N1.8 | `hello` | `user[hello]: Hello from VOS!` (user line) and the kernel line `proc[hello]: exited status 0` |
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
