# Dell OptiPlex 9020 USB survey v2.1: physical evidence record (USB.3-A)

**Genuine physical-hardware evidence from a photograph; not QEMU, not an independent register measurement.** Historical QEMU results are not touched or reinterpreted.

## Identity
| Item | Value |
| --- | --- |
| Original photograph (vault, unmodified) | owner-held evidence vault, file `dell-optiplex-9020-usb-survey-v21-2026-10-31.jpg` (not in this repository) |
| Photograph SHA-256 / size / dimensions | `4756f838b29ae1d77e2954d65a0080f0f478f03d0e5d6741532124f61bd78040`, 545,222 B, 2560x1920 (hash re-verified 2026-10-31 after the review) |
| Image booted | `vos-usb-survey-dual.iso`, SHA-256 `04dad6e50a5c59cc661ddcb71c070a12d26f2c7ce6f808ad7b0b4ee86236195e` (13,721,600 B) |
| Kernel inside | `vos.bin` SHA-256 `069df35a71b99fcc7fa7f13aa417ae494d73bea40d89e1f7daeff91bda1b8eab` (1,244,272 B) |
| Source commit | `602a1dc7f784e4017d9dc2f0624889f700a9c5a2` (main after PR #43) |
| Menu entry used | survey entry, `vos.usb-survey=hold` (reported by the owner; the photograph shows survey output and the halted line, not the GRUB menu) |
| Machine | Dell OptiPlex 9020, Legacy BIOS boot from a USB stick (owner-stated) |
| Transcription | 53 `usb:` lines, [transcription](dell-survey-v21-transcription.md); values marked "read once" were read from the photograph one time |

## Observed (printed by the survey and legible in the photograph)
- xHCI `8086:8c31` at 00:14.0: 21 ports, 0 connected, slots 32, scratchpads 16, USBCMD `0x00000005` (R/S 1, HCH 0), BIOS-owned 1 / OS-owned 0, SMI register `0x40002001`, every PORTSC `0x000002a0`.
- Routing registers (PCI config 0xd0-0xdc): XUSB2PR `0x00000000`, XUSB2PRM `0x00000000`, USB3_PSSEN `0x00000000`, USB3PRM `0x0000003f`.
- EHCI `8086:8c2d` at 00:1a.0: ports 2, companions 0 x 0, USBCMD `0x00000011`, USBSTS `0x0000600a`, CONFIGFLAG 1, PORTSC p1 `0x00001005` p2 `0x00001000`, legacy capability at `0x68`, BIOS-owned 1 / OS-owned 0, USBLEGCTLSTS `0x000a2005` (**read once**).
- EHCI `8086:8c26` at 00:1d.0: the same, with USBSTS `0x00006008` and USBLEGCTLSTS `0x00002005` (**read once**).
- DMAR present, host width 39, 2 remapping units (unit 0 `0xfed90000` flags 0x00 does not cover the xHCI; unit 1 `0xfed91000` flags 0x01 covers all devices), both status `0x00000000` translation off, 2 RMRRs (one names the xHCI), `DMA isolation NOT ESTABLISHED`.
- i8042 status `0x1c`.

## Inferred (hypotheses, not observed)
- The enabled high-speed device on port 1 of each EHCI is probably an internal hub (the PCH rate-matching hub); not established, and the photograph does not show what is behind it.
- The BIOS is polling a periodic schedule on both EHCIs (USBCMD periodic-schedule-enable plus USBSTS periodic-status set); not proven which devices.
- Which EHCI the keyboard and mouse use is not shown (the earlier Windows Device Manager evidence names 8C2D, separately labeled).

## Not observed (the survey did not print these; models must treat them as unknown)
EHCI CAPLENGTH and HCSPARAMS/HCCPARAMS raw values, raw USBLEGSUP word, USBINTR, FRINDEX, ASYNCLISTADDR, PERIODICLISTBASE, CTRLDSSEGMENT, which device the second RMRR names and whether either RMRR names the EHCIs, the xHCI interrupter/port contents beyond the printed lines.

## Authoritative routing rule (owner-supplied)
Intel 8 Series/C220 datasheet section 16.1.37: zero bits in XUSB2PRM prohibit the OS from modifying the corresponding USB 2.0 routing selector bits. XUSB2PRM is `0x00000000`, so no XUSB2PR write is proposed. I have not independently re-read the datasheet.

## Not covered
No BIOS USB configuration photographs and no Windows device topology details have been supplied since the survey; if they arrive they will be added as separately labeled evidence. No BIOS setting, firmware, controller register or media was changed for this record.
