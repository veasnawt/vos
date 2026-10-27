# USB.2-B progress

Baseline: `f24b3a803a65324f31cdb4c43fd4f504394a7cdb` (accepted PR #24).
Checkout: `/home/veasnawt/src/vos-usb2b`; branch: `feat/usb2b-resource-ownership`.
Initial HEAD equals the baseline. No USB.2-B work was present at recovery.

## Investigation checkpoint

- Read the USB.2 architecture, USB.2-A and USB.1 review, repository policies,
  PMM, devmap, VMM, scheduler and GUI worker-reclamation contracts.
- Existing devmap mappings are permanent. A failed multi-table allocation can
  retain empty tables. Scoped leases need transactional table ownership;
  self-test rollback is not a production release API.
- PMM proves allocated frames, not subsystem ownership. Generation-bound
  records must retain physical identifiers on failure or abandoned Rust values.
- Scheduler IDs are never reused. Only THREAD_FREE proves reaping; a completion
  flag or THREAD_DEAD does not permit freeing worker arguments.
- All 5,747 historical snapshot entries matched their recorded file hashes;
  both original repositories' HEAD/index/status/refs/worktrees/stashes matched.
  All 49 protected inventory entries matched. The first reference comparison
  used the wrong display format; normalized ref-name/hash comparison matched.
- No QEMU or Antigravity process was present. Existing Claude bridge processes
  had Windows-system-directory working directories and no open VOS files;
  no competing VOS writer was observed. No process was terminated.

## Bounded plan

Implement exclusive scoped devmap ownership, one ordinary non-DMA control page,
and explicit Rust ownership/release using the existing logical lease types.
Exercise a controlled non-device mapping and worker fixture, never xHCI MMIO.
Preserve the existing portable lifecycle model; do not implement a live backend.
Verify allocation rollback, context rejection, retained identifiers, worker
reaping and unchanged default/read-only boots on BIOS and UEFI.

New-output provision: **84 MiB including this independent checkout**, with
read-only reuse of accepted app ELFs and GRUB inputs. Linux free space at start:
1,007,318,900,736 bytes; C: 21,579,321,344 bytes; E: 244,754,505,728 bytes.
Retain the existing physical reserve; stop if new growth threatens it.
No D1 controller, legacy funding or historical reservation is reused or changed.

## Outstanding

Implementation, gates A-G, native execution, measurements, final preservation,
commit and publication are **PENDING**. No success is inferred from this plan.

## Implementation and verification checkpoint

- Implemented an exclusive devmap fixture lease with two actually owned PTs,
  exact rollback, kernel-CR3/context checks and a fixed non-access physical
  region. No production BAR qualification or xHCI backend is claimed.
- Implemented one ordinary PMM page owner, explicit C/Rust checked release and
  retained identities on Drop/failure. Reused USB.2-A Lease and shared value
  definitions; the controller state machine is unchanged.
- Kernel linking and strict five-segment validation passed twice. First build
  inputs/results remain recorded separately from the final native kernel.
- Hosted final suite: 1,016 ASan/UBSan assertions; one Rust layout test; Copy,
  Send and Sync compile rejections PASS. Optimized C-equivalent/Rust success
  and first/second-table rollback raw samples retained separately.
- Initial host compile failed on an assertion-parentheses warning. The failed
  log remains; the assertion was parenthesized without changing expectations.
- BIOS normal and opt-in native execution PASS: four allocation stages,
  18 created/reaped worker cycles, Drop retention, GUI a/b/q, shell, hello,
  Rixie and PCI metadata preservation.
- Original UEFI boot stopped at the unchanged 35-second pre-GRUB gate. A
  separate 35-second observation-only diagnostic reached GRUB by 20 seconds;
  the first delay's cause remains INCONCLUSIVE, not a retrospectively passing run.
- A subsequent UEFI normal boot reached VOS, but refused GUI startup without a
  framebuffer. The new media recipe had omitted the accepted efi_gop header.
  Both failed roots, logs, receipt and diagnostic screenshots remain unchanged.
- Corrected only the media recipe to prepend arch/x86_64/grub-uefi-head.cfg.
  A fresh UEFI image/native verification is pending; kernel/app bytes unchanged.
- Revised ordinary new-output estimate: **96 MiB including retained failed
  media and transient producer scratch**, instead of the initial 84 MiB.
  Fresh physical capacity covers remaining work above reserves. This changes
  no historical authorization, ledger, reservation, controller or funding.

Current HEAD is still the accepted baseline; all USB.2-B work is uncommitted.
Next: finish UEFI, focused compatibility/static checks, independent preservation
and report. Do not publish while any required gate remains incomplete.

## Final verification checkpoint

- Corrected-GOP UEFI normal/resource cases both PASS, with unchanged kernel/app
  bytes and 35-second gates. Both firmwares verify four resource stages,
  18 created/reaped workers, Drop retention and ordinary GUI/apps.
- Fresh compatibility: 46 core, four policy, model Send/Sync controls, detected
  stale-ticket mutation, 40,110 USB.1 differential, 497,127 R1 bounds cases.
  Four apps pass static/kernel ELF checks; 140 retained negative ELF inputs
  match the independent C reference.
- Static/memory/performance evidence complete. Native kernel full hash:
  017cbc9eda6144b2c0078e531b6e1808627f5fd1111ebea56bcd262fd5e7c4c8.
  Final comment-only relink full hash:
  65d980871e1f4401212cb08574a2317997b9a3d4c7f7d1bd693c75cf9844b869.
  All PT_LOAD metadata/bytes and the Rust archive are identical.
- Final independent preservation PASS: 5,747 historical entries, 49 protected
  entries, original refs/index/working sets, 13 worktrees, two stashes, retained
  symlink and original USB.1 checkout.
- Precommit logical footprint 90,125,403 bytes; allocated blocks 97,243,136.
  The 96 MiB logical-output estimate excludes about 7 MiB filesystem overhead.
  Conservative physical producer peak provision: 104 MiB, not sampled high-water.
  Fresh host space covers finalization above reserves. These local estimates
  change no owner-authorized historical ceiling, reservation or funding.
- Report, raw logs, failures and genuine captures are preserved in the candidate.
  No runtime code changed after native verification.

After commit, Git and local `build/usb2b-1/publication.json` record exact HEAD and
publication state, avoiding a self-referential commit hash. Only scoped staging,
commit, push/PR and CI review remain.
