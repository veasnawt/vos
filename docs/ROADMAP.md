# VOS Roadmap

N0–N4 implementation objectives are complete; N4 is ready for integration.
N3 closure was accepted on 2026-10-24 after the existing
D1/D2/C20 requirements passed. The milestone is published on
`feat/n3.6-storage-closure` at `897312f`. GitHub main integrated it through
PR #2 (`d9a4ade`) during N4 development. Local main and the N4 branch were
fast-forwarded to that merge; its source tree is identical to the N3 baseline.
See [status](STATUS.md) and the [N3 closure record](results/n3-closure.md).

## Completed

| Milestone | Delivered capability | Record |
| --- | --- | --- |
| N0 | Original i386 boot and serial baseline | `m1-32bit` history; legacy `arch/i386/` |
| N1 | Ring-3 ELF programs and ABI v0 system calls | [Plan/results](plans/n1-user-space.md) |
| N2 | Protected memory/stacks, kernel threads, user preemption and ABI v1 channels | [Foundation](plans/n2-foundation.md) |
| N3.0 | Storage-safe higher-half kernel map | [Storage plan](plans/n3-storage.md) |
| N3.1 | Read-only virtio-blk | [Storage plan](plans/n3-storage.md) |
| N3.2 | Block registry and read-only cache | [Storage plan](plans/n3-storage.md) |
| N3.3 | Read-only ext2 reader | [Storage plan](plans/n3-storage.md) |
| N3.4 | Bounded interrupt-driven block I/O and polling fallback | [Storage plan](plans/n3-storage.md) |
| N3.5 | Capability-scoped read-only ABI v2 files/directories | [Historical closure](results/n3.5-closure.md) |
| N3.6 / N3 | Integrated accounting/stress, final regression, five mutations, fifteen larger-RAM runs and preserved evidence | [Final N3 closure](results/n3-closure.md) |
| N4 | Keyboard, buffered console, native Rust shell, cwd prompt, reusable Rust SDK and independent app | [Final N4 closure](results/n4-closure.md) |
| U0/U1 | QEMU UEFI feasibility/readiness, BIOS compatibility and framebuffer diagnostics | [UEFI plan/results](plans/uefi.md) |

## N4: published and ready for integration

The original authorized scope is complete on `feat/n4-interactive-console`,
based on the integrated N3 baseline. The [closure record](results/n4-closure.md)
links the five implementation publications, focused host/BIOS/UEFI evidence and
authentic captures. The owner will create the GitHub pull request and merge;
this publication does not alter main.

Delivered in the authorized order:

1. Keyboard input: bounded PS/2 input, US keys, modifiers, Backspace and Enter.
2. Interactive text console: buffered editing, completed-line delivery, overflow
   rejection and chronological scrolling, with explicitly granted ring-3 I/O.
3. Native `vsh`: Rust userspace parsing and a prompt derived from normalized cwd.
4. `help`, `clear`, `echo`, `pwd`, `ls`, `cd`, `cat`: working with bounded text and real read-only filesystem calls.
5. Rust-first native application foundations: shared bindings/runtime, a reusable
   ELF build and an independent example use the same C entry/syscall/ELF interfaces.

N0–N3 and ABI v0–v3 remain preserved. No further feature or verification campaign
is required for this N4 scope. Further development requires separate authorization.

## Later work

| Area | Scope still to be designed/authorized |
| --- | --- |
| U2 | IOAPIC/local APIC or MSI-X support and broader verified framebuffer formats |
| U3 | Independent EFI loader, then Secure Boot |
| H2 | Controlled physical-hardware tests, with explicit approval of each step |
| Other milestones | Writable storage, networking and later native UI/platform work remain future designs |

Current limitations are listed in [STATUS](STATUS.md) and the closure record:
QEMU-only verification, trusted immutable read-only media, RAM tracked below
1 GiB, UEFI polling, bounded operations without real-time/fairness guarantees,
retained DMA quarantine after unconfirmed reset, incomplete full dynamic stack
high-water proof and the documented deep/sparse-directory progress limits.
