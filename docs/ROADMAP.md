# VOS Roadmap

N0–N5 are complete and accepted within their authorized scopes. The owner formally
accepted N5 networking on 2026-10-25 as a bounded kernel-controlled prototype.
[PR #7](https://github.com/veasnawt/vos/pull/7) integrated it into main at
[`39b40a9`](https://github.com/veasnawt/vos/commit/39b40a904cce0778c307dd9154fff2531567d6d4).
Acceptance does not establish production networking or physical-hardware support.
N4, including Rixie Core v0, remains integrated at `bbca69d` (tree `50cbc8b`).
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
| N4 | Keyboard, buffered console, native Rust shell, cwd prompt, reusable Rust SDK, independent app and minimal offline Rixie Core v0 | [N4 records](results/n4-closure.md) |
| N5 | Bounded kernel virtio-net, Ethernet/ARP, IPv4/ICMP, UDP and passive TCP payload/retransmission/closure; owner accepted 2026-10-25 | [Networking](networking.md), [closure evidence](results/n5-tcp-close.md), [PR #7](https://github.com/veasnawt/vos/pull/7) |
| U0/U1 | QEMU UEFI feasibility/readiness, BIOS compatibility and framebuffer diagnostics | [UEFI plan/results](plans/uefi.md) |

## N4: complete, including Rixie Core v0

The original authorized shell/SDK scope is complete on
`feat/n4-interactive-console`, based on the integrated N3 baseline. After its
publication, the owner explicitly added the first minimal Rixie Core prototype
before final closure. [Rixie Core v0](results/n4-rixie-core.md) is now implemented
and verified as a small independent offline Rust userspace application. The
[closure record](results/n4-closure.md) links the original publications and the
scope update, focused host/BIOS/UEFI evidence and authentic captures. The owner
completed acceptance and integration at `bbca69d`; earlier readiness statements
remain records of the pre-merge checkpoint.

Delivered in the authorized order:

1. Keyboard input: bounded PS/2 input, US keys, modifiers, Backspace and Enter.
2. Interactive text console: buffered editing, completed-line delivery, overflow
   rejection and chronological scrolling, with explicitly granted ring-3 I/O.
3. Native `vsh`: Rust userspace parsing and a prompt derived from normalized cwd.
4. `help`, `clear`, `echo`, `pwd`, `ls`, `cd`, `cat`: working with bounded text and real read-only filesystem calls.
5. Rust-first native application foundations: shared bindings/runtime, a reusable
   ELF build and an independent example use the same C entry/syscall/ELF interfaces.

The subsequent Rixie extension reuses that SDK and the existing console ABI with
a separate trusted launch configuration. It explains commands and selects fixed
answers; it is not a conversational AI model. No external-program launcher,
network, GUI or model subsystem is part of this prototype.

N0–N3 and ABI v0–v3 remain preserved. The extended N4 scope is complete.

## N5: complete and formally accepted within the bounded prototype scope

The owner accepted N5's original and subsequently authorized bounded prototype
scope on 2026-10-25. [PR #7](https://github.com/veasnawt/vos/pull/7) merged closure
`9f443746` / feature integration `93053b81` at
`39b40a904cce0778c307dd9154fff2531567d6d4`. Its tree matches the verified feature
tree; feature-head build smoke and secret history audit passed.

`feat/n5-networking` began at the verified N4 merge. The
[first adapter increment](results/n5-first-network.md) initializes modern
virtio-net and exchanges bounded raw Ethernet frames in QEMU. The
[design and resource limits](networking.md) reuse existing PCI, PMM and kernel
isolation infrastructure. No application gains network access automatically.

The first increment is published at `27fe770`. Bounded
[Ethernet dispatch and ARP](results/n5-arp.md) now also pass focused host and
BIOS/UEFI checks, with a four-entry expiring neighbor cache and safe rejection
of malformed/unsupported input. ARP is published at `a6571c1`.
[Native IPv4 and ICMP echo](results/n5-ipv4.md) now pass focused host and
BIOS/UEFI checks: two originated pings and one incoming request answered per
boot, with independent PCAP validation, published at `33693b3`.
[Kernel UDP](results/n5-udp.md) now passes focused host and BIOS/UEFI checks,
with one explicit endpoint, bounded copied datagrams and correct checksum-zero
handling, published at `a5889f9`. A [passive TCP prototype](results/n5-tcp.md)
now completes the handshake and bounded timeout/cleanup checks on both firmware
paths, published at `4e799c2`. [Opt-in payload exchange](results/n5-tcp-data.md)
now verifies fixed buffers, in-order reception, partial ACKs, real receive-window
accounting and explicit timeout cleanup, published at `ff251a7`.
[Bounded retransmission](results/n5-tcp-retry.md) now recovers controlled data and
ACK loss with fixed retry limits, published at d512db7.
[Bounded connection closure](results/n5-tcp-close.md) now verifies FIN states,
reset validation, retry/timeout interaction and cleanup, now merged through PR #7.
Adaptive recovery, out-of-order reassembly and production ISNs remain unsupported.
The owner merged published networking through
IPv4 into GitHub main via [PR #4](https://github.com/veasnawt/vos/pull/4) at
`d01b3f5` while UDP verification ran, then PR #5 advanced main to `ffaf2fb` with
the TCP establishment tree 4e799c2. The feature now has payload and retry
publications, subsequently integrated by the owner through PR #6 at 369153d.
Those historical integrations and pre-publication branch states remain recorded
in the original verification reports; they do not describe the accepted PR #7
checkpoint. The accepted TCP profile has one passive connection, fixed 64-byte
RX/TX buffers and a 504-byte context. Live TIME-WAIT is explicitly aborted after
its checks; full 2xMSL expiry is host-tested. Production networking, DNS, DHCP,
IPv6, TLS and userspace networking remain deferred. Verification is QEMU-only;
physical x86_64 compatibility has not been demonstrated. An eventual
userspace network interface requires explicit process-owned capability grants,
resource bounds and revocation. Rixie Connected remains outside this increment.
Focused checks accompany changes; no N3 campaign or new milestone is introduced.

## Later work

| Area | Scope still to be designed/authorized |
| --- | --- |
| U2 | IOAPIC/local APIC or MSI-X support and broader verified framebuffer formats |
| U3 | Independent EFI loader, then Secure Boot |
| H2 | Controlled physical-hardware tests, with explicit approval of each step |
| Other work | Writable storage and native UI/platform work beyond the authorized N5 networking scope remain future designs |

Current limitations are listed in [STATUS](STATUS.md) and the closure record:
QEMU-only verification, trusted immutable read-only media, RAM tracked below
1 GiB, UEFI polling, bounded operations without real-time/fairness guarantees,
retained DMA quarantine after unconfirmed reset, incomplete full dynamic stack
high-water proof and the documented deep/sparse-directory progress limits.
