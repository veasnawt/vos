# VOS Roadmap

**N0-N5 are complete and accepted within their authorized scopes.**
Unified native application launching and the N6.1-N6.3 graphical components are
also accepted and merged. **N6 remains in progress.** The owner-accepted
graphical baseline is [PR #15](https://github.com/veasnawt/vos/pull/15) at
[`bf5eb43f2e07478e31be8b4123c5a297add67efe`](https://github.com/veasnawt/vos/commit/bf5eb43f2e07478e31be8b4123c5a297add67efe).
PR #16 synchronized documentation; PRs #17/#18 added the reviewed infrastructure
foundation. Master Plan PR #19 is merged. Accepted main for this review is the
owner's USB.2-A PR #23 merge, `86493ff2b2a8f2d84c4ce3ed4d0ccf132d97a072`,
following R0/R1 and USB.1 integration PR #22.

The accepted milestone sequence is preserved below. Proposed desktop,
Rust-kernel and portability work is **not implementation authorization**.
VOS is pre-alpha and targets independent physical x86_64 computers; current
recorded acceptance verification is QEMU SeaBIOS/OVMF only; the partial physical
boot observation below is not hardware acceptance. See [STATUS](STATUS.md) for precise limits.
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

## Rust kernel foundation and proposed migration/portability

[R0-A](results/r0a-rust-kernel-assessment.md) is complete as a read-only assessment.
[R0-B](results/r0b-rust-kernel-foundation.md) is implemented and locally verified:
one freestanding Rust static library, a checked scalar C-ABI call in the
higher-half kernel, and a minimal panic bridge. Both original commits and their
evidence were integrated through PR #20 at
`0291b1ab8f7d7d491bd2f276efa59a46b638d400`.
[Final-tree review](results/r0-publication-review.md) records provenance
and focused publication verification; the dated reports retain historical status.

The first [R1 helper](results/r1-elf-bounds.md) is implemented and locally verified
at `f67ebd968169560979fc1d5eefbc7ca6402a6910`: the existing ELF bounds predicate
now calls Rust. Its [performance/publication review](results/r1-performance.md)
preserves the candidate and compares the original C and real Rust-backed paths.
It is merged through PR #21 at `7c8082b98751f65ab0bcdf884339a6bf8298e2ce`;
further R1 helpers and R2-R5 remain
future, separately authorized increments under the
[Master Plan R-series](plans/MASTER-PLAN.md#6-r-series-rust-first-kernel-migration).
The kernel is still primarily C/assembly. Further migration must define small
interoperable boundaries, preserve ABIs/capabilities, and audit unsafe Rust,
assembly, FFI, interrupts and DMA ownership. Rust application support remains
implemented independently of this kernel proof.

USB.2-A's [portable Rust core](results/usb2a-rust-core.md) is merged through
PR #23. It models bounded validation, rings and lifecycle without operating a
controller. The separately authorized
[USB.2-B ownership foundation](results/usb2b-resource-ownership.md) is merged
through PR #25 at `d2c0ac2d92bb917531a372d21d6b432425c4645f`. It adds non-device
scoped mapping/PMM ownership and worker fixtures, not a live backend. USB.2-C
activation remains separately authorized future work. USB.1's original evidence
is preserved.

The first R2 increment, the [R2-B Rust PS/2 pointer decoder](results/r2b-ps2-pointer-rust.md),
is merged through PR #26 at `d8edfcec643e541cc882062e5121175b50778f89`. Only the
pure packet/idle decoder moved to Rust; the PS/2 controller, routing and GUI
delivery remain C. Further R2 drivers and R3-R5 remain separately authorized.

[USB.2-C1](results/usb2c1-qualification.md), the first USB.2-C increment, is merged
through PR #27 at `84a0c15c223001411499f396a6a89cc98684d6d3`: one canonical Rust USB
tree, the USB.2-A controller linked but never instantiated, and opt-in read-only
qualification of the QEMU xHCI profile through a releasable BAR lease.
[USB.2-C2](results/usb2c2-dma.md) is merged through PR #28 at
`62ce8de2b3d4b12e53c465f9ad3bb92fb0bfeffe`: Rust-owned DMA-designated pages with
publication statically unavailable. [USB.2-C3a](results/usb2c3a-residue.md) is
merged through PR #29 at `9c93d549678e37b4857e3a927971972583486983`: an opt-in
firmware DMA residue guard and a dry-run halt/reset plan that is never executed.
[USB.2-C3a.2](results/usb2c3a2-efi-exclusion.md) is merged through PR #30 at
`662a3ccb6d1111393c30c486ca4af657fac5808e`: EFI boot-services and other non-RAM
memory is excluded from the PMM on every UEFI boot before the first allocation.
[USB.2-C3a.3](results/usb2c3a3-protected-inspection.md) is merged through PR #31 at
`55f6693a4e537248c912aaed61f0194cc07be811`: bounded, read-only inspection of that
excluded firmware DMA memory, with reuse protection and inspection completeness
reported separately. [USB.2-C3b.1](results/usb2c3b1-containment-dry-run.md) is
merged through PR #32 at `6eb9a30be63e29667a75b9a04c4cafe994f25e3c`: a read-only
rehearsal of controller containment with no write path.
[USB.2-C3b.2](results/usb2c3b2-controlled-containment.md) is merged through PR #33 at
`03e7532aba739c636438c887e8aa9d6169f4cfcb`: the first, QEMU-only, test-image-only
controller writes (clear Bus Master Enable, then HCRST) for the one eligible idle
UEFI QEMU xHCI. [USB.2-C4.1](results/usb2c41-hosted-init-model.md) is a hosted-only Rust
protocol model merged through PR #34 at `dbb7d1d06452414c9674d9f9bb4b6e08d859da59`.
[USB.2-C4.2a](results/usb2c42a-hosted-backend-contract.md) is the hosted backend contract for it in
this PR candidate (no kernel, no device, no new write path). USB.2-C4.2b and later, native USB.2-C4
stages, physical-PC activation and USB HID require separate authorization; physical
controller takeover remains unauthorized.
This status update grants no implementation authority.
Unrelated bug fixes, boot-media work and desktop planning need not
wait for the full migration. Hardware and migration work coordinate when sharing
files; neither authorizes the other automatically.

Real physical x86_64 operation remains the target. Broader hardware portability
requires explicit firmware/boot, hardware discovery, APIC/interrupt, memory/DMA,
storage, input and display resource contracts plus controlled physical testing.
Other architecture ports are proposals requiring their own feasibility and
owner authorization. Emulator success alone is not physical-hardware evidence.

## Hardware and USB follow-up

[USB.1](plans/MASTER-PLAN.md#71-usb-discovery-and-proposed-input-sequence) is
implemented originally at
`bd22803b8b5635b776c848d08d1494f65b49dd21` on `feat/usb-xhci-discovery`.
The [Rust-enabled integration](results/usb1-rust-integration.md) preserves that
provenance and is merged through PR #22; its dated report remains unchanged.
The [USB.1 discovery migration to Rust](results/usb1-rust-publication-review.md)
is accepted through PR #24 at `f24b3a803a65324f31cdb4c43fd4f504394a7cdb`,
with physical verification still unperformed.
It provides opt-in, read-only bus-0 xHCI discovery/diagnostics, not USB
keyboard/mouse input. USB.2-A is software-only; live controller operations remain
outside this publication review.
Proposed USB.2–USB.5 cover controller operation, enumeration/keyboard HID,
unified keyboard routing and mouse HID within the existing H-series scope.

The owner reports DD-mode Rufus media reaching a physical graphical display,
with GUI/shell startup failures; functional GUI, shell and USB input remain
undemonstrated. PS/2 initialization is a hypothesis, not a confirmed cause.
[Rufus ISO-mode compatibility](plans/MASTER-PLAN.md#72-owner-reported-physical-boot-and-rufus-compatibility)
is a separate proposed boot-media task, preserving DD mode and requiring
BIOS/UEFI and physical validation before compatibility claims. Publication review,
USB, boot-media work, R0-A and desktop development are coordinated priorities,
not mandatory serial gates or permission to begin implementation.

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
