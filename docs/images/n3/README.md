# Authentic N3 VOS screenshots

These three 1280×800 PNG files are copied byte-for-byte from accepted D1 evidence.
They were visually inspected before publication. No new VM boot, crop, overlay,
retouch or generated UI was used for this milestone.

| File | Demonstrated behavior | SHA-256 |
| --- | --- | --- |
| [BIOS boot/status](vos-n3-bios-boot-status.png) | Actual framebuffer console, ring-3 hello, reclaimed frames and final PASS; no attached block device in this production boot | `a1883dfca291522ebdc4be15431d5d9f20b1ae1fc783f94d632c53335f028cc6` |
| [UEFI boot/status](vos-n3-uefi-boot-status.png) | Actual GOP framebuffer boot diagnostics and PASS; no interactive prompt | `c4a7f4b66b377255699ed99f5659ea942d9257721b6969f5b000a0537ce62a2c` |
| [UEFI deliberate panic](vos-n3-uefi-deliberate-panic.png) | Expected test-only `test case: deliberate panic` banner; not a normal boot failure | `eeec541187c38b3be238af02dd6db90d2a35559e1864fc31f15cfd6d184f371d` |

Source root: `n36-final-d1seal-codex-20261024T034457Z`.
Independent archive SHA-256:
`5dcc1b19d09a7a006b5cf2e216c7c43af68f75f40091c4ba225893499284d7f6`.

Exact paths within that root/archive prefix:

- `preserved-d1ef/inherited-d1r3/ws/build/x86_64/bios-screen.png`
- `preserved-d1ef/inherited-d1r3/ws/build/x86_64/uefi-boot.png`
- `preserved-d1ef/inherited-d1r3/ws/build/x86_64-test-panic/uefi-panic-screen.png`

The originals were captured during completed D1r3 display runs. D1r3 itself
remains unfinished, unsealed and unarchived. The accepted finalization root
contains hash-verified copies, including the original PPM captures and these PNGs.
The published files are the existing PNG bytes, without another conversion.

VOS currently has a boot/status/panic console, not keyboard input, a shell or a
native desktop. See the [N3 closure record](../../results/n3-closure.md).
