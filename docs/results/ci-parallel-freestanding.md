# CI: parallel Freestanding groups behind the required check

**The Freestanding checks now run as three parallel jobs behind an aggregator that keeps the required check name.**
- Wall clock fell from 37.6 to 14.9 minutes.
- Every one of the 37 verification steps is moved verbatim and passed before and after the split.
- The aggregator fails on a failed, skipped or cancelled group, shown by real GitHub runs.

No test, coverage, timeout, permission or repository rule changed.

## Change

`.github/workflows/build-smoke.yml`:

| Job | Contents |
| --- | --- |
| `freestanding-core` | legacy i386, user ELF profiles, ELF bounds, Multiboot2, B1, H1-B, the 11 R2 input gates and the 5 network gates (22 checks) |
| `freestanding-graphics` | A3 framebuffer, R3-G1 desktop, R3-G2 screen (3 checks) |
| `freestanding-usb` | USB.1 probing, USB.2-C1 through USB.3-A, USB survey and firmware-table lease (12 checks) |
| `build-smoke`: **Freestanding builds and user ELF static tests** | The required check, as an aggregator. It `needs` the three groups, runs `if: always()`, and exits 0 only when every group's result is `success`. |

**Each group:**
- checks out the same commit;
- runs the identical prerequisites, toolchain, tool-version record and reproducible kernel/ISO build;
- keeps the original 45-minute limit;
- uploads its own failure-only text logs (`build-smoke-logs-core`, `-graphics`, `-usb`).

QEMU runtime smoke, Gitleaks, triggers, permissions and concurrency are unchanged.

## Verification

| Gate | Evidence |
| --- | --- |
| Partition proof (`scripts/test-ci-partition.py`) | Against main's workflow:<br>• all 43 original steps are accounted for<br>• the 37 verification steps are each in exactly one group, unmodified as parsed steps and in their original order<br>• the setup steps are identical in every group, and the job settings are unchanged<br>• the artifacts are distinct<br>• exactly one job has the required name<br>• the QEMU job is unchanged<br>[`partition.json`](ci-parallel-freestanding/partition.json) |
| Checker negative controls | The checker rejects every deliberate corruption:<br>• a dropped, duplicated, or weakened step (desktop and screen gates)<br>• a shared artifact name<br>• a missing `always()` or a missing group<br>• a second required-named job<br>• `continue-on-error` or a raised timeout<br>• an aggregator that accepts `skipped`<br>• a changed QEMU job<br>[`partition-checker-negative-controls.json`](ci-parallel-freestanding/partition-checker-negative-controls.json) |
| Aggregator decision | The extracted aggregator script passes only for all-success, across 15 cases: each group failed, skipped, cancelled or missing; an extra group; empty. |
| GitHub controls | Real runs:<br>• **positive:** PR run 42496233386, all groups success, aggregator success<br>• **failed group:** run 42496270398, graphics fails, aggregator **failure**<br>• **skipped group:** run 42496273355, USB skipped, aggregator **failure**<br>• **cancelled:** run 42496276468, cancelled after every group started; the aggregator still ran and **failed**<br>[`github-negative-controls.json`](ci-parallel-freestanding/github-negative-controls.json), [`cancel-control-aggregator-log.txt`](ci-parallel-freestanding/cancel-control-aggregator-log.txt) |
| Step results | Each of the 37 verification steps succeeded on PR #69's head (single job) and on this PR's head (groups), and every group's setup succeeded. [`step-comparison.json`](ci-parallel-freestanding/step-comparison.json) |

## Timing (measured, not assumed)

| | Before (PR #69 head, run 42484313548) | After (this PR, run 42496233386) |
| --- | --- | --- |
| Build smoke wall clock | 37.6 min | **14.9 min** |
| Longest Freestanding job | 37.6 min (single job) | 14.8 min (graphics) |
| Other Freestanding jobs | n/a | USB 11.7 min, core 8.5 min |
| Runner time (all Build smoke jobs) | 41.5 min | 39.1 min |
| Billed minutes (each job rounded up) | 42 | 42 |

Each group now has 30 or more minutes of headroom. The graphics group, which carries the about-12.5-minute R3-G2 gate, is the longest. [`timing.json`](ci-parallel-freestanding/timing.json)

## Limits

This is a CI structure change only. The control branches `ci-negative-agg-fail` and `ci-negative-agg-skip` are throwaway branches that are never merged.
