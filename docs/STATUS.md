# VOS Status

**N4 is complete, accepted and integrated into main** at
`bbca69d850e8c168bfa824ace475a27226b2cdb7`. Its tree matches accepted publication
`50cbc8bc49bd10d5272647b267ed23895d2f533d`, including Rixie Core v0.
[N4 records](results/n4-closure.md) preserve the pre-merge readiness and evidence.

**N5 development is authorized and in progress** on `feat/n5-networking`, created
from that verified merge after safely fast-forwarding local main. The
[first networking increment](results/n5-first-network.md) provides bounded modern
virtio-net initialization and raw frame I/O. Host checks and isolated BIOS/UEFI
frame exchanges pass. There are no application network grants or new syscalls.
The first 40-file increment is published at `27fe770676b4ce138eb9596c881a556c820cbe74`.
[Ethernet/ARP](results/n5-arp.md) now passes 4,598 sanitizer-backed host checks
and both firmware wire checks (4 TX/11 RX, 8 rejected frames each). It reuses
the unchanged driver and a four-entry expiring cache, published at `a6571c1`.
[Native IPv4/ICMP](results/n5-ipv4.md) now passes 1,806 sanitizer-backed checks
plus the 4,598-check ARP host regression. Each BIOS/UEFI boot completes two pings,
answers one request and rejects 11 malformed/unsupported/unmatched packets.
UDP/TCP and explicit application network authority remain next.
Rixie Connected is not started. The existing 272 MiB continuation meter is enforced
alongside the unchanged inherited 3360 MiB aggregate ceiling and host reserves.

N3 remains complete and accepted on 2026-10-24.
[Final N3 closure and evidence](results/n3-closure.md) consolidate D1a–D1f,
five mutation controls and fifteen larger-RAM stress runs.

**N3 publication:** `897312fb991b2f55799b37fbd4459fb552acfe82` on
`feat/n3.6-storage-closure`. GitHub main integrated it through PR #2 at
`d9a4ade0db1df8ab7c8c7fe3c4553cd3f36c3622` during N4 development. Local main
and `feat/n4-interactive-console` were fast-forwarded to that merge; its tree
is identical to N3. The first N4 keyboard/echo implementation is published on
`feat/n4-interactive-console`. No new release tag was created; the older U1 tag
remains `checkpoint-2026-10-19-u1`.

**The original N4 scope is complete.** The [first input increment](results/n4-first-input.md)
is published at `3a2b642`; [buffered input](results/n4-buffered-input.md) is
published at `87b99dfc867c45bd01f512cf9ecac0117d1a6eac`. The subsequent
[native Rust vsh](results/n4-native-vsh.md) runs in ring 3 with a granted ABI v3
console and supports all seven requested commands plus `exit`. File commands
use real read-only ext2/ABI v2 operations in an explicitly granted namespace.
Host checks and BIOS/UEFI runtime checks pass, including five ring-3 console
probes per firmware and the original normal boot-and-halt path. Native shell
publication is `1beb40677bfeb605da5b2920b70af0fc24138493`.
The subsequent [shared Rust SDK and independent app](results/n4-rust-foundations.md)
also pass host, loader and BIOS/UEFI checks and are published at
`f7ac43478ea548ddc4bb74ae177b0bb828e67596`. The requested
[normalized cwd prompt](results/n4-dynamic-prompt.md) is published at
`bdde1bc33f99030ba51fa78078a6db90f87f2a07`; three host tests and both firmware
interactions pass. [Rixie Core v0](results/n4-rixie-core.md) subsequently passes
six host tests and 17 interactions on each firmware, plus independent vsh smoke
checks. It is a deterministic offline Rust ring-3 application with a console-only
grant, launched through its own trusted boot configuration. No external-app shell
launcher or new kernel subsystem was added. Main now contains this accepted N4
implementation; the older publication statements below remain historical.

Historical N3.5 approval, integration, failed attempts and original authorization
boundaries below remain records of those checkpoints, not current N3 blockers.

VOS is an experimental, independently written x86_64 kernel (with a legacy i386 kernel). It
runs only in QEMU so far: no physical machine has booted it. This is a development checkpoint,
not a release.

## Completed milestones

N4 now delivers keyboard input, the buffered console, native Rust `vsh` with the
seven requested commands and dynamic cwd prompt, and reusable Rust application
foundations with an independent example. The authorized extension adds the first
minimal Rixie Core prototype. All are integrated into main at the verified N4 merge.

N1 (user mode, system calls), N2 (scheduling, preemption of user programs, IPC), N3.0–N3.4
(W^X higher-half memory, virtio-blk, block cache, read-only ext2, interrupt-driven block I/O),
N3.5 (capability-scoped read-only userspace file ABI v2), N3.6/N3 (integrated
storage stress and final closure), U0 (UEFI feasibility) and U1 (UEFI readiness
with BIOS compatibility).
See `docs/ROADMAP.md`.

## Boot paths (all verified in QEMU only)

| Firmware | Loader | Media | Screen |
|---|---|---|---|
| UEFI: OVMF 2025.11, q35, Secure Boot not enabled | GRUB 2.14 x86_64-efi (`make uefi-iso`) | CD; USB stick (xHCI) | GOP framebuffer console |
| BIOS: SeaBIOS (QEMU pc and q35) | GRUB 2.14 i386-pc (`make iso`) | CD; USB stick (hybrid MBR) | VBE framebuffer console |

On both paths:
- Multiboot2 handoff, with loader data accepted below 1 MiB under explicit rules.
- Serial log on COM1 (38400 8N1).
- Boots without a serial port, or without a framebuffer (then serial only).

## Kernel capabilities

**Memory and protection:**
- Higher-half kernel with a W^X map, NX, and SMEP/SMAP/UMIP when the CPU has them.
- A defined CR0/CR4/EFER state on every boot path: SYSCALL disabled, no RDPMC, WRFSBASE or
  XSAVE in ring 3.
- The PMM tracks RAM below 1 GiB.
- An audited, supervisor-only device-memory window.

**Execution:**
- Kernel threads.
- Ring-3 programs in separate address spaces, preempted by the timer.
- IPC channels.
- System-call ABI v0/v1, plus integrated N3.5 ABI v2 file operations;
  [Stage 3 implementation](results/n3.5-stage3.md) and [Stage 4 security verification](results/n3.5-stage4.md).
  Stage 4 closure is approved. [Stage 5 final verification](results/n3.5-stage5.md)
  passes 74 BIOS and 38 fresh UEFI runs, 845/0 file assertions, 302/0 static checks
  and five mutation controls. Frozen hashes and resource budgets hold.
  N3.5 closure is approved. N3.6's accounting foundations, deterministic stress
  and final D1/D2/C20 verification are also complete; see the final N3 record below.

**Interrupts and timing:**
- 8259 PIC and PIT.
- Device IRQ lines with top and bottom halves, and a storm guard.
- Deadlines for blocking waits.

**Storage (read-only):**
- virtio-blk, interrupt-driven where a level-triggered INTx line exists, otherwise polling.
- Block cache.
- ext2 reader and capability-scoped read-only ABI v2 file calls in local main.
  Only explicit trusted launches receive non-root directory grants; normal
  hello still requires no disk. No writable operations or authenticated-media claim.

**Display:**
- Framebuffer console with an original 8×16 font, status bar (PASS/FAIL) and panic banner.
- VGA text only when text mode is positively known.

## N3 final accepted verification (2026-10-24)

| Check | Result |
| --- | --- |
| D1a/D1b/D1c | 74 BIOS, 19 primary UEFI and 19 additional UEFI passing verdicts |
| D1d/D1e | Static ELF/ABI checks; 139 frozen hashes, 131 old ELF comparisons and 55 ISO payload audits pass |
| D1f | 43 layouts pass; cumulative static/reserved growth 45864/57344 B stays within 65536/131072 B |
| D2 | rights-width, symlink, error-copy, incarnation and reserved-inode: 5/5 detected by intended assertions |
| C20 | 15/15 profiles: each Gate B/C/D on BIOS default 2 GiB and UEFI default/max at 1 and 8 GiB |
| Evidence | D1, D2 and C20 sealed; independent archive verification 566/566, 64/64 and 204/204 |
| Integrity | Seven protected references unchanged; prior evidence, failures, 13 worktrees and two stashes preserved |

D1's C2-T held-publication correction fixes an invalid test assumption about
un-notified requests; production and protected stress images remain unchanged.
D1r3 is still unfinished, unsealed and unarchived. Monitoring interruptions and
failed controller/dependency attempts remain documented, not erased.
The [closure record](results/n3-closure.md) contains complete scope, hashes,
reproducibility constraints, storage accounting and authentic screenshots.

## N3.5 accepted verification checkpoint (historical)

The user approved closure on the evidence committed at `0109d9b`; the formal closure
changes documentation only. Historical stage reports and failures remain unchanged.

| Accepted check | Stage 5 result |
|---|---|
| Full BIOS | 74 passing QEMU verdicts |
| Full fresh UEFI plus additional configurations | 19 + 19 = 38 passing runs, fresh VARS; reader/file default and CPU max at 128 MiB, 1 GiB and sparse 8 GiB |
| File / static / mutation checks | 845/0 file assertions per full file run; 302/0 static; all five deliberate mutation controls detected |
| Frozen compatibility | 139 hashes and 131 old userspace binaries unchanged; ABI v0/v1 behavior preserved |
| Reproducibility | Paired production binaries identical; ISO payloads verified separately from timestamps/identifiers |
| Cumulative footprint vs `c5f5acb` | +43808 B static of 65536 B; +57344 B reserved image of 131072 B |

See the [closure record](results/n3.5-closure.md) for the evidence review and accepted
limitations, and [N3.6 preparation](plans/n3-storage.md#11-n36-preparation-planning-only)
for the historical design questions. Final N3/N3.6 acceptance is recorded above.

The subsequent integration audit retained the completed BIOS/UEFI and static
results, completed the three unfinished mutation controls after sparse-copy
harness recovery, and rechecked hashes, production reproducibility and resource
limits. It did not rerun the full regression matrix after the harness repair.
The repair changes test orchestration only; production and ABI sources remain
unchanged from the accepted implementation.

## U1 verification (historical 2026-10-19 checkpoint)

| Check | Result |
|---|---|
| Reproducibility | two clean builds: production `vos.bin` identical; self-test `vos.bin` identical. ISO images differ by design (embedded timestamps and UUIDs) |
| `make test-all` (BIOS: i386 + x86_64; a superset of `make test` and `make ARCH=x86_64 test`) | exit 0: 68 runs passed, 0 failed, 11 fault comparisons, `test-user-elf` 292 passed / 0 failed |
| `make test-uefi` | exit 0: EFI boot path verified (UEFI El Torito entry, ESP type GUID, `BOOTX64.EFI`); 16 runs passed, 0 failed |
| Compatibility hashes | i386 `vos.bin` `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd` and `hello.elf` `c6c6416195a0f18d680d7f8f19be52ce02ff02912ef3bac45324fbe88006cd06`, both unchanged. ABI v0/v1 unchanged: no change under `user/`, `docs/abi/` or `arch/i386/` |
| `git diff --check` | clean |
| Mutations of the U1 security code | 15 mutations, each a valid build in the main test image; 15 caught by a concrete failing assertion or boot panic (one, F1, only after the test added in `fa37ccd`). Details below |

**Mutations.** The 15 mutations, by area:

| Area | Mutations |
|---|---|
| Loader objects | page 0 accepted; legacy hole accepted; kernel pages shared; module/info byte overlap accepted |
| Device window | overflow check removed; RAM accepted; physical width ignored; user-accessible pages; cacheable "UC" pages; executable pages |
| CPU state | EFER.SCE set; CR4.PCE set |
| Framebuffer | channel layout not checked (F1); size limit removed |
| Screen selection | UEFI without a framebuffer falls back to VGA text |

**Not mutation-tested** (no QEMU configuration produces the state):
- MTRR/PAT behaviour on real hardware;
- a loader that hands this kernel no framebuffer on BIOS (the VGA text fallback);
- EFER.SCE inherited set. QEMU firmware never sets it, so the "not cleared" mutation would be
  equivalent; the "set" mutation is caught.

After the F1 fix, the self-test kernel passed again under SeaBIOS and OVMF (bootinfo 183
cases). The production kernel is unaffected by that test-only change.

**The UEFI runs:**
- **Production:** CD at 1 GiB; USB stick at 8 GiB; no serial port; no framebuffer; 640×480;
  1920×1080. Each is judged by the CPU halted right after `kernel_main` returned, plus a PASS
  badge in the screenshot.
- **Test kernel:** at 1 GiB and 8 GiB.
- **Panic banner.**
- **Test images:** suite (memory isolation), ipc, mp, stress, irq, vblk (polling) and ext2.

**NOT APPLICABLE checks** (reported with reasons, never counted as passed):

| Configuration | Check | Reason |
|---|---|---|
| BIOS and UEFI | VGA text write | a framebuffer console is active |
| UEFI | level-triggered IRQ registration | OVMF's ELCR is 0 |
| UEFI | virtio-blk interrupt-mode tests | the driver polls |
| UEFI | ext2 readers interrupted during I/O | polling waits cannot be interrupted |

On BIOS (SeaBIOS: ELCR `0xc00`), virtio-blk interrupt mode is fully tested: 149 cases, 0 failed.

## Known defects and limitations

- **Hardware:** QEMU only. Untested on any physical machine.
- **Directory progress:** the 512-unit work limit can reject deep single-block
  chains and scans before block 344 in the sparse `/scope/huge` fixture; EAGAIN
  carries no progress cookie. Gate D records exact boundaries; resumable progress
  remains future work.
- **Kernel stack:** full dynamic kernel-stack high-water proof is incomplete.
  Compiler frame reports and runtime guard/canary tests do not constitute that proof.
- **Filesystem trust:** read-only; trusted immutable provisioned directory graphs.
  Namespace/parser validation does not authenticate malicious media or inode/block aliases.
- **Storage lifetime/timing:** finite work and poll bounds do not promise real-time latency
  or fairness. Unconfirmed device reset may retain bounded DMA quarantine; physical
  reset-refusal/hot-unplug reclamation is untested.
- **Secure Boot:** not supported. GRUB's lockdown refuses unsigned Multiboot2 kernels.
- **UEFI interrupts:** no IOAPIC, MSI or MSI-X. INTx under OVMF is edge only, so virtio-blk
  polls and its waits cannot be interrupted.
- **Memory:**
  - RAM above 1 GiB is ignored.
  - Loader objects above 1 GiB are refused (not observed).
  - The device window holds at most 8 mappings and 64 MiB.
- **Framebuffer:**
  - Only 32-bpp BGRX is accepted; other formats give serial only.
  - The console's write-combining is unverified on hardware (QEMU ignores memory types).
- **VGA text fallback:** only unit-tested. GRUB always hands this kernel a framebuffer, because
  the kernel requests one.
- **BIOS display change:** BIOS boots now switch to VBE graphics.
- **Input:** PS/2 polling, buffered line input and an explicitly granted ABI v3
  native text console, with a US layout and 128-byte limit; no USB keyboard or mouse driver.
  Editing appends/deletes at the end; an overlong submission is discarded in full.
  Boot selects `vos.console=1` for the buffered demo or `vos.vsh=1` for the
  native shell; normal production boot still halts.
- **Shell:** first native Rust `vsh` only; no quoting, pipes, history or external-program launch.
- **Missing features:** writable storage, networking, SMP and ACPI power management.

## Unsupported (by design, for now)

- Physical-disk installation.
- `grub-install` / `efibootmgr` use.
- Writes of any kind to storage.
- Secure Boot.
- Hardware other than QEMU's emulated PC.
