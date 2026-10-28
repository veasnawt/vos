# R2-B progress

Accepted base: d2c0ac2d92bb917531a372d21d6b432425c4645f (PR #25).
Branch: feat/r2b-ps2-pointer-rust; independent checkout: /home/veasnawt/src/vos-r2b.

Preflight: Windows C: 22,717,329,408 bytes free; after 20 GiB reserve and
128 MiB conservative new-work provision, 1,108,275,200 bytes remain.
Provision includes checkout/Git, kernel variants, host tests, firmware media,
producer scratch, evidence and filesystem overhead; no historical allocation used.
All 5,747 historical snapshot entries and 49 protected entries matched; original
references, index, worktrees, stashes and retained symlink unchanged.
No running QEMU/build/cleanup worker found. Antigravity's resident hub is registered
to the old myos workspace, with only a console child; no active operation or
protected-state change observed. Claude bridges have no VOS open-file handles.
No executor attestation or discontinued maintainer controller was invoked.

Implemented draft: safe packet/idle transitions and narrow old-symbol C ABI in
the existing Rust archive; production pointer.o removed. Original C source and
pointer.h are untouched. USB host checks select the actual Rust symbols.

All verification gates A-G remain PENDING. No test or publication result is
inferred from compilation or this progress record. No physical test or USB.2-C.

## Continuation (Claude Code, 2026-10-29)

The text above is Codex's unaltered record from 2026-10-28. Codex stopped
after hosted checks and one failed BIOS case; no process was still writing
the checkout. Recovered findings and corrections:

- Rust decoder, FFI, build and CI changes audited; no defect found, unchanged.
- Fresh builds reproduce Codex's kernel (`6dbb8cb6...`) and archive
  (`4e7238c5...`) byte for byte; the host differential was also rerun fresh.
- Codex's BIOS failure (`shell response pwd`) came from the native runner
  omitting the accepted read-only disk, so vsh correctly reported no grant.
- Codex's "verify" kernel was identical to the normal kernel (no
  `-DVOS_GUI_VERIFY`); it was never booted.
- The runner now attaches the hash-pinned disk, rejects a verify/normal kernel
  mismatch and restores dropped N6.3 checks (checkerboard pixels, recentering,
  click cycles, shell key-leak check).
- Claude's first matrix attempt failed producing UEFI media (no system EFI
  GRUB modules); the hash-checked retained modules were then used.

Gates A-G are recorded in [the report](../r2b-ps2-pointer-rust.md).
