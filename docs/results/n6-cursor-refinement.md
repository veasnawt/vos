# N6.3 native cursor visibility refinement

Local refinement of accepted merge `78cc80c9ac2b8b9d36ffb411ab06f5a63811bbc3`
on `feat/n6-cursor-refinement`. This is cursor polish, not N6.4 or a complete
desktop. Publication, PR creation and merge are outside this authorization.

## Change and resource bounds

| Property | Accepted N6.3 | Refined cursor |
| --- | --- | --- |
| Backing canvas | 8x12 | 32x32 |
| Visible arrow | 6x11 | 21x28 |
| Underlay | 384 bytes | 4,096 bytes |
| Cursor state including underlay | 400 bytes | 4,112 bytes |
| Hotspot | Exact tip | Exact tip (0,0) |

The fixed state grows by 3,712 bytes. Two constant row masks occupy 256 bytes.
There is no dynamic allocation, dependency, glow or host-rendered artwork.
The arrow uses white fill and a dark `0x101a2a` outline. Mask geometry follows
the polygon (0,0), (0,23), (6,18), (11,27), (15,25), (11,17), (20,17).

Only the cursor block of `arch/x86_64/screen.c` changes kernel behavior.
Movement decoding, coordinate limits, button semantics, event ABI, authority,
keyboard input and process lifecycle remain identical. Damage intersection
uses the expanded canvas; clipped underlay save/restore and stride handling
retain the supervisor framebuffer boundary. The pointer ABI's resource paragraph
is updated without changing its structures or syscall semantics.

## Verification

The [machine result](n6-cursor-refinement/result.json),
[evidence index](n6-cursor-refinement/evidence.json) and
[host output](n6-cursor-refinement/host.txt) record the following:

- Fresh ASan/UBSan renderer: **2,179,862 assertions PASS**. The 562,325 base
  renderer assertions printed first are included in that total, not additional.
  Independent polygon expectations cover every arrow/outline pixel, transparent
  canvas, exact tip, stride/canaries, restoration and intersecting redraw.
- Reused by source identity: 987,802 service and 49,499 legacy-console assertions,
  three Rust tests and 16 logger cases. The non-renderer hosted test branch is
  unchanged after preprocessing. Fresh plus reused C assertions total 3,217,163;
  this is not a claim that the entire total was rerun.
- Normal kernel relink, layout and Multiboot checks PASS. All five existing
  application ELF identities remain unchanged.
- BIOS and UEFI: movement, primary press/release and Rust click response PASS.
  Fifteen captured frames per firmware pass independent geometry/pixel checks.
  Exact corners remain (0,0), (1023,0), (1023,767), (0,767). Mid-edge and partial
  bottom-right clipping checks also pass, including (1013,763).
- Both paths exercise a/b/q, held-button exit, cancellation/input cleanup,
  kernel-CR3 reclamation, restored resource counters, vsh, apps, hello, Rixie,
  echo and pwd. See [BIOS serial](n6-cursor-refinement/bios.serial.txt) and
  [UEFI serial](n6-cursor-refinement/uefi.serial.txt).
- Fourteen strict reference-alias acceptance/rejection cases PASS. Seven graft
  producer tests PASS, including actual ISO payload mismatch rejection.

Authentic final [BIOS](../images/n6/vos-cursor-refined-bios.png) and
[UEFI](../images/n6/vos-cursor-refined-uefi.png) QMP screenshots show the enlarged
arrow over the native Rust panel during a verified held-button interaction.
The earlier [BIOS cursor](../images/n6/vos-pointer-bios.png) is retained for
comparison. Intermediate movement, click and clipping captures remain in the
raw execution directory; no image was synthesized or retouched.

## Image production and reproducibility

The focused driver is `scripts/test-cursor-refinement.py`. It compiles only
the changed renderer object and links the existing hash-verified normal kernel
objects. It reuses unchanged application binaries and the read-only filesystem
fixture. Exact source hashes, tool commands, object inputs, image hashes, QMP
input records and screenshot identities are in the result and raw manifest.

UEFI uses the accepted stock GRUB producer with xorriso graft points for the
kernel, configuration and five applications. It avoids duplicate payload staging;
the private GRUB boot inputs and original partition/boot geometry are retained.
Strict payload comparison, GPT primary/backup CRC, non-overlap, ESP and EFI PE
validation pass. BIOS uses the accepted BIOS-only seed replay with fresh output.
No historical image is overwritten. This is a focused producer improvement,
not a replacement image-integrity policy.

| Output | SHA-256 |
| --- | --- |
| Normal kernel | `c2d172c4575f584c68eecf44a2ca79ad9c9d4046d859c1b4453323e007389eda` |
| BIOS ISO | `6f07266c40f58b9d593bd7e297945fa7241dbfa9b1f6a2d7e0d3c163880e376e` |
| UEFI ISO | `8f2fbe15b604cb21681f1a23f4a399380caeb4dca540fca381ba3fc2dfb80869` |
| Runtime manifest | `c1d2901be95087ab175b7346dc6e744b09d801e7594b378620ce3297f4820f6f` |

Raw execution: `build/n6-pointer-1/cursor-refinement-1/` (75 verified manifest
entries). Guarded recovery/final receipt:
`build/n6-pointer-1/final/cursor-refinement-resume-1/`.
Reproduction requires these retained immutable inputs and fresh controller
admission; the driver is not permission to overwrite an existing attempt.

## Preservation, recovery and accounting

The original `final/cursor-refinement-1/` controller stop is immutable. Its
reference guard rejected the resolved remote HEAD alias transition even though
the approved Git setup had succeeded. The prospective recovery accepts only a
direct symbolic `refs/remotes/origin/HEAD -> refs/remotes/origin/main` alias
resolving to the exact accepted merge. Missing, ordinary/detached, chained,
retargeted and wrong-SHA aliases or unrelated ref movement remain rejected.
No ref was rewritten to satisfy the guard. The old failed receipt and charges
remain recorded; this does not relabel the failed session as successful.

An additive versioned authorization retains the old 72/96/3,360 MiB history and
sets cumulative execution/phase/aggregate ceilings to **97/107/3,371 MiB** only
for this task. The fresh attempt admits at most 42 MiB execution plus 2 MiB
nonexecution. Historical reservations remain 880 MiB; N6.3 reserves 107 MiB,
giving 987 MiB full-reservation obligation. The original 1 MiB source forecast,
16 MiB finalization, 8 MiB metadata and all physical-reserve/isolation rules
remain enforced. Reservation margin after unspent source forecast is
**634,880 bytes (0.60546875 MiB)**, not discretionary headroom.

The independently verified pre-documentation snapshot charged execution
77.226563 / 97 MiB, N6.3
86.019531 / 107 MiB and aggregate
2990.484375 / 3,371 MiB.
Final documentation, Git objects and receipts are additionally charged by
`APP-ACCOUNTING.json`, the final guard result and the additive post-commit
audit. These later measurements supersede this explicitly pre-finalization
snapshot; no historical consumption is reset.

The independent runtime audit verified all 75 raw manifest entries and all 49
protected records. Finalization must also confirm historical phase hashes,
13 worktrees, two stashes, source scope, explicit staging and reviewed Git blob
identities. The exact [14-file inventory](n6-cursor-refinement/changed-files.txt)
excludes images, build outputs and controller records from earlier attempts.

## Documentation follow-up and limitations

README.md still describes mouse/graphics as future and N5 as in progress.
docs/STATUS.md omits the accepted N6 components and contains stale input-driver
wording. docs/ROADMAP.md needs an accepted-component status synchronization,
without declaring all of N6 complete or altering its sequence. These three files
are identified for a separately scoped/accounted synchronization, not rewritten
by this refinement. Future publications should jointly review them using the
[publication checklist](../milestone-publication-checklist.md).

Evidence establishes QEMU q35/TCG BIOS and OVMF behavior at 1024x768. It does not
establish physical-hardware support. The existing PS/2 three-byte/primary-button,
single-surface and kernel-owned cursor limitations remain. No window manager,
mouse wheel/extra buttons, graphical Terminal, dock, GUI framework or branding
integration is introduced.

## Normal interactive preview

These commands use the preserved local images and read-only filesystem fixture;
they do not rebuild VOS or overwrite test logs. Run one VM at a time from WSL
with a working graphical display. Automated overflow, timing and corner tests
are separate from this human preview.

```bash
cd ~/src/myos
root="$PWD/build/n6-pointer-1/cursor-refinement-1"
disk="$PWD/build/app-console-20261025T051241Z/attempt-1/disk.img"
common=(-machine q35 -accel tcg -m 128M -boot d -nic none
        -display gtk -vga none -device VGA,edid=on,xres=1024,yres=768
        -serial stdio -no-reboot -no-shutdown
        -drive "file=$disk,if=none,id=vs0,format=raw,readonly=on"
        -device virtio-blk-pci,drive=vs0,disable-legacy=on)

# BIOS
qemu-system-x86_64 "${common[@]}" -cdrom "$root/vos-cursor-bios.iso"

# After closing BIOS: UEFI. Snapshot mode protects the installed variable template.
TMPDIR="$PWD/build/n6-pointer-1/tmp" \
qemu-system-x86_64 "${common[@]}" -cdrom "$root/vos-cursor-uefi.iso" \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,snapshot=on,file=/usr/share/OVMF/OVMF_VARS_4M.fd
```

Both images retain `vos.gui=1 vos.pointer=1`, omit verification flags and probe
modules, and contain the normal vsh, hello, Rixie, GUI demo and pointer demo
payloads. There is no arbitrary interactive-session deadline.

The Rust pointer panel starts first. Click the QEMU display if needed to capture
the mouse; move the arrow and press/release the primary button over the panel.
The panel changes appearance on a real click. Press `a` and `b` normally to
exercise keyboard-driven palette changes. Press `q` to leave the demo and
restore vsh. Run `apps`, `hello`, then `rixie`; use `about` and `exit`
inside Rixie. No timed key sequence is required. Ctrl+Alt+G releases QEMU's
pointer grab on the usual GTK frontend. Close the preview VM and terminal
before another guarded development session.
