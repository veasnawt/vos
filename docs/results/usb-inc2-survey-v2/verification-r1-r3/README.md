# Increment 2 verification supplement (R1-R3)

This directory is a **supplement** to the Increment 2 evidence set, produced after the merge-readiness review. It adds the firmware-lease
read-only and rejection verification (R3) and BIOS kernel runtime checks (R2a, R2b), plus a local repeat (R1) of the hosted `devmap`
sanitizer tests that Build smoke CI already runs on every pull request (R1 is not new coverage). Nothing in the original evidence set
was rewritten. UEFI runtime verification of these paths is not covered.

## How the two manifests relate (snapshot documentation)

* `../manifest.json` is the manifest of the evidence set **as committed in `210d25f`**. It is byte-identical to that commit and is
  not edited by this supplement. It lists 94 files and, as its last entry, the report `../../usb-inc2-survey-v2.md`.
* This supplement edits that report: it adds the section "Additional verification R1-R3" (commit `fe74e85`) and a later correction commit fixes an inaccurate statement in that section and annotates the R1 row
  (see its "Correction" paragraph). Nothing else of the report changed. The report therefore
  has a **different SHA-256 now than the one recorded in `../manifest.json`**. A plain check of `../manifest.json` against the
  current tree reports exactly one mismatch, the report, and the other 93 entries still verify. That mismatch is expected, not
  damage, and the original manifest is **not** claimed to describe the current report.
* The recorded value is the real hash of the report at `210d25f`; check it with
  `git show 210d25f:docs/results/usb-inc2-survey-v2.md | sha256sum`
  (`c7effde93c6470cefe08f728b231baaac048b140838f1aab7a3701d3a7e1ff8e`).
* `manifest.json` in this directory covers the supplement files and records the facts needed to check every version of the report: the hash of the
  original manifest file (`original_manifest_sha256_unchanged`), the report hash recorded in it (`report_sha256_in_original_manifest_at_210d25f`),
  the report hash in the supplement commit (`report_sha256_at_fe74e85_supplement_commit`, check with
  `git show fe74e85:docs/results/usb-inc2-survey-v2.md | sha256sum`) and the current report hash (`report_sha256_now`).

## Contents

| Path | What it is |
| --- | --- |
| `r1/` | R1: sanitizer builds and runs of `usb-resources`, `usb2c1-qualify`, `usb2c3a-residue` (real `devmap.c`, ASan + UBSan) |
| `r3-1/` | R3 first gate run: **FAILED**, 11 of 12 mutants killed, `bad-memory-is-firmware` survived. Kept as it happened. |
| `r3-2/` | R3 corrected gate run: PASS, 595 assertions, 12 of 12 mutants killed |
| `r2a/` | R2a: production BIOS boot (`test.sh`) |
| `r2b/` | R2b: `VOS_TEST=1` BIOS kernel, `boot-128M` and `boot-2G` |
| `scripts-and-logs/` | the runners, the build log, and a snapshot of the storage log (later guard lines are not in the snapshot) |
| `leak-audit.json` | secret audit of the change set before this manifest existed (`scripts/leak-scan.py` of the main evidence set) |
| `leak-audit-correction.json` | secret audit of the correction commit (changed files only) |
| `leak-audit-full-pr.json` | secret audit of the whole pull request, every file changed against `main` (0 findings; the file count is recorded in the JSON) |

The repository ignores `*.log`; serial and QEMU logs are committed as `.txt` with byte-identical content
(`serial.log` is `serial.txt`, `qemu.log` is `qemu.txt`).

## Reproducing

R1: `python3 -I scripts-and-logs/r1.py` (needs the production kernel archive). R3: `python3 -I scripts/test-usb-firmware-lease.py
--out build/<fresh dir>`. R2a and R2b: the commands are in the report, section "Additional verification R1-R3", and in
`scripts-and-logs/r2b-run.sh`. The kernel archive and ISOs are not committed; their hashes are in `manifest.json`.

## Limits of this evidence

BIOS only for the kernel selftests and the production boot; no UEFI selftest or UEFI regression ISO of the earlier USB modes;
the apps-based C2/C3a native regressions and the C3b.2 execute harness were not run; the hosted harness cannot dereference a
lease, so the byte copy through a firmware lease is only exercised by the QEMU native cases; nothing ran on physical hardware.
