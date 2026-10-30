# Physical hardware evidence: Dell OptiPlex 9020, first USB survey (2026-10-30)

Three photographs of the Dell's monitor, taken by the owner, are preserved unmodified in `photos/` (originals, hashes in
`original-sha256.txt`, copied from `/home/veasnawt/vos-physical-evidence/` and re-verified after copying). They have no EXIF data and
the file names (`photo_YYYY-MM-DD_HH-MM-SS.jpg`) look like messenger-export times, so the **capture order and exact time are not
established**. They are physical hardware evidence of what the screen showed; they are not reproducible logs.

| Photo | SHA-256 | Size | Shows |
| --- | --- | --- | --- |
| `photo_2026-10-30_13-50-58.jpg` | `579c7d746a92eaa2171c99f49e4ce5ea51fc78254161c1459ff49c6cb8c4e7c8` | 404,348 B, 2560x1920 | the USB survey result screen (halted) |
| `photo_2026-10-30_13-51-28.jpg` | `cb0b223c9f86fb9a30fbc0dee5519b3d128a431a3234248743fb21fbb6deaffb` | 93,019 B, 1280x960 | the GRUB menu of the survey image, countdown at 27 s |
| `photo_2026-10-30_13-55-09.jpg` | `59868589d3a71b23457bee07850c1ed2b48f96c2bd006a513c1876eee62fa03d` | 522,058 B, 2560x1920 | a normal VOS boot (default entry) completing on the Dell |

## Association

- **Survey ISO tested:** `vos-usb-survey-dual.iso`, 13,686,784 B, SHA-256
  `736e5fdeb8b6b32c28374d6dee5b5be4bedef18d39cbede05517e69e0a887535` (the owner states this is the image used; a photograph cannot show a hash).
- **Build:** `6d6a941` (merge of PR #36, tree identical to reviewed commit `37b3aeb`). **Evidence and image-recipe commit:** `88c64fb`
  (merged as `496cc29`, PR #37). The GRUB menu photo shows `build 6d6a941` on both entries.
- **Survey photo** is tied to that ISO by its content: the line layout, wording and the final line
  `usb: survey complete. Nothing was written. The machine is halted: power it off.` are exactly what `vos.usb-survey=hold` of that kernel prints.
- **GRUB menu photo:** `GNU GRUB version 2.14`, entry 0 `VOS normal boot (default)  [survey diagnostic image, build 6d6a941]`, entry 1
  `>>> VOS USB SURVEY: read-only, stops on screen  [build 6d6a941] <<<`, "The highlighted entry will be executed automatically in 27s":
  the menu of `grub-physical-survey.cfg` as built (30 s timeout). It shows the menu before a choice; it does not show which entry was taken.
- **Normal boot photo, kept with its own provenance:** the image or boot configuration cannot be confirmed from the photo. What it shows is
  consistent with the default entry of the tested dual ISO: `boot: kernel command line ""`, `boot: modules 1` (`hello`), loader
  `GRUB 2.14-2ubuntu2.1`, `efi: no EFI map (BIOS): e820 protection only`, and kernel layout values that equal the final tested kernel's
  QEMU log exactly: kernel image `0x100000-0x1e2000`, text `0xffffffff80107000-0xffffffff80143000`, rodata `...80143000-...80150000`,
  `PDPT[510] 0x105000 -> 0x177000`, `fn=0xffffffff8013c900 seed=0xffffffffb6c71022 salt=0xffffffff800578e0`. A different kernel would not
  give these addresses. It is evidence that this build's default entry boots to `boot: checks passed` on the Dell; the machine-specific values
  (memory map 25 entries, 8,493,506,432 usable bytes, 7,420,297,216 bytes above 1 GiB ignored, framebuffer 1920x1080 BGRX at 0xe0000000, pitch 7680)
  are read from the photo and are not reproducible.

## Survey photo, line by line (read from the photo; nothing guessed)

Every line is legible. Where a value is not cross-checked by another printed value it is marked (R) for "read, plausible, not cross-checked".

| Printed on the Dell | Value |
| --- | --- |
| survey line | PCI functions 15, buses 3, bridges 2, usb 3, iommu-fn 0 |
| xHCI | `00:14.0` `0x8086:0x8c31` class `0x0c0330` rev `0x04` subsystem `0x05a41028` cmd `0x0006` |
| EHCI | `00:1a.0` `0x8086:0x8c2d` and `00:1d.0` `0x8086:0x8c26`, both class `0x0c0320` rev `0x04`, subsystem `0x05a41028`, cmd `0x0006` |
| BAR / PCI | bar0 `0xf7c20000` (R), irq 11/pin1, power state D0, msi 0, msix 0, 2 PCI capabilities (no PCIe capability: no Device Control line) |
| hci line | version `0x0100`, caplen `0x80`, slots 32, interrupters 8, **ports 21**, **scratchpads 16**, ac64 1, csz 0, ppc 0 |
| hcs1 / hcs2 / hcs3 / hcc1 / hcc2 | `0x15000820` / `0x84000054` / `0x0200000a` / `0x200077c1` (R) / `0x00000000` |
| usbcmd / usbsts | `0x00000005` (R/S 1, bit 2 = interrupter enable set) / `0x00000000` (HCH 0, HSE 0, HCE 0), pagesize `0x0001`, config `0x0020` |
| ports | **ports read 21, connected 0** |
| extended capabilities | 5, ids `2 2 193 192 1`; legacy support at `0x846c` (R): **bios-owned 1, os-owned 0**, USBLEGCTLSTS (smi) `0x40002001` (R) |
| protocols | usb2.00 ports 1+15, usb3.00 ports 16+6 |
| i8042 | status `0x1c`, output buffer 0, input buffer 0 |
| IOMMU line | 0 PCI IOMMU functions; ACPI DMAR and IVRS NOT read, so IOMMU UNKNOWN |
| summary | `xhci 00:14.0 8086:8c31 v0100 ports 21 connected 0 slots 32 spad 16`; `bios-owned 1 os-owned 0 running 1 halted 0` |
| end | `usb: survey complete. Nothing was written. The machine is halted: power it off.` |

Cross-checks that make the decoded values trustworthy: `hcs1 0x15000820` decodes to 21 ports, 8 interrupters, 32 slots (as printed);
`hcs2 0x84000054` decodes to a scratchpad count of 16 (Hi 0, Lo 0x10; as printed); `hcc1 0x200077c1` gives AC64 1, CSZ 0, PPC 0 (as printed)
and an extended-capability start of `0x2000 << 2 = 0x8000`; `config 0x0020` equals the 32 slots; the usbsts `0x0` agrees with "running 1 halted 0".

## Differences from the first transcription the owner gave in chat
The photo shows **0 connected ports** (the chat text said 8) and **16 scratchpad buffers** (the chat text said 32; 32 is the slot count). Everything
else in that transcription (21 ports, 32 slots, 32-byte contexts, running, BIOS-owned and OS not claimed, two EHCI, IOMMU unknown, halted) matches the photo.
The photo is the evidence; the earlier text is superseded.

## What the photo means for the next steps
- No device is connected to any xHCI port: the keyboard and mouse are on the EHCI controllers (or on a routing state the survey did not print).
  That makes the Intel port routing registers (PCI configuration 0xD0-0xDC) the key unknown, and means no already-connected device exists on this controller today.
- 16 buffers are well inside the 64-buffer budget. 21 ports still exceed the model's current limit of 16.
- The controller is running, BIOS-owned with SMI enabled (`0x40002001`), exactly the state the handoff sequence is for.
- ACPI DMAR/IVRS are still unread: IOMMU, translation and DMA isolation are all unknown.

## Missing evidence
- A hash-verified statement of which ISO file was on the USB drive (the owner states `vos-usb-survey-dual.iso`); a photo cannot prove it.
- Whether the survey photo was taken on the same boot as the GRUB menu photo, and which entry was chosen for the normal-boot photo (order and times unknown).
- The Intel routing registers, the EHCI port states, the xHCI ring pointers and the DMAR tables (the planned survey v2).
- Windows' own view: Device Manager "devices by connection" for the keyboard and mouse, the BIOS xHCI-mode and VT-d settings, and whether the Dell has a PS/2 port.
