# VOS Roadmap

**N0-N5 are complete and accepted within their authorized scopes.**
Unified native application launching and the N6.1-N6.3 graphical components are
also accepted and merged. **N6 remains in progress.** The owner-accepted
graphical baseline is [PR #15](https://github.com/veasnawt/vos/pull/15) at
[`bf5eb43f2e07478e31be8b4123c5a297add67efe`](https://github.com/veasnawt/vos/commit/bf5eb43f2e07478e31be8b4123c5a297add67efe).
PR #16 synchronized documentation; accepted main at this planning integration is
`cbe9f9202ca9a45aaafa57ec45fc01baeb347bc1`, after infrastructure PRs #17/#18.

The accepted milestone sequence is preserved below. Proposed desktop,
Rust-kernel and portability work is **not implementation authorization**.
VOS is pre-alpha and targets independent physical x86_64 computers; current
verification is QEMU SeaBIOS/OVMF only. See [STATUS](STATUS.md) for precise limits.
Historical publication and branch narratives remain in their existing reports.

## Plan navigation

The [Master Development Plan](plans/MASTER-PLAN.md) consolidates the **proposed
long-term strategy**, including N, R, H, D, NET, WF and W tracks. Its integration
does not approve implementation or make the full N6 desktop complete.

- This roadmap provides high-level navigation and the current milestone overview.
- [MASTER-PLAN](plans/MASTER-PLAN.md) records cross-track proposals, dependencies,
  open decisions and provenance; it remains subject to owner review.
- [STATUS](STATUS.md) records verified implementation and acceptance state.
- [Individual plans](plans/) retain detailed engineering specifications and their
  own approval status; future subplans are not created as empty placeholders.
- [Results](results/) preserve historical verification evidence unchanged.

The owner's later decision stopped the D1 controller-transition project and
withdrew the legacy maintainer-controller requirement. Historical controllers,
reservations and interrupted work remain preserved. The master plan does not
restart D1 or make its completion a prerequisite for ordinary scoped development.

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
| Unified apps | Common payload/catalog, ABI v4 broker, isolated foreground children and exclusive console restoration | [Payload](results/app-payload.md), [broker](results/app-exec.md), [handoff](results/app-handoff.md); PRs #9-#11 |
| N6.1 | Framebuffer scene, one system bar and bounded console viewport | [Desktop slice](results/n6-desktop.md), PR #12 |
| N6.2 | Bounded native GUI surface/events, keyboard delivery and bootstrap rollback | [GUI and hardening](results/n6-gui.md), PR #13 |
| N6.3 | PS/2 pointer input, primary-button routing, clipped cursor and accepted visibility refinement | [Pointer](results/n6-pointer.md), [cursor refinement](results/n6-cursor-refinement.md), PRs #14-#15 |

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

## N6: graphical desktop in progress

The accepted slices establish a foundation, not a complete desktop:

1. N6.1 reuses the supported GRUB framebuffer path, private bounded renderer and
   existing shell console. PR #12 integrated `20d9b93` at `3308cc8`.
2. N6.2 adds a single explicitly granted ring-3 surface and bounded event queue.
   PR #13 integrated foundation `e639008` plus bootstrap cleanup `9f59802`
   at `dfd292c`. Eighteen injected ELF-load failures per firmware verify worker
   reclamation; historical interruptions and recoveries remain recorded.
3. N6.3 adds standard PS/2 mouse input through the existing i8042 owner,
   primary-button semantics, secure event routing and cursor restoration.
   PR #14 integrated `dccee3b` at `78cc80c`. PR #15 then integrated cursor
   refinement `ab3d4de` at the accepted baseline above. The owner accepted its
   manual preview. Cursor size is 21x28 visible pixels on a 32x32 canvas;
   future size customization belongs to Settings/Accessibility planning.

Existing [N6.1](results/n6-desktop.md), [N6.2](results/n6-gui.md),
[N6.3](results/n6-pointer.md) and [cursor](results/n6-cursor-refinement.md)
reports preserve their original local-candidate wording, raw evidence and failure
history. Their publication/acceptance status is recorded here; they are not
retroactively rewritten.

Next desktop increments are planning candidates requiring owner approval:
bounded multi-surface composition and ownership/focus, window interaction,
a native Rust Terminal using the existing shell, then appropriate desktop/dock
and Settings integration. No new increment or implementation order is approved
by this synchronization. Input, rendering and native application UI should
remain separate; a complete application UI must not be embedded in the kernel.

The approved visual target uses the name **Terminal**, a minimalist dark-blue
desktop, a bottom-centered floating pill dock and compact circular window
controls. VOS Display and logo sources require separately approved identity,
licensing and asset integration. VICons remains a candidate; the GUI architecture
is icon-library-agnostic and requires no npm/React runtime. These are design
directions, not implemented desktop features.

## Proposed Rust-first kernel migration and portability

A Rust-first kernel migration is a proposed planning track. No migration or
R0-A execution is authorized here. A future plan must define small interoperable
boundaries, preserve supported ABIs and capability semantics, and audit unsafe
Rust, assembly, FFI, interrupt entry and DMA ownership. Existing Rust application
support is implemented; kernel migration is not.

Real physical x86_64 operation remains the target. Broader hardware portability
requires explicit firmware/boot, hardware discovery, APIC/interrupt, memory/DMA,
storage, input and display resource contracts plus controlled physical testing.
Other architecture ports are proposals requiring their own feasibility and
owner authorization. Emulator success alone is not physical-hardware evidence.

## Later work

| Area | Scope still to be designed/authorized |
| --- | --- |
| U2 | IOAPIC/local APIC or MSI-X support and broader verified framebuffer formats |
| U3 | Independent EFI loader, then Secure Boot |
| H2 (historical UEFI roadmap label) | Controlled physical-hardware tests, with explicit approval of each step; the master plan's proposed H-series taxonomy does not renumber this record |
| Other work | Writable storage, production/userspace networking and platform expansion remain separate future designs; the incomplete N6 desktop work is described above |

Current limitations are listed in [STATUS](STATUS.md) and the closure record:
QEMU-only verification, trusted immutable read-only media, RAM tracked below
1 GiB, UEFI polling, bounded operations without real-time/fairness guarantees,
retained DMA quarantine after unconfirmed reset, incomplete full dynamic stack
high-water proof and the documented deep/sparse-directory progress limits.
