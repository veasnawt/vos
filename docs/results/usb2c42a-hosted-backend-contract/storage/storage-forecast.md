# USB.2-C4.2a storage forecast and admission

Admission: the owner confirmed (2026-10-31, in answer to a direct question) that Antigravity accepted the section 2.3 storage plan
of the C4.2 Gate 0 report with these C4.2a limits. This repository holds no separate Antigravity record; the confirmation is
recorded here as the acceptance.

| Item | Forecast (unit costs from build/usb2c42-gate0/baseline.json) |
| --- | --- |
| Worktree checkout (tracked files incl. C4.1 evidence) | 20.1 MB |
| Dev scratch (rlib, one test binary ~9 MB, 300-seed trace ~5 MB) | 15 MB |
| Production kernel + BIOS ISO identity build | 22.8 MB |
| Resource measurement, object-only (no staticlib, no core archive) | 1 MB |
| Mutation workers (2 workers, built once, reused in place) | 20 MB |
| Three gate runs (rlib + test binary + trace) x 15 MB | 45 MB |
| Evidence in the repository | 0.4 MB |
| Total forecast | ~124 MB |

Limits: stop line 168,000,000 B; hard cap 200,000,000 B; `C:` reserve 20 GiB (+1 GiB margin per step). Every step: projection
before, measurement after, STOP at actual growth above 1.25 x projection. Capacity measured 2026-10-31: C: free 23,012,720,640 B,
1,537,884,160 B above the reserve. No cleanup, move, pruning or deletion of any artifact is performed by this work.
