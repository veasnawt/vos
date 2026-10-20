# VOS Roadmap

The detailed plans and results live in `docs/plans/`; this page is the summary.
Status is updated at each checkpoint (see `docs/STATUS.md`).

## Completed

| Milestone | Content | Record |
|---|---|---|
| N1 | User-mode execution, ELF loading, system calls (ABI v0) | `docs/plans/n1-user-space.md` |
| N2 | Kernel threads, preemptive scheduling of user programs, IPC channels (ABI v1) | `docs/plans/n2-foundation.md` |
| N3.0 | Higher-half kernel map with W^X, extended to 4 MiB of 4 KiB pages | `docs/plans/n3-storage.md` §3 |
| N3.1 | Read-only virtio-blk driver | `docs/plans/n3-storage.md` §5 |
| N3.2 | Block layer and read-only cache | `docs/plans/n3-storage.md` §6 |
| N3.3 | Read-only ext2 reader (kernel-internal) | `docs/plans/n3-storage.md` §7 |
| N3.4 | Interrupt-driven block I/O: deadlines, device IRQ layer, cancellation | `docs/plans/n3-storage.md` §9 |
| U0 | UEFI feasibility under OVMF | `docs/plans/uefi.md` §1–7 |
| U1 | UEFI readiness with BIOS compatibility: low-memory loader data, defined CPU state, device-memory window, framebuffer screen console, `make uefi-iso` / `make test-uefi` | `docs/plans/uefi.md` §8 |

## Remaining

| Milestone | Content |
|---|---|
| N3.5 **INCOMPLETE** | [Direction conditionally approved, 2026-10-20](plans/n3.5-file-api.md); Stage 1 bounded storage foundations authorized, then stop for review. [ABI v2](abi/syscall-v2.md), file handles and later stages remain proposed and unimplemented. |
| U2 | Broader UEFI and interrupt-controller support: local APIC / IOAPIC (ACPI MADT) or MSI-X, so devices can interrupt under UEFI; more framebuffer formats as verified |
| U3 | Independent VOS EFI loader, then Secure Boot (own keys first, test keys in OVMF) |
| H2 | Controlled physical-hardware testing (first: Dell OptiPlex 9020), only with explicit approval of each step |

## Provisional (not designed)

N4, N5, N6 and later are placeholders. Their content is not decided and needs a design
document and approval before any work starts.
