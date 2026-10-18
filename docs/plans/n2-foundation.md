# N2 — Operating-System Foundation (Roadmap Proposal)

| | |
|---|---|
| Status | **Approved as the current development plan (2026-10-18), revised after the N2 architecture review.** **N2.0–N2.4 are complete (2026-10-18, sections 3.1–3.5).** N2.5 and every later stage need explicit approval. Sections marked [Proposal] or [Option] are still design, not implementation. |
| Date | 2026-10-17, revised 2026-10-18 |
| Starting point | N1 complete (see `docs/plans/n1-user-space.md`, `docs/abi/syscall-v0.md`) |
| Target | `ARCH=x86_64`; the i386 kernel stays as it is |

Labels: **[Proposal]** a recommendation awaiting approval; **[Option]** an alternative being
compared; **[Open]** a question that needs a decision.

Long-term product goals (desktop, creator tools, AI assistance, packages) are deliberately not
in this document; they are in `docs/vision/product-direction.md`. This document is only about
the kernel and the system underneath them.

---

## 1. Where N1 leaves the kernel

- One synchronous user process at a time; the kernel runs it to completion.
- Interrupts are disabled forever; there is no timer and no interrupt controller set up.
- One kernel stack for kernel work plus one ring-0 entry stack (TSS.RSP0) shared by "the"
  process; no kernel threads, no per-process kernel stacks.
- Physical memory below 1 GiB only; the kernel reaches it through a 2 MiB-page alias that is
  writable and executable (no kernel W^X); no kernel heap (all kernel data is static).
- ABI v0: `exit` and `debug_write` over `int $0x80`; `copy_from_user` is race-free only because
  nothing else can run.
- Programs arrive only as GRUB modules and only `hello` is launched.

Several N1 safety arguments rest on "nothing else can run". N2 removes that assumption, so it
must revisit them explicitly (section 5).

## 2. N2 goal (bounded)

**[Proposal]** N2 ends when VOS can run **several isolated user processes at the same time on
one CPU, switch between them on a timer, let them exchange small messages, and clean each one up
correctly when it exits or faults** — with the same kind of evidence as N1 (CPU-generated, in
QEMU, sequential runs with timeouts).

Concretely, N2's acceptance test would be: GRUB delivers three programs; two exchange messages
over a channel while a third spins in an endless loop; the timer preempts the spinner, both
communicating programs finish, the spinner is terminated by a kernel test policy, and every
frame, page table and kernel object is reclaimed.

**Not in N2:** SMP (more than one CPU), storage, file systems, networking, USB, graphics,
audio, a shell, a user-space loader or app manager, `.vapp` packages, dynamic linking, FPU/SIMD
state, power management, real-hardware support beyond what QEMU emulates.

## 3. Stages (revised after the architecture review)

The review (2026-10-18) checked the first draft against the code and changed it: the timer is
the PIT behind the 8259 PIC instead of the local APIC (`qemu64` has no x2APIC and the xAPIC's
registers at physical `0xFEE00000` lie outside the kernel's only mapping); fixed-size static
object pools replace a general kernel heap; the existing kernel alias becomes the direct map
(no new PML4 entry); the old heap and ABI stages are folded into the others.

Each stage keeps `make test-all` green, adds deterministic tests and stops for review.

| Stage | Content | Acceptance (examples) |
|---|---|---|
| **N2.0** Kernel memory hardening | Kernel W^X: kernel image mapped with 4 KiB pages (text R-X, read-only data R, data RW NX), the rest of the 1 GiB alias RW NX in 2 MiB pages, no writable alias of code; guard pages under the boot, kernel-entry and IST stacks; IST stacks for NMI and #MC | Kernel writes to text and read-only data fault; executing data or a free frame through the alias faults; stack overflow ends in #DF on its IST stack; all N1 tests pass |
| **N2.1** Interrupts and timer | 8259 PIC remapped to vectors 0x20–0x2F and masked except IRQ0; PIT at about 100 Hz; IRQ dispatch, EOI, spurious IRQ7/15; kernel always IF=0, user code IF=1 (entry RFLAGS `0x202`, an ABI amendment); a test-only tick budget that kills a runaway program | Ticks counted while a program runs; IRQ frames from ring 3 return to the same instruction with all registers intact; a runaway program is killed |
| **N2.2** Kernel threads and scheduler core | Thread records and guarded 16 KiB kernel stacks from static pools; context switch; idle thread (`sti; hlt`); round robin; kernel non-preemptible; tested with kernel threads only | Kernel threads interleave; registers and canaries intact after many switches |
| **N2.3** Multiple user processes | Static process table, 64-bit IDs never reused, one thread per process, CR3 and TSS.RSP0 switched per thread, preemption only when an IRQ interrupts ring 3, exit/kill and reaping after the switch away, several boot modules started from trusted configuration; `proc_run` kept as a wrapper for N1 tests | Several programs run concurrently; a faulting or runaway one is removed while others continue; everything returns to baseline |
| **N2.4** Channels, handles and ABI v1 | Static pools of channels; per-process handle tables with rights; messages up to 256 bytes copied in and out (`copy_to_user`); non-blocking send (`-VOS_EAGAIN`), blocking receive, `-VOS_EPIPE` after the peer closes; ABI v1 = v0 plus `yield`, `channel_send`, `channel_recv`, `handle_close` and new error codes; loader accepts note versions 0 and 1 | Ping-pong; peer death wakes the receiver; limits return errors, never panics; `hello` (v0) unchanged |
| **N2.5** Concurrency and isolation suite v2, documentation | The N1.9 suite extended to concurrent processes, IPC faults, killing blocked processes and exhaustion | Full suite under qemu64, 2 GiB and `-cpu max` |

Order: N2.0 → N2.1 → N2.2 → N2.3 → N2.4 → N2.5. Guard pages and IST stacks come before any
asynchronous entry (N2.1).

### 3.1 N2.0 result: kernel W^X and protected kernel stacks (complete)

**Memory map of the kernel alias** (production image; addresses from `nm` and the boot log;
"phys" is the frame, the virtual address is `0xFFFFFFFF80000000` + phys):

| Phys range | Before N2.0 (boot tables) | After N2.0 (`kmap.c`) |
|---|---|---|
| `0x0`–`0xFFFFF` (low memory, VGA) | part of one 2 MiB page, RW **X** | 256 x 4 KiB, RW NX |
| `0x100000` (Multiboot2 header, 32-bit boot code) | " | 4 KiB, R NX |
| `0x101000`–`0x106FFF` (boot data, boot PML4/PDPTs/PD) | " | 4 KiB, RW NX |
| `0x107000`–`0x10EFFF` (`.text`, 8 pages) | " | 4 KiB, **R X** (the only ring-0 executable pages) |
| `0x10F000`–`0x111FFF` (`.rodata`) | " | 4 KiB, R NX |
| `0x112000`–`0x13AFFF` (`.data`, `.bss`) | " | 4 KiB, RW NX, except the guards below |
| `0x113000`, `0x119000`, `0x11B000`, `0x11D000`, `0x11F000` | " | **not present**: guards below the boot stack, IST1, IST2, IST3 and the RSP0 entry stack |
| `0x13B000`–`0x1FFFFF` (free frames, info block, modules) | " | 4 KiB, RW NX |
| `0x200000`–`0x3FFFFFFF` | 511 x 2 MiB, RW **X** | 511 x 2 MiB, RW NX |

PML4[511] and PDPT[510] are unchanged; `kmap_init` points PDPT[510] at a new page directory
whose entry 0 is one page table for phys 0–2 MiB. User address spaces copy PML4[511] by value
and therefore share the new map. `phys_to_virt()` remains valid for every frame below 1 GiB
except the five guard frames. The boot page directory is zeroed after the switch.

**Stacks.** Boot kernel stack 16 KiB, RSP0 entry stack 16 KiB, IST1 (#DF), IST2 (NMI), IST3
(#MC) 4 KiB each; each sits directly above its own unmapped 4 KiB guard page, and keeps its
canaries.

**Transition.** `kmap_init` runs once, after the identity map is removed: it builds the tables
from linker symbols and the guard list, checks them (layout, then every address the running
kernel needs: text, the running code and its caller, every IDT handler, all stacks, GDT, IDT,
TSS, the active PML4 and PDPT, the new tables, allocator state and bitmaps, `.data`/`.bss`,
boot data, VGA, boot information, modules and one probe per 2 MiB page), primes the TLB by
touching a text page and a guard page through the old map, switches PDPT[510], flushes all
non-global translations (CR4.PGE toggled if set, CR3 reloaded), requires the CPU to refuse a
text write (#PF error 0x3) and a guard read (error 0x0) through two armed, fault-tolerant probe
instructions, and repeats the checks on the live tables through `vmm_lookup`. If anything after
the switch fails, it restores the old PDPT entry and reports; the kernel then panics.

**Rollback limits.** Rollback only helps when the new map still lets the kernel run its check
code. If the switch itself removed the running code, its stack or the exception path, the CPU
faults (or triple-faults and resets) before any check can run. The pre-switch check exists for
that reason; it cannot cover errors in the checker itself.

**Rules enforced from now on.** `vmm_map` refuses executable mappings without `VMM_USER` and
frames not allocated from the PMM (so never kernel-image, module or info-block frames). The
audit rejects, in every address space: effective W+X leaves; ring-0-executable leaves other
than kernel text (user pages excepted, see limits); writable mappings of kernel text; any leaf
covering a guard page; lower-half leaves on kernel-image frames. For the kernel half it compares
every entry with the intended layout.

**Evidence (QEMU TCG, `make test`):** `selftest: kmap 49 cases, 0 failed` in every test image
(live layout page by page, 1 GiB alias translation, verifier refusing ten defective table copies
and ten running-address defects, audit refusing nine alias-bypass hierarchies, IST and guard
layout); nine isolated CPU-fault cases whose `exc:` line is compared with the expected vector,
error code and CR2/RIP by `scripts/check-fault.sh`: kernel write to text and to `.rodata`
(#PF 0x3), execution of `.data` and of a free frame through a 2 MiB alias page (#PF 0x11, RIP =
CR2), a guard-page read (#PF 0x0), kernel stack overflow (#DF on IST1, CR2 in the boot-stack
guard), `int $2` and `int $18` (IST2 and IST3 — gate and stack tests only, not hardware events),
and a **genuine NMI** injected with QEMU's monitor command `nmi` while the kernel waits in
`cli; hlt` (`scripts/nmi-test.sh`; delivered on IST2, RIP right after `hlt`). Twenty source
mutations (text writable, data executable, guard mapped, guard list lost, required address
dropped, IST wrong, alias rules removed, TLB flush removed, over-broad probe recovery, ...) were
each caught by at least one test; the TLB-flush mutation was caught only after the probe check
was added.

**Regression at the end of N2.0:** `make test`, `make ARCH=x86_64 test` and `make test-all`
pass (36 QEMU runs in `test-all`, all nine fault comparisons, 249 user/ELF checks, the 41-case
suite under qemu64, 2 GiB and `-cpu max` with equal free frames before and after);
`make ARCH=i386 test` passes and the i386 `vos.bin` SHA-256 is still
`0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd`; `hello.elf` is unchanged
(`c6c64161…cd06`). User entry RFLAGS stay `0x2` (IF=0); there is still no timer.

**Limits of N2.0 (known, not fixed):**
- Machine checks: CR4.MCE is not enabled, so a real machine check shuts the machine down
  instead of raising #MC; no machine check was injected (only the `int $18` gate test).
- The NMI test uses QEMU's injection under TCG; real hardware NMIs (watchdogs, IPIs) are not
  tested. An NMI that arrives while the NMI handler runs is not handled specially (the handler
  panics; there is no nested-NMI logic).
- With SMEP off (`qemu64`), ring 0 can still execute user code pages; the kernel can write the
  frames of user code through its alias (that is how the loader fills them). With SMEP on
  (`-cpu max`) the CPU refuses ring-0 execution of user pages.
- Guard pages are 4 KiB: an access that skips more than 4 KiB below a stack (a large local
  array) would reach the memory below the guard. There is no stack probing.
- RSP0-stack overflow from a system call and IST-stack overflow are not exercised by CPU tests;
  their guards are verified unmapped through the page tables and the audit.
- The 4 KiB table covers only phys 0–2 MiB; a linker assertion stops the build if the kernel
  image grows beyond it.
- Everything ran under QEMU TCG only.

### 3.2 N2.1 result: interrupt controller and timer (complete)

**Architecture.** `pic.c` drives the 8259 pair through an injectable port-I/O interface;
`irq.c` reaches it only through `struct irq_chip` (init, mask, unmask, masks, in-service,
begin, EOI), so a later controller can replace it; `pit.c` programs channel 0. `irq_init`
runs once after the kernel W^X map: both chips are reinitialized with every line masked
(ICW1 `0x11`, ICW2 `0x20`/`0x28`, ICW3 `0x04`/`0x02`, ICW4 `0x01`, OCW1 `0xFF`), the PIT
is set to mode 2 with divisor 11932 (99.998 Hz), and only then IRQ0 is unmasked. The cascade
line IRQ2 stays masked because no slave line is used; slave EOI and spurious-IRQ15 handling
exist and are unit-tested.

**Vector allocation.** `0x00`–`0x1F` exceptions (#DF IST1, NMI IST2, #MC IST3); `0x20`–`0x27`
IRQ0–7; `0x28`–`0x2F` IRQ8–15; `0x80` system calls (DPL 3); all others absent. IRQ gates
are DPL 0 interrupt gates without IST and exist before the controller is touched.

**Hazard found at the start of N2.1.** The kernel had never touched the PIC: the firmware
state (master base `0x08`, IRQ0 unmasked, BIOS PIT running) was harmless only because IF was
0 everywhere. Running ring 3 with IF = 1 before remapping would have delivered IRQ0 on the #DF
gate. The order is therefore IDT gates → controller remapped and masked → PIT → IRQ0 unmasked
→ first user program.

**IF policy.** The kernel always runs with IF = 0 and never executes `sti`; all gates are
interrupt gates; the trap handler panics if entered with IF = 1; an IRQ vector from ring 0
panics. Ring 3 runs with IF = 1, IOPL = 0 (entry RFLAGS `0x202`, ABI amendment in
`docs/abi/syscall-v0.md`). An IRQ is taken only from ring 3, on TSS.RSP0; `irq_handle` checks
the vector against the in-service registers (spurious IRQ7: no EOI; spurious IRQ15: master
EOI only; any other line not in service: panic), counts it, sends EOI, checks that the line
left service, and the program resumes after the same frame validation as a system call (now
IF = 1, IOPL = 0, VM = 0, frame at the top of RSP0). There are no exceptions to this policy,
not even for tests. Test builds add a kernel-side tick budget (default 1000, per test) that
kills a ring-3 program after N ticks, plus a second bound on interrupts in case ticks stop.

**Evidence (QEMU TCG).** Synthetic: 14 driver cases against a fake 8259 (exact init sequence,
masks, EOI routing, spurious IRQ7/15, not-in-service refusal). Hardware read-back: IMR
`0xFFFE`, nothing in service, PIT status `0x34` and counting, IDT layout. CPU-delivered
interrupts: an endless loop interrupted from CPL 3 (CS `0x23`, IF 1, IOPL 0, RIP inside the
loop, user RSP) and killed after exactly 5/50 ticks; 15 registers and DF intact across 20 and
50 interrupts; `popfq` cannot clear IF or raise IOPL; `sti` raises #GP(0); the kernel sees IF =
0 inside a system call; twelve crafted frames show each resume check; `int $0x20` in ring 0
panics; `hello`, system calls and user faults work afterwards with frames back at baseline.
Separately built programs (`user/suite/spin.c`, `irqregs.S`, `iflock.c`, `kernelif.c`,
`sti.c`) run through GRUB/ELF/`proc_run` in the `irq` image under qemu64, 2 GiB and
`-cpu max`. Observed under TCG: 50 ticks took about 0.49 s of host time; nothing depends on it.
Nineteen source mutations (wrong remap, missing EOI, IRQ0 masked, extra line unmasked, wrong
vector dispatch, trap gate, clobbered saved register, swapped register restore, user IF = 0,
IOPL 3, stuck tick counter, resume accepting IF = 0 / IOPL / any stack, three spurious/EOI
routing errors, `sti` in the kernel, PIT one-shot) were each caught with a specific diagnostic.
The missing EOI was at first only a hang; the post-EOI in-service check was added for it. IRQ0
masked and a one-shot PIT still end in a timeout, but only after a specific self-test failure.

**Regression at the end of N2.1:** `make test` and `make ARCH=x86_64 test` (39 QEMU runs each)
and `make test-all` (40) pass; 258 user/ELF checks; the 41-case isolation suite and the 9-case
`irq` image under qemu64, 2 GiB and `-cpu max` with free frames equal before and after; all
nine N2.0 fault comparisons and the genuine NMI case; `selftest: irq 22`, `user 151`,
`kmap 49` cases with 0 failures. `make ARCH=i386 test` passes and the i386 `vos.bin` SHA-256 is
still `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd`; `hello.elf` is
unchanged. No scheduler exists.

**Also fixed in N2.1:** the N2.0 audit checked text overlap page by page (about 261,000 calls
per kernel-half audit); a constant-time check brought the isolation suite from about 21 s back
to about 2 s. Pending IRQ0 while the kernel runs with IF = 0 also slows QEMU TCG measurably
(about 50 % on kernel-heavy code in one measurement); real hardware has no such cost.

**Constraints for N2.2.** The kernel is non-preemptible and IF = 0 everywhere in it; an idle
thread's `sti; hlt` would be the first ring-0 IF = 1 window and needs ring-0 IRQ handling
(today a panic), a decision on nesting and an EOI-before-switch rule. There is one RSP0 stack;
per-thread stacks must keep the "frame at the top of RSP0" check true. Ticks are a counter
only: no timekeeping, no timers, no sleeping. The tick budget is test-only and must not become
the production policy. Spurious IRQ7/15 handling is tested only synthetically (QEMU produced
none). APIC, IOAPIC, ACPI and SMP stay out.

### 3.3 N2.2 result: kernel threads and the first scheduler (complete)

**What exists.** `thread.c`/`thread.h`, `thread_switch.S`. A fixed pool of 8 thread records
(64-bit IDs from 1, never reused) and 7 static 16 KiB kernel stacks in `.bss`, each directly
above its own 4 KiB guard page; the guards are part of the kernel map's guard list, so the W^X
map leaves them unmapped (12 guard pages in all). Record 0 is the boot thread (`kernel_main`, on
the boot stack); one record is the idle thread; 6 are left for kernel threads. No heap: a full
pool returns `THREAD_ENOMEM`. States: free, runnable, running, blocked, dead.

**Context switch.** `context_switch` is a function call that saves `rbx, rbp, r12–r15` and RSP
and loads another thread's; caller-saved registers are the compiler's business, flags are not
switched (IF is always 0 at a switch), CR3 never changes (all threads share the kernel address
space), and the kernel has no FPU/SIMD state. A new thread starts from an initial frame whose
return address is `thread_start`, with RSP 16-byte aligned for its first call. Every switch
checks: IF = 0, the kernel CR3, no user program running, the target runnable, its saved RSP
inside its own stack (and `% 16 == 8`), the stack owned by it, canaries intact, TSS.RSP0 not
inside a thread stack.

**Scheduler.** Cooperative round robin over a FIFO run queue: `thread_yield`, `thread_block` /
`thread_wake_one` / `thread_wake_all` on FIFO wait queues (the mechanism later IPC will use),
`thread_sleep(ticks)`, `thread_exit`. A dead thread is reaped by the next thread to run (after
the switch away): canary check, stack poisoned in test builds, record and slot freed. Sleepers
are woken by the scheduler in thread context, never by the interrupt handler. If every thread is
blocked and none sleeps, the idle thread panics ("lost wakeup?") instead of halting forever.

**Interrupt-state policy.** All scheduler entry points run with IF = 0, in thread context (never
inside `irq_handle`), with no user program running. The kernel is non-preemptive: a thread runs
until it yields, blocks, sleeps or exits; the timer never switches threads. The idle thread runs
only when nothing else can; it waits with `sti; hlt` followed by `cli`. That `hlt` (and the
`cli` after it) is the only kernel code that ever runs with IF = 1. A hardware interrupt in ring
0 is accepted only if its frame is exactly that state (kernel CS/SS, RIP right after `hlt`,
IF 1, IOPL 0, VM 0, saved RSP and frame on the idle stack, the idle thread current); it is
acknowledged and returns to the idle loop, which disables interrupts at once and checks that
nothing is left in service. The IRETQ back restores IF = 1, so an interrupt that became pending
meanwhile can be taken just before the `cli`, with the same return address (observed once under
TCG: two ticks in one halt). Every other ring-0 IRQ still panics. Interrupt frames from ring 0
have the same layout as from ring 3 (64-bit mode always pushes SS:RSP) but stay on the current
stack; there is no IST for IRQs.

**User programs (unchanged boundary).** `proc_run` stays synchronous and is allowed only on the
boot thread (a worker thread trying it panics); TSS.RSP0 stays the single dedicated entry stack;
an interrupt from ring 3 never switches threads. ABI v0, entry RFLAGS `0x202`, system calls and
fault isolation are unchanged. Per-thread RSP0, user processes on their own threads and CR3
switching are N2.3 work.

**Evidence (QEMU TCG).** `selftest: sched 37 cases, 0 failed` in every test image, all kernel-side
verdicts: guard pages unmapped and listed, stacks RW NX and disjoint; run-queue order and
`A B C A B C A B C` alternation; callee-saved registers of three threads intact over 20+
switches each; stack isolation; deferred reaping seen by the next thread (record free, stack
poisoned); pool exhaustion and slot reuse with increasing IDs; the idle thread taking every timer
IRQ of a 5-tick sleep in ring 0 with the frame checked; blocked threads never on the run queue or
scheduled; FIFO wakeups; producer/consumer without lost or extra wakeups; sleep duration; the last
runnable thread exiting while the boot thread sleeps; ten crafted frames for the ring-0 IRQ
check. Typical run: 25 threads created, 156 switches, 24 reaped, idle halted 12 times and took 12
timer IRQs. Isolated cases: a kernel thread's stack overflow ends in #DF on IST1 with CR2 in that
thread's guard page (`check-fault.sh`), a deadlock ends in the lost-wakeup panic, a user program
on a worker thread is refused. `hello`, the 41-case suite and the 9-case interrupt suite run
after the scheduler tests.

**Mutations.** All 19 were caught with a specific diagnostic: wrong saved RSP, r13 not
restored, block without the blocked state, reuse of a live stack (also with the ownership check
removed: CPU #UD), freeing the running thread's stack (also without the in-use check: RIP
`0xdddd…` from the poisoned stack), wrong TSS.RSP0, run-queue tail corruption, sleep wakeup of
blocked threads, dead threads never reaped, a lost wakeup, idle without `cli`, an idle IRQ not
acknowledged, a thread guard page mapped, user programs on worker threads, and the ring-0 IRQ
check without its thread, RIP or IF condition. Two needed new checks first: a missing
acknowledgement on the idle path only hung until the idle loop checked the in-service registers,
and the RIP condition went unnoticed until the ring-0 IRQ check got its own crafted-frame tests.

**Regression at the end of N2.2:** `make test` and `make ARCH=x86_64 test` (42 QEMU runs each)
and `make test-all` (43) pass; ten `check-fault.sh` comparisons (nine N2.0 cases and the thread
stack overflow) and the genuine NMI case (IST2); 264 user/ELF checks; the 41-case isolation suite
and the 9-case interrupt suite under qemu64, 2 GiB and `-cpu max` with free frames equal before
and after; `selftest: kmap 49`, `irq 22`, `sched 37`, `user 151` cases with 0 failures. Two
clean builds of the x86_64 kernel are byte-identical. `make ARCH=i386 test` passes and the i386
`vos.bin` SHA-256 is still `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd`;
`hello.elf` is unchanged (`c6c64161…cd06`).

**Limits.** Cooperative only: a kernel thread that never yields keeps the CPU. One CPU, no
locks (none are needed while the kernel is non-preemptive with IF = 0). Wakeups come only from
threads and timed sleeps; there are no IRQ-driven wakeups yet. Stacks are static (7, 16 KiB, 4 KiB
guards, no stack probing). No priorities, no timeouts on wait queues, no thread join beyond
polling the state.

**Constraints for N2.3.** Running user programs on their own threads needs a kernel stack and a
TSS.RSP0 per thread (updated at every switch, never pointing at a freed stack), CR3 switching on
the switch path, an ownership rule for the RSP0 frame check, preemption only when an interrupt
comes from ring 3 (switching on the IRQ return path after EOI), and teardown of a process whose
thread is not running.

### 3.4 N2.3 result: multiple user processes and timer preemption (complete)

**Capacity.** 16 thread records and 15 guarded 16 KiB kernel stacks (was 8/7); with the boot and
idle threads that leaves 14 threads for processes and kernel threads. A process table of 8
(`PROC_TABLE`; `PROC_EBUSY` beyond). The static stacks put the test image's end at `0x1e5000`
(production `0x18b000`); the linker assertion keeps it inside the 2 MiB covered by the kernel
map's 4 KiB table, and 20 guard pages are unmapped. Process and thread IDs are never reused.

**Ownership.** Every process has exactly one thread of its own (`thread_create_user`), created by
`proc_start` and recorded both ways (process `tid`/`kstack_top`, thread `proc`/`root`). Process
records belong to their kernel caller (static memory, no heap); the frames and page tables of an
address space belong to the process; the thread record and stack belong to the scheduler.
Processes are created only by kernel code from trusted GRUB modules (ELF loader) or test
programs; there is no spawn system call and no file-system loading.

**CR3 and TSS.RSP0.** At every switch the scheduler sets TSS.RSP0 to the destination thread's
kernel-stack top if it runs a process (the dedicated entry stack while a kernel thread runs) and
loads the destination's CR3 if it differs (a full flush of non-global translations; the kernel
half, PML4[511], is shared by value and supervisor-only). A switch is refused unless the target
address space shares the kernel half and matches the thread kind (kernel threads on the kernel
root, user threads never), the running thread is on its own address space, the stack belongs to
the target, the canaries hold, and nothing is in service at the interrupt controller; the arriving
thread checks its CR3 and RSP0 again. Each trap from ring 3 must sit exactly at the top of the
current process's kernel stack, which must equal TSS.RSP0, in that process's address space. The
fixed N1 "entry stack" check became this per-thread check.

**Execution and preemption.** A process's thread enters ring 3 once (`user_enter`: RFLAGS
`0x202`, registers zero; ABI v0 unchanged). Every later kernel entry is an interrupt on top of its
own kernel stack. Preemption happens only there: a timer interrupt from ring 3, after
`irq_handle` acknowledged it, if the thread's slice (2 ticks) is over and another thread is
runnable; the thread yields inside its trap path with the complete user state in its trap frame,
and when it runs again the frame is validated before IRETQ. Kernel code (kernel threads, system
calls, the idle IRQ path) is never preempted. Per-process ticks count only time the process ran.

**Exit and cleanup.** Exit, a user fault, the test tick budget and `proc_kill` (taking effect at
the next kernel entry, at the latest the next timer tick, or before the first entry) end the
process from its own thread: final state kept, the thread leaves the process's address space for
the kernel root, waiters are woken, the thread exits; the next thread to run reaps its stack.
`proc_wait` returns only when the thread is gone; `proc_destroy` then frees every frame and page
table (refused while the thread exists or the address space is active). `proc_run` remains as
`proc_start` + `proc_wait`. GRUB module frames stay reserved throughout.

**Changed N1/N2.2 tests (behaviour kept).** "Only one process at a time" became: a second process
has its own root, frames and ID; the table is bounded; nothing leaks. "The trap frame is at the
top of the entry stack" became "at the top of the process's own kernel stack". The N2.2
boot-thread-only rule and its `proc-on-worker` panic case are obsolete; a kernel worker thread
now runs and destroys a process as a positive test.

**Evidence (QEMU TCG).** In-kernel tests (`selftest: user`): an endless loop beside a normal
program, which finishes; three endless loops sharing 36 ticks 12/12/12; 15 registers and DF
intact while other processes preempt; two processes writing the same user address each seeing
only their own value; every trap on its own kernel stack; one faulting, one exiting, one running
on; a kill before first entry; 15 rounds without leaks; a full thread pool making `proc_start`
fail with `PROC_ENOMEM`. The `mp` image (`selftest: mp 12 cases`) runs separately built ELF
programs from GRUB at once: `memtag1`/`memtag2` (no shared frame, different roots, never the
other's data), with `irqregs` beside them; three endless loops 22/22/22 ticks; `hello` and the
system-call program beside an endless loop; a page fault ending only its process; 8 programs at
once and the 9th refused; 10 rounds of `hello` beside an endless loop; `hello` alone afterwards
— frames, records and threads back to baseline each time, under qemu64, 2 GiB and `-cpu max`.

**Mutations.** Caught with specific evidence: wrong destination CR3 and wrong TSS.RSP0 (also with
the arrival checks removed: the thread-entry check), no CR3 switch between processes (also with
every CR3 check removed: the multi-process tests fail), registers clobbered at preemption, a
killed process left running, `proc_wait` not waiting, freeing a running process (with both
teardown checks removed: the scheduler refuses the freed root), a preempted thread lost from the
run queue (lost-wakeup panic), frames not reclaimed, ticks counted for the wrong process (after
adding an upper bound to the fairness tests), acknowledging the timer IRQ only after preemption
(after adding the "nothing in service at a switch" check; a first variant was an equivalent mutant
that could never fire). Spot checks: missing EOI, missing TLB flush, freeing a running thread's
stack.

**Regression at the end of N2.3:** `make test` and `make ARCH=x86_64 test` (44 QEMU runs each)
and `make test-all` (45) pass; ten `check-fault.sh` comparisons and the genuine NMI case; 266
user/ELF checks; the 41-case isolation suite, the 9-case interrupt suite and the 12-case `mp`
image under qemu64, 2 GiB and `-cpu max` with free frames equal before and after; `selftest:
kmap 49`, `irq 22`, `sched 37`, `user 164`, `elf 31` with 0 failures. Two clean builds of the
x86_64 kernel are byte-identical. `make ARCH=i386 test` passes and the i386 `vos.bin` SHA-256 is
still `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd`; `hello.elf` is
unchanged (`c6c64161…cd06`).

**Limits.** One CPU; one thread per process; no IPC, no spawn/wait for programs, no blocking
system calls; static 16 KiB kernel stacks; a 2-tick round-robin slice without priorities; the
image is within about 108 KiB of the 2 MiB limit of the kernel map's 4 KiB table (more
stacks or test records will need stacks mapped from allocated frames or a second table); the
kernel half is mapped in every address space (supervisor-only, no side-channel mitigation).

**Constraints for N2.4.** Blocking system calls (channel receive) will block a process's thread
inside its trap path — the same mechanism as preemption, so its frame stays at the top of its
kernel stack; wakeups come from other threads (`thread_wake_*`) in thread context. Killing a
blocked process needs `proc_kill` to wake it from its wait queue. Handles and channels need
per-process tables in the process record and teardown in `proc_destroy`.

### 3.5 N2.4 result: channels and native ABI v1 (complete)

**What exists.** `channel.{h,c}`, `copy_to_user` in `usercopy.c`, ABI v1 in `syscall.c`; the
contract is `docs/abi/syscall-v1.md`. A static pool of 16 channel records with two endpoints
each; every endpoint's receive queue (16 messages of up to 256 bytes) is one 4 KiB frame from
the physical allocator, owned by the channel and freed with it (no heap; the image only grows
by the records). 8 handle slots per process, in the process record. Kernel image: production
ends at `0x18d000`, the largest test image at `0x1ed000` (about 76 KiB below the 2 MiB
covered by the kernel map's 4 KiB table); no mapping change was needed.

**Ownership and handles.** An endpoint belongs to the process whose handle names it; a handle
value is `(generation << 8) | (slot + 1)` and is looked up only in the caller's own table, so
foreign, unissued and closed values are `-VOS_EBADHANDLE`; a 16-bit generation per slot makes
a recycled slot's new handle differ from the old one (generation counters were judged cheap
enough to do now; they wrap after 65 536 reuses of one slot, which is documented as a limit).
Rights (send/receive) are set by the trusted creator and checked on every operation
(`-VOS_EACCES`). Channels are created only by kernel code (`channel_create`); programs get
their handles numbered from 1 in installation order. No raw pointer crosses address spaces
and nothing is shared: messages are copied in and out through validated kernel aliases.

**ABI v1.** `yield` (2), `channel_send` (3), `channel_recv` (4), `handle_close` (5); new errors
`EAGAIN` 4, `EPIPE` 5, `EBADHANDLE` 6, `ENOMEM` 7, and two beyond the proposal: `EMSGSIZE` 8
(the receive buffer is smaller than the message; it stays queued, never truncated) and
`EACCES` 9 (a missing right, distinct from an invalid handle). The loader accepts ABI note
versions 0 and 1 and records the version; an ABI v0 process gets `-VOS_ENOSYS` for 2–5.
ABI v1 programs link `abi_note_v1.o` and `syscall_v1.o`; `hello.elf` is unchanged.

**copy_to_user.** Same rules as `copy_from_user` plus effective W at every level; the whole
range is validated before the first byte is written, through the kernel alias, never the user
address (no fault, no SMAP dependence); length 0 does not examine the pointer. The mappings
cannot change during a copy: one CPU, the kernel runs with IF = 0 and is not preempted inside
it, and a process's page tables change only while it is built or after its thread is gone.

**Blocking and wakeups.** `channel_recv` on an empty queue with an open peer blocks the
process's thread inside its system-call path (the same mechanism as preemption: the complete
user state stays in the trap frame at the top of its kernel stack). A send or the peer's close
wakes it in thread context; it re-checks and the call returns normally. Because the kernel is
non-preemptive with IF = 0, "empty, so block" cannot lose a wakeup. No blocking ever happens in
an IRQ handler. Ticks spent blocked or yielding are not counted as the process's own.

**Process death.** Every termination closes the process's handles first (peers see
`-VOS_EPIPE` at once; messages queued to it are discarded; messages it had sent stay in the
peer's queue and are still delivered). `proc_kill` wakes a thread blocked on a wait queue
(`thread_interrupt` removes it from the queue); `channel_recv` notices the kill and the trap
path ends the process before anything returns to user mode. Closing an endpoint its owner is
blocked on, and reaping a thread still linked on a queue, are kernel errors (panics).
`proc_destroy` closes what a never-started process holds.

**Bug found and fixed (`64a5fef`).** Tick sleepers were only woken when something entered the
scheduler; a lone CPU-bound process is never preempted, so a kernel thread sleeping beside it
waited until the process ended (an IPC test slept 3 ticks and woke 400 later). The ring-3 timer
path now wakes due sleepers before the preemption check; a test pins it.

**Evidence (QEMU TCG).** `selftest: ipc 25 cases` (kernel interface on processes that do not run:
lengths 0/256/257, `EMSGSIZE` and `EFAULT` without loss, FIFO over a full queue with equal
in/out hashes, `EAGAIN`, invalid/foreign/stale handles, rights, drain-then-`EPIPE`, double close,
slot reuse, teardown by `proc_destroy`, handle-table and pool exhaustion, frames back).
`selftest: user` adds `copy_to_user` (every invalid destination refused with not one byte
written) and running processes: an ABI v0 program gets four `ENOSYS`; a receiver blocked before
the sender runs wakes with all registers intact; a receiver killed while blocked; a message from
a sender that already exited; the sleeper fix. The `ipc` image runs 11 trusted scenarios of
separately built programs (`user/suite/ipc_*`), judged by the kernel's records (state, exit
status or fault, kill reason, messages in/out with order-sensitive hashes): ping/echo with the
receiver blocked first (×3), registers across send/yield/blocking receive, flood to `EAGAIN`
and drain to `EPIPE`, ping/echo started together, a blocked receiver killed, a peer that already
faulted, rights and double close, yield, ABI v0 compatibility, hello, and a program printing a
forged verdict that the harness must not accept. Under qemu64, 2 GiB and `-cpu max`.

**Mutations.** All 18 caught with specific evidence: no generation check; wrong endpoint owner
(also with the integrity check removed); no send-right check (also caught by the ELF scenario);
`copy_to_user` without the W or U check, or writing before validating the whole range; queue
overflow; LIFO instead of FIFO; no wakeup on send, a yield between "empty" and "block", close not
waking the receiver (these three only in the `ipc` image: in the in-kernel test the sender exits
at once, and its close wakes the receiver anyway); channel never released; message frames
leaked; a killed thread left on its wait queue; a kill not waking a blocked thread; ABI v0
allowed v1 calls; the harness accepting a verdict printed by a program. Spot checks of N2.3
mutations (acknowledging after preemption, wrong CR3) still catch.

**Regression at the end of N2.4:** `make test` and `make ARCH=x86_64 test` (47 QEMU runs each)
and `make test-all` (48) pass; ten `check-fault.sh` comparisons and the genuine NMI case; 278
user/ELF checks; the 41-case isolation suite, the 9-case interrupt suite, the 12-case `mp` image
and the 11-scenario `ipc` image under qemu64, 2 GiB and `-cpu max`, frames equal before and
after; `selftest: kmap 49`, `irq 22`, `sched 37`, `ipc 25`, `user 179`, `elf 34` with 0
failures; the forged-verdict check holds. Two clean builds of the x86_64 kernel are
byte-identical. `make ARCH=i386 test` passes and the i386 `vos.bin` SHA-256 is still
`0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd`; `hello.elf` is unchanged
(`c6c64161…cd06`).

**Limits.** Channels only between processes wired by the kernel; no handle transfer, no
multi-handle wait, no timeouts or non-blocking receive; 16 channels, 8 handles per process,
16 × 256-byte queues; generation wrap after 65 536 reuses of a slot; one CPU, one thread per
process; about 76 KiB left under the 2 MiB image limit.

**Constraints for N2.5.** A concurrency and isolation suite v2 can build on the `ipc` scenario
runner (trusted configuration, kernel-side verdicts). Exhaustion and kill tests at scale are
bounded by 8 processes, 14 free threads and 16 channels; more needs the image-limit decision
(stacks from allocated frames, or a second 4 KiB table).

**Deferred beyond N2:** a general kernel heap, `SYSCALL`/`SYSRET`, a `spawn` system call and a
user-visible `wait`, LAPIC/IOAPIC/ACPI, SMP, more than one thread per process, FPU/SIMD state,
RAM above 1 GiB, device interrupts, storage, file systems, networking, graphics, a shell.

## 4. Design alternatives

### 4.1 Timer and interrupt controller
- **[Option] PIT + 8259 PIC:** simplest, always present in QEMU's `pc` machine; legacy, coarse,
  and a dead end for SMP.
- **[Option] Local APIC timer + mask the 8259:** the modern path, per-CPU, needed for SMP later;
  its frequency must be calibrated (against the PIT or with CPUID/TSC information).
- **[Option] HPET or TSC-deadline:** more precise; more discovery work (ACPI) and not needed yet.
- **[Superseded by N2-D2: PIT and 8259 for N2]** Local APIC timer, calibrated once against the PIT, with the 8259 masked. The
  I/O APIC (and therefore ACPI MADT parsing) only becomes necessary for device interrupts, which
  N2 does not need beyond the timer.

### 4.2 Scheduler
- **[Proposal]** Single-CPU round-robin with a fixed time slice and an idle thread that `hlt`s.
  Priorities, fairness and real-time classes later.
- **[Open]** Whether the kernel itself is preemptible. **[Proposal]** Not in N2: interrupts stay
  disabled while in the kernel, so kernel code needs no locks on one CPU, and preemption happens
  only at the boundary back to user mode.

### 4.3 IPC model
- **[Option] Synchronous rendezvous (L4 style):** small, fast, simple to reason about; needs a
  careful design for asynchronous events.
- **[Option] Asynchronous channels with handles (Zircon style):** matches the architecture
  proposal (section 10 of the architecture document); needs kernel buffering and limits.
- **[Option] Byte pipes:** easiest to explain; weak for structured services.
- **[Proposal]** Channels with bounded, copied messages and handle passing deferred to the end of
  N2.5, because they fit the long-term architecture and keep the kernel free of message parsing.

### 4.4 System-call entry
- **[Option] Keep `int $0x80`:** proven in N1; slower; full frame saved.
- **[Option] Add `SYSCALL`/`SYSRET`:** faster; clobbers `rcx`/`r11` (ABI change), needs `swapgs`
  or an equivalent per-CPU pointer, a non-canonical return-address check, and care with
  interrupts on the user stack.
- **[Proposal]** Keep `int $0x80` through N2; revisit when performance is measured. Any change is a
  new ABI version.

### 4.5 Process creation
- **[Option] Kernel spawns from boot modules only** (N1's path, extended to several).
- **[Option] A `spawn` system call taking an executable from memory or a handle.**
- **[Option] User-space loader** (architecture proposal D6): the long-term direction.
- **[Proposal]** N2 creates processes from boot modules in the kernel (a list in a trusted boot
  configuration), and adds `spawn` only if IPC tests need dynamic creation. A user-space loader
  waits until there is storage.

### 4.6 Memory
- **[Proposal]** In N2: kernel W^X and a direct map (N2.0), a kernel heap (N2.2), per-thread
  kernel stacks with guard pages, `copy_to_user`. **Not in N2:** demand paging, copy-on-write,
  shared memory, memory above 1 GiB (unless the direct map makes it cheap), swapping.

## 5. Safety concerns N2 must address

- **User-memory checks under preemption.** N1's `copy_from_user` relies on nothing changing
  between validation and copy. With one thread per process and no shared memory, another
  process cannot change this process's page tables, so the argument survives if page tables are
  only changed by their owner's own system calls. This must be written down and tested again,
  and revisited before shared memory or threads within a process.
- **Interrupts in user mode.** IRQs will arrive with CS 0x23; the trap classifier must
  distinguish exceptions (kill), system calls (resume) and IRQs (handle, maybe reschedule,
  resume). A spurious or unexpected vector must never be treated as a user fault or vice versa.
- **Kernel stacks.** One RSP0 per thread, updated on every switch; canaries and guard pages;
  stack overflow in the kernel must fault, not corrupt.
- **Lifetimes.** A process can be killed while blocked in IPC or while another holds a handle to
  it; reference counting and a defined teardown order are needed to avoid use-after-free.
- **TLB and CR3.** Switching CR3 flushes non-global entries; the kernel must not mark user
  entries global. (SMP shootdowns are out of scope.)
- **Resource exhaustion.** Per-process limits for frames, handles and queued messages, so one
  program cannot starve the kernel.
- **NMI/#MC/#DF** on dedicated IST stacks, because a timer-driven kernel spends more time with
  interesting state on its stacks.

## 6. Beyond N2 (outline, not a commitment)

| Area | Depends on | Notes |
|---|---|---|
| Input (keyboard) and a text console | IRQs (N2.1), IPC (N2.5) | A first interactive program; prerequisite for VOS Terminal and `vsh` |
| Storage (virtio-blk first) | Device interrupts (I/O APIC/MSI), DMA policy, heap | DMA safety needs an IOMMU or a trusted driver model |
| File systems | Storage, a VFS-like service, IPC | Likely user-space services per the architecture proposal |
| Networking (virtio-net first) | Device drivers, buffers, IPC, a stack choice | Large; separate milestone |
| User-space loader / app manager | Storage or a ramdisk, `spawn`, IPC | Moves ELF parsing out of the kernel |
| SMP | APIC, per-CPU data, locking | Changes nearly every invariant; its own milestone |
| Graphics | Framebuffer handoff, IPC, memory sharing | After the above |

## 7. Decisions

Approved with the revised roadmap (2026-10-18):

| ID | Decision |
|---|---|
| N2-D1 | N2 scope and stages as in section 3 |
| N2-D2 | PIT and 8259 PIC for N2; APICs deferred |
| N2-D3 | Non-preemptible kernel; only code interrupted in ring 3 (or idle) is preempted |
| N2-D4 | Bounded channels with copied messages, non-blocking send, blocking receive |
| N2-D5 | Keep `int $0x80`; ABI v1 is an additive extension; v0 programs keep running |
| N2-D7 | Kernel W^X first (N2.0), by remapping the existing alias in place (done: a new directory under the existing PDPT[510]) |
| N2-D9 | `ARCH=x86_64` becomes the default build; `ARCH=i386` stays buildable and tested |
| N2-D10 | Static object pools instead of a general kernel heap in N2 |

Still open: N2-D6 details (whether v0 programs may use v1 calls — the proposal is no, they get
`-VOS_ENOSYS`), N2-D8 (installing GDB; not done in N2.0, see section 8), and whether to enable
CR4.MCE (without it a machine check shuts the machine down instead of raising #MC; see the
N2.0 limits in section 3.1).

## 8. Tooling (recorded, not installed)

GDB is not installed (approved in principle; installing it is a separate step). After
`sudo apt install gdb`: `make debug` (N2.2) builds the image and starts QEMU halted, waiting for
a debugger on TCP port 1234 (`-s -S`), with the serial port on the terminal; in another terminal
`gdb build/x86_64/vos.bin -ex 'target remote :1234' -ex 'break kernel_main' -ex continue`.
Useful scheduler breakpoints: `context_switch`, `thread_first_run`, `sched_idle_irq`, `panic`.
Test images are debugged the same way with `build/x86_64-test*/vos.bin` and their ISO. The x86_64 kernel is built with `-g`
from N2.0 so the debugger and `addr2line` can show source lines; the i386 flags are unchanged.
A cross toolchain is not needed yet.

Carried over from N1 and still open: the project licence. (`ARCH=x86_64` became the default in
N2.0, decision N2-D9.)
