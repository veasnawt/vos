# USB.1 evidence index

See the [technical report and reproduction commands](../usb1-xhci.md).
All serial files are genuine guest output; none claims physical-hardware success.
Raw logs retain carriage returns and prompt spaces byte-for-byte. Source/document
whitespace checks exclude those logs; their integrity is checked against the
original captures and recorded SHA-256 values instead.

- `execution.json`: successful host checks, eight initial native cases, source
  identities and read-only GRUB producer hashes.
- `executed-eight-case-verifier.py.txt`: exact verifier matching that execution's
  script digest. The current source adds two GUI negative controls, their
  assertions, and the separately passed bootstrap regression; production inputs
  are unchanged.
- `gui-negative.json`: final-verifier identity and both additional GUI
  PS/2-disabled cases. These intentionally report guest startup failures.
- `*-host.log`: xHCI, keyboard, diagnostic and console sanitizer results, plus
  the focused GUI bootstrap regression. Bootstrap worker/scheduler boundaries
  are modeled; do not interpret its 523 assertions as native injection results.
- `bios-*.serial.log`, `uefi-*.serial.log`: five cases per firmware: option-off
  control, USB controller/keyboard attached, controller absent, text-only without
  PS/2, GUI then text without PS/2. Companion command/result records identify
  the exact configuration and expected negative outcomes.
- `*-usb.pci.json`: independently queried QMP device IDs, BAR addresses and
  aperture sizes, cross-checked against guest diagnostics in `inputs.json`.
- `inputs.json`: versions and SHA-256/lengths of the kernel, applications and
  all six retained images. Large generated images and objects are not committed.
- `failed-console-invocation.log`: first console harness invocation incorrectly
  linked a standalone test with duplicate production definitions.
- `failed-diagnostics-link.log`: missing hosted I/O stubs in the new diagnostic
  fixture, subsequently supplied without altering production I/O behavior.
- `failed-uefi-prerequisite.log`: missing system GRUB EFI modules; recovered with
  the existing supported explicit producer override, without installation.
- `failed-negative-catalog.serial.log`: rejected four-module text-only fixture;
  corrected to the existing three-module contract, without changing the catalog.
- `changed-files.txt`: complete commit inventory.
- `manifest.json`: sizes and SHA-256 hashes of every candidate file except the
  manifest itself, which is identified by Git. This is integrity evidence, not
  an independent authorization or signature.
- `review.json`: focused final consistency checks and original-workspace facts.

Original logs, failed roots and generated inputs remain under this checkout's
`build/`. The interrupted D1 checkout and its evidence are not build inputs or
output targets. The prior maintainer-controller policy was withdrawn by the
owner; no legacy admission is claimed and no D1 funding or workflow resumed.
