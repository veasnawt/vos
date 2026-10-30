# USB input bring-up Increment 1: storage admission

Admission: the owner confirmed (2026-10-30, in answer to a direct question) that Antigravity accepted this plan. This repository
holds no separate Antigravity record; the confirmation is recorded here as the acceptance.

| Item | Forecast (unit costs from build/usb2c42-gate0/baseline.json) |
| --- | --- |
| Worktree checkout | 21 MB |
| Dev scratch (rlib, test binaries, traces) | 15 MB |
| Mutation workers | 20 MB |
| Three hosted gate runs | 45 MB |
| Production BIOS and UEFI kernel/ISO builds incl. one rebuild | 60 MB |
| QEMU read-only discovery and regression matrix, compressed traces | 15 MB |
| Object-only measurement | 1 MB |
| Repository evidence | 2 MB |
| Total forecast | about 180 MB |

Limits: forecast 180,000,000 B, stop line 243,000,000 B, hard cap 270,000,000 B (`du -sb` of the worktree: checkout, build, evidence);
Windows `C:` reserve 20 GiB plus 1 GiB margin per step; every step projects before and measures after; STOP at growth above
1.25 x projection. Capacity at admission: C: free 22,961,090,560 B, 1,486,254,080 B above the reserve.

Amendments (each recorded in storage-log.txt and the investigation record):
1. The C4.1 gate step exceeded 1.25 x its projection (my projection counted only the mutation workers). The owner accepted the
   investigation; limits unchanged.
2. The survey gate step exceeded 1.25 x its projection (my projection ignored the 7.5 MB test binary in the unit-cost table). The
   owner approved crossing the 243 MB stop line to 262,000,000 B for the remaining steps; the cap and the 1.25 x STOP are unchanged.
No cleanup, move, pruning or deletion of any artifact was performed by this work.
