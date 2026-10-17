# N2 — Operating-System Foundation (Roadmap Proposal)

| | |
|---|---|
| Status | **Approved as the current development plan (2026-10-18), revised after the N2 architecture review.** Only N2.0 is authorized; every later stage needs explicit approval. Sections marked [Proposal] or [Option] are still design, not implementation. |
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

## 7. Decisions

Approved with the revised roadmap (2026-10-18):

| ID | Decision |
|---|---|
| N2-D1 | N2 scope and stages as in section 3 |
| N2-D2 | PIT and 8259 PIC for N2; APICs deferred |
| N2-D3 | Non-preemptible kernel; only code interrupted in ring 3 (or idle) is preempted |
| N2-D4 | Bounded channels with copied messages, non-blocking send, blocking receive |
| N2-D5 | Keep `int $0x80`; ABI v1 is an additive extension; v0 programs keep running |
| N2-D7 | Kernel W^X first (N2.0), by remapping the existing alias in place |
| N2-D9 | `ARCH=x86_64` becomes the default build; `ARCH=i386` stays buildable and tested |
| N2-D10 | Static object pools instead of a general kernel heap in N2 |

Still open: N2-D6 details (whether v0 programs may use v1 calls — the proposal is no, they get
`-VOS_ENOSYS`), N2-D8 (installing GDB; not done in N2.0, see section 8), and whether to enable
CR4.MCE (without it a machine check shuts the machine down instead of raising #MC; noted in
N2.0).

## 8. Tooling (recorded, not installed)

GDB is not installed. When approved: `sudo apt install gdb`, then debug with
`qemu-system-x86_64 -cdrom build/x86_64/vos.iso -s -S ...` and
`gdb build/x86_64/vos.bin -ex 'target remote :1234'`. The x86_64 kernel is built with `-g`
from N2.0 so the debugger and `addr2line` can show source lines; the i386 flags are unchanged.
A cross toolchain is not needed yet.

Carried over from N1 and still open: the project licence; whether `ARCH=x86_64` becomes the
default (see the N1 plan, section 15).
