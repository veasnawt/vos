# USB.2-C4.1 storage forecast (made before any substantial work)

Measured 2026-10-30 after creating the isolated worktree `/home/veasnawt/src/vos-usb2c41`
(branch `feat/usb2c41-hosted-init-model`, base `03e7532`):

| Item | Bytes |
| --- | --- |
| Checkout of the base tree (`du -sb`, measured) | 19,478,155 |
| Windows `C:` free (measured) | 23,033,344,000 |
| `C:` reserve (unchanged guard) | 21,474,836,480 |
| `C:` headroom above the reserve | 1,558,507,520 |

C4.1 limits (new, for this milestone only; the C3b.2 600 MiB cap is not reused):

- **Cap** 209,715,200 B (200 MiB) and **stop line** 167,772,160 B (160 MiB) for the whole C4.1
  worktree (checkout + build + evidence, measured with `du -sb`). Crossing the stop line means STOP
  and request a storage-management plan from Antigravity. Codex performs no cleanup or migration.

Forecast of growth (estimates, to be replaced by measurements in the final report):

| Item | Forecast | Basis |
| --- | --- | --- |
| Hosted rustc test binaries and rlibs | 40,000,000 | ~4-8 MB per debug test binary, a handful, overwritten in place |
| Mutation builds | 0 net | every mutant reuses one fixed output directory (overwritten, nothing is removed) |
| Oracle trace export (text) | 12,000,000 | ~100k steps x ~120 B; only a gzip summary is kept as evidence |
| Production binary-identity build | 23,000,000 | C3b.2 `kernel-4` was 22.8 MB |
| Kernel-target object for size data | 1,000,000 | no-std model object, not linked |
| Repository evidence (docs/results) | 2,000,000 | logs, JSON, no raw traces or binaries |
| **Total growth** | **~78,000,000** | |
| **Forecast tree size** | **~97,500,000** | 58 % of the stop line, 46 % of the cap |

Headroom is sufficient; no storage-management plan is needed unless a measured step projects past
the stop line. Every substantial step runs through `final/guard.sh`, which records the tree size
and the `C:` free space before and after.

Not touched: historical build trees (`vos-usb2c3b1/build`, `vos-usb2c3b2/build`, `vos-r2b/build`),
evidence directories, other worktrees, the Gate 0 report. No deletion, pruning, move or compaction.
