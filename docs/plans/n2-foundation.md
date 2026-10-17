# N2 — Operating-System Foundation (Roadmap Proposal)

| | |
|---|---|
| Status | **Planning only. Not approved.** Nothing here is implemented, and every stage needs explicit approval before work starts. |
| Date | 2026-10-17 |
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

## 3. Proposed stages

Each stage keeps `make test-all` green, adds its own deterministic tests and stops for review.

| Stage | Content | Acceptance (examples) |
|---|---|---|
| **N2.0** Hardening carried over from N1 | Kernel W^X: remap the kernel with 4 KiB pages (text R-X, rodata R, data RW NX) and stop using the boot alias for code; a separate direct map of RAM (NX, supervisor) for page-table and frame access; IST stacks for NMI and #MC; guard pages under kernel stacks | A kernel write to its own text faults; executing the direct map faults; NMI injected by QEMU is handled on its own stack |
| **N2.1** Interrupt controller and timer | Mask and remap the legacy 8259 PIC; local APIC timer (or PIT, see 4.1); IRQ entry stubs sharing the exception path; EOI; spurious-interrupt handling; interrupts enabled in user mode, disabled in the kernel at first | Tick counter advances at the expected rate (within tolerance) under QEMU; an IRQ arriving in ring 3 shows CS 0x23 in its frame and returns to the same instruction; all N1 tests still pass with interrupts on |
| **N2.2** Kernel heap and objects | A small kernel allocator (slab-style for fixed-size objects) on top of the PMM; reference-counted kernel objects (process, thread, channel) with explicit ownership | Allocation/free round trips; no leak after create/destroy cycles; object counts return to baseline |
| **N2.3** Threads and scheduler | Per-thread kernel stacks and saved contexts; context switch; one user thread per process at first; round-robin preemptive scheduler with an idle thread; `yield` system call | Two programs printing alternately; an endless loop no longer blocks the system; canaries on every kernel stack |
| **N2.4** Multiple processes and lifecycle | Process table; create from boot modules (several at once); exit status; kernel-side termination (fault, policy); `wait`-style notification for the creator; CR3 switch on context switch; safe teardown while others run | Faulting process killed while others continue; frames and objects back to baseline after all exit |
| **N2.5** Basic IPC | Handles with rights; channels carrying small messages (copied, bounded size); blocking receive with wakeup; peer-closed notification | Ping-pong between two processes; closing one end wakes the other with "peer closed"; message size and handle limits enforced |
| **N2.6** ABI v1 and user runtime | Versioned system-call table for the new calls; `copy_to_user`; revised user-memory rules for preemption (section 5); possibly `SYSCALL` entry (section 4.4); runtime updated, still static | ABI v1 programs run; ABI v0 `hello` either still runs or is rejected by version, as decided |
| **N2.7** Isolation suite v2 and documentation | Extend the N1.9 suite to concurrency: faults during IPC, killing a blocked process, exhausting handles/memory per process | Full suite under qemu64, 2 GiB and `-cpu max` |

Dependencies: N2.0 → N2.1 → N2.2 → N2.3 → N2.4 → N2.5 → N2.6 → N2.7. N2.0 could move later if
time matters more than defense in depth, but it gets harder once more code depends on the alias.

## 4. Design alternatives

### 4.1 Timer and interrupt controller
- **[Option] PIT + 8259 PIC:** simplest, always present in QEMU's `pc` machine; legacy, coarse,
  and a dead end for SMP.
- **[Option] Local APIC timer + mask the 8259:** the modern path, per-CPU, needed for SMP later;
  its frequency must be calibrated (against the PIT or with CPUID/TSC information).
- **[Option] HPET or TSC-deadline:** more precise; more discovery work (ACPI) and not needed yet.
- **[Proposal]** Local APIC timer, calibrated once against the PIT, with the 8259 masked. The
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

## 7. Decisions requiring approval

| ID | Decision | Recommendation |
|---|---|---|
| N2-D1 | N2 scope as defined in section 2 | Approve as the bounded target |
| N2-D2 | Timer source | Local APIC timer calibrated with the PIT |
| N2-D3 | Kernel preemption | Non-preemptible kernel in N2 |
| N2-D4 | IPC primitive | Bounded channels with copied messages; handle passing late in N2 |
| N2-D5 | System-call entry | Keep `int $0x80`; new calls make ABI v1 |
| N2-D6 | ABI v0 compatibility | Decide whether ABI v0 programs keep running after ABI v1 or are rejected |
| N2-D7 | Order of kernel W^X (N2.0) | First, before more code depends on the alias |
| N2-D8 | Debugging tools | Install GDB (and consider an `x86_64-elf` cross toolchain) before N2.3; both need approval |

Carried over from N1 and still open: the project licence; whether `ARCH=x86_64` becomes the
default (see the N1 plan, section 15).
