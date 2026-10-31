# Dell OptiPlex 9020 USB survey v2.1: transcription and read-only assessment

**Genuine physical-hardware evidence, kept separate from QEMU evidence.** Source: one owner photograph of the final diagnostic screen,
owner-held evidence vault, file `dell-optiplex-9020-usb-survey-v21-2026-10-31.jpg` (not in this repository)
(545,222 B, 2560x1920, SHA-256 `4756f838b29ae1d77e2954d65a0080f0f478f03d0e5d6741532124f61bd78040`). Image booted: the dual survey ISO built from
main `602a1dc7f784e4017d9dc2f0624889f700a9c5a2`. The whole survey (53 `usb:` lines) fits on one screen, so nothing scrolled away.
This is a transcription of a photograph, not a register measurement reproduced by a second tool. I had no image-crop tool, so I read the
2000-px display of the photo; where a value is not independently confirmed by another line it is marked **(read once)**.

## Transcription (line by line)

Legible and cross-checked unless marked.

| # | Line (as read) | Notes |
| --- | --- | --- |
| 1 | `survey (read-only) pci functions 15 buses 3 bridges 2 usb 3 iommu-fn 0` | |
| 2 | `xhci 00:14.0 0x8086:0x8c31 cls 0x0c0330 rev 0x04 sub 0x05a41028 cmd 0x0006` | |
| 3 | `ehci 00:1a.0 0x8086:0x8c2d cls 0x0c0320 rev 0x04 sub 0x05a41028 cmd 0x0006` | |
| 4 | `ehci 00:1d.0 0x8086:0x8c26 cls 0x0c0320 rev 0x04 sub 0x05a41028 cmd 0x0006` | |
| 5 | `00:14.0 bar0 0xf7c20000 irq 11/pin1 pm D0 msi 0 msix 0 caps 2` | bar0 equals the v1 survey |
| 6-7 | `intel routing (pci cfg 0xd0-0xdc, read only): xusb2pr 0x00000000 xusb2prm 0x00000000` / `usb3_pssen 0x00000000 usb3prm 0x0000003f` | repeated identically in the SUMMARY line `routing raw` |
| 8-10 | `usb2 ports routable -: to xhci -, left with ehci -` / `superspeed ports enabled -` / `superspeed ports disabled 1-6` | derived from the four registers |
| 11 | `00:14.0 hci 0x0100 caplen 0x80 slots 32 intr 8 ports 21 scratchpads 16 ac64 1 csz 0 ppc 0` | consistent with hcs1/hcs2 below |
| 12 | `hcs1 0x15000820 hcs2 0x84000054 hcs3 0x0200000a hcc1 0x200077c1 hcc2 0x00000000` | hcs1 and hcs2 decode to 21 ports, 32 slots, 8 interrupters, 16 scratchpads; **hcs3 and hcc2 (read once)** |
| 13 | `usbcmd 0x00000005 usbsts 0x00000000 (R/S 1 HCH 0 HSE 0 HCE 0) pagesize 0x0001 config 0x0020` | |
| 14-16 | `ports read 21 connected 0` / `ext caps 5 ids 2 2 193 192 1 legacy@0x846c bios 1 os 0 smi 0x40002001` / `protocols usb2.00 ports 1+15 usb3.00 ports 16+6` | |
| 17-18 | `portsc of 21 of 21 ports: connected 0 enabled 0 powered 21 over-current 0`; p1..p21 all `000002a0` | same word 21 times |
| 19-20 | `dcbaap 0xdbe24000 [reserved] crcr(lo) 0x00000000 ir0 erstba 0xdbe25200 [reserved] erdp 0xdbe26400 [reserved]`; `ir0 iman 0x00000002 erstsz 0x00000001` | addresses **(read once)** |
| 21-23 | `ehci 00:1a.0 8086:8c2d hci 0x0100 ports 2 companions 0 x 0 cmd 0x00000011 sts 0x0000600a halted 0 configflag 1`; `p1=00001005 p2=00001000`; `connected 1 companion-owned 0 legacy@0x68 bios 1 os 0 smi 0x000a2005` | the SUMMARY lines repeat cmd, sts, ports and portsc identically; **smi value (read once, differs between the two EHCIs)** |
| 24 | `ehci root port 1 connected hs enabled 1 owner ehci (a hub behind it? devices below it are not visible)` | |
| 25-28 | `ehci 00:1d.0 8086:8c26 ... cmd 0x00000011 sts 0x00006008 halted 0 configflag 1`; `p1=00001005 p2=00001000`; `legacy@0x68 bios 1 os 0 smi 0x00002005`; same root-port line | **smi value (read once)** |
| 29-31 | `acpi rsdp revision 2 rsdt 0xd8fee028 xsdt 0xd8fee088 root xsdt tables 14`; signatures `FACP APIC FPDT SLIC LPIT SSDT SSDT SSDT HPET SSDT MCFG SSDT`, `ASF! DMAR` | |
| 32-36 | `dmar host-width 39 flags 0x01 units 2 rmrr 2 (an RMRR names the xhci: firmware DMA)`; unit 0 base `0xfed90000` flags `0x00` covers xhci: no, `registers ver 0x10 status 0x00000000 translation off`; unit 1 base `0xfed91000` flags `0x01` covers xhci: yes (all devices), same registers | |
| 37-39 | `dma-protection facts: table present yes, hardware answers yes`; `translation enabled no, xhci translated no`; `DMA isolation NOT ESTABLISHED: translation state is not a proof (translation tables are not read)` | |
| 40-41 | `i8042 status 0x1c (output buffer 0, input buffer 0)`; `IOMMU: 0 PCI IOMMU function(s) of class 08/06` | |
| 42-53 | SUMMARY block: xhci line, `usb2 routing: to xhci - left with ehci -`, `ehci connected root ports: 8c2d=1 8c26=1`, `routing raw: xusb2pr 0x00000000 xusb2prm 0x00000000 pssen 0x00000000 usb3prm 0x0000003f`, two EHCI status lines and two portsc lines, `dma: table yes hw yes on no xhci no`, `DMA isolation NOT ESTABLISHED`, `survey complete. Nothing was written. The machine is halted: power it off.` | the v2.1 lines display as designed, with `DMA isolation NOT ESTABLISHED` last |

Agreement with the owner's summary: every finding in the owner's list matches the photograph. Two details the owner's list did not state and I read from the screen:
EHCI `companions 0 x 0` (the EHCIs declare no companion controllers) and the two xHCI USB3 facts (`usb3_pssen` 0, `usb3prm` 0x3f).

Cross-checks on the legibility: hcs1/hcs2 decode to the printed ports, slots, interrupters and scratchpads; USBCMD/USBSTS agree on the EHCI schedule bits;
the xHCI line values equal the v1 survey (21 ports, 0 connected, 16 scratchpads, bar0 `0xf7c20000`, SMI `0x40002001`). The first survey v1 values were
the same, so the xHCI findings reproduce across two boots (still transcribed from photographs).

Not observable from the photograph (do not infer): the `0001306`-like counter at the top right, anything the survey did not print.

## What the values mean (and what they do not)

**Routing masks.** XUSB2PR is 0: no USB 2.0 port is routed to the xHCI. XUSB2PRM is 0: the mask of ports that *may* be routed is empty. Per the owner (Intel 8 Series/C220 datasheet section 16.1.37, not independently re-read by me), zero bits in XUSB2PRM prohibit the OS from modifying the corresponding USB 2.0 routing selector bits, so no XUSB2PR write is proposed for this firmware configuration. (My earlier text cited Linux behavior from memory; the datasheet statement supersedes it.) The survey's own register-layout label "datasheet layout, unverified here" still applies to the derived port lists. It does **not** show whether a BIOS setting would make the mask non-zero or remove the need for hub support. USB3_PSSEN 0 / USB3PRM 0x3f: six SuperSpeed ports exist and none is enabled by the firmware. All 21 xHCI
ports read `0x000002a0` (powered, link state RxDetect, nothing connected), consistent with every USB 2.0 port staying with the EHCIs.

**EHCI port 1 on both controllers.** Each EHCI has two root ports and declares no companion controllers. Port 1 of each reads `0x00001005`: connected, enabled,
powered, high speed, owned by the EHCI; port 2 is powered and empty. A *high-speed* connected, enabled device on each EHCI is what an internal USB 2.0 hub looks like
(the Intel PCH rate-matching hub is the usual explanation), and a low- or full-speed device cannot be a direct EHCI root-port device when there are no companions. This is
a hypothesis, not an established fact. It does **not** show which EHCI the keyboard or mouse use (Windows earlier put both under 8C2D), nor how many devices sit below either hub, nor that
the two connections are the keyboard and mouse. The boot USB stick is also a high-speed device and sits behind one of them.

**Firmware ownership.** The xHCI is running (`R/S 1`, interrupts enabled, `HCH 0`) and BIOS-owned with SMI enabled; its command, event-ring and device-context pointers
are in firmware-reserved memory. Both EHCIs are BIOS-owned and running: USBCMD `0x11` is Run plus Periodic Schedule Enable, and USBSTS `0x600a`/`0x6008` has the periodic
schedule status set and the asynchronous schedule off. That shows the BIOS is actively polling a periodic schedule on both EHCIs (typical of BIOS keyboard/mouse emulation), and its SMI enables (low bits `0x2005`) mean
SMM code can run on USB events. A future driver must therefore take ownership first: set the OS-owned bit and wait for BIOS ownership to clear, disable the SMI enables, halt (clear Run, wait for halted) and only then program its own structures.
All of those are PCI configuration or register writes that have never been executed on real hardware here, and none is authorized.

**IOMMU.** DMAR is present with two remapping units; unit 1 covers all PCI devices (including the xHCI), unit 0 does not; both read status 0 and translation off. Two RMRRs exist and
the survey reports that one names the xHCI (firmware DMA); which devices the other RMRR names, and whether either names the EHCIs, is not printed. With translation off, controller DMA is unrestricted:
a driver gets no isolation, only whatever discipline it applies itself (kernel-owned bounded DMA pool, never passing other memory). `DMA isolation NOT ESTABLISHED` stays true. Enabling VT-d translation
would be a separate large project that would have to preserve the RMRR identity mappings.

## Comparison with the existing models and QEMU evidence

- The hosted xHCI models (C4.1, C4.2a) and every accepted QEMU result are xHCI-only, QEMU-profile, with controllers that are not BIOS-owned or running, no attached devices at init, no EHCI, no hub, no DMAR. The earlier recorded blockers still stand: only the QEMU profile qualifies, attached devices are refused, identity-DMA/IOMMU unknown refuses (not re-run now).
- This Dell xHCI is BIOS-owned and running with firmware rings, has 16 scratchpads and 21 ports (the port bound was raised to 31 in survey v2) and has **no connected port**, so xHCI model work alone would not produce input on this machine.
- There is no EHCI model, hub-class model, split-transaction model or HID model anywhere in the repository.
- The physical facts here (EHCI schedules already running, SMIs enabled, routing mask zero) have no QEMU counterpart: QEMU's EHCI and xHCI are not firmware-owned and QEMU has no Intel PCH routing. QEMU still correctly exercised the survey rendering (10/10).

## Paths toward vsh keyboard and GUI mouse

1. **BIOS setting that exposes xHCI routing.** The cheapest investigation is not code: the owner can *look* at (and photograph) the Dell BIOS Setup USB/xHCI options without changing anything. If the BIOS offered a mode that sets a non-zero XUSB2PRM, the devices would become direct xHCI root-port devices and the existing xHCI work would apply, with no hub or split transactions for them. That is unknown: the survey proves nothing about such a setting, and changing it is not authorized.
2. **Native EHCI + hub + HID**, which is credible on the machine as it boots today. It needs, in order: a hosted EHCI/hub/HID model; BIOS handoff, halt and reset on real hardware; asynchronous and periodic schedules in kernel-owned DMA memory; hub class enumeration of the rate-matching hub; split interrupt transactions for low/full-speed devices behind the hub's transaction translator; HID boot keyboard and mouse; then the existing input seam (keyboard to vsh, pointer to the GUI session). Keyboard first, mouse second.
3. **Existing PS/2 path with BIOS emulation.** The i8042 status `0x1c` shows something answers port 0x64, but the earlier correction stands: status polling cannot show BIOS USB emulation or tell a keyboard from a mouse. The owner's earlier vsh and GUI failures on this machine are consistent with the emulation not delivering input to the kernel, but no diagnostic trace was captured. A diagnostic boot with the existing input entry would settle it; it needs its own approval and a new image.

**Recommendation.** Investigate (1) first because it costs nothing in code and would remove the largest piece of work, and in parallel start the hosted EHCI/hub work from (2), which is the only path that works regardless of the firmware setting. Do not choose a routing write on the strength of the registers alone.

## Smallest next safe implementation increment

**USB.3-A: hosted EHCI qualification and BIOS-handoff model seeded from the Dell survey.** Hosted only, no kernel input, no hardware, no write executed anywhere.

Acceptance criteria:
1. A `no_std`, `forbid(unsafe)` Rust crate under `kernel/rust/usb/` that no kernel build input references (the C4.1 pattern), with typed operations for: qualification of an EHCI from survey facts (ports, companions, BIOS-owned bit, schedules running, configflag), the ownership-handoff protocol (OS-owned set, wait for BIOS clear with a bounded timeout, SMI-disable), halt (clear run, wait for halted) and controller reset. It programs no schedule, no list address and no port.
2. Fixtures that reproduce the photographed registers of both EHCIs exactly, plus refusal fixtures (companions present, unknown schedule bases, translation state unknown, BIOS never releasing ownership, controller never halting).
3. An independent Python oracle replaying at least 200 seeded traces with zero mismatches, and mutation controls for each decision in the model (bounded to the decisions the model contains, not an open-ended campaign), all killed.
4. Compile-fail controls showing no operation can issue a write outside the typed set, as in C4.1.
5. Existing CI unchanged; the gate runs in the existing hosted steps; no new CI job.
6. Storage admission and projections stated before work starts; no physical media, boot or hardware action.

Optional parallel read-only increment, only if the model needs it: survey v2.2 adding the EHCI schedule registers (ASYNCLISTADDR, PERIODICLISTBASE, FRINDEX), HCCPARAMS raw, USBLEGCTLSTS raw and the RMRR device scopes. It would need another physical run and separate approval.

## Limits of this review

Read-only. No VOS source, firmware, controller or media was touched, no new physical test run, no storage guard changed. The transcription is of one photograph; values marked "read once" should be confirmed from the original if they matter. Interpretations (rate-matching hub, BIOS polling, routing-mask meaning) are hypotheses, not established facts.
