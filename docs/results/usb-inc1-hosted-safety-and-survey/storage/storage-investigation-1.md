# Storage investigation 1: step inc1-gate41-1 exceeded 1.25 x its projection

- Step: C4.1 machine gate (`scripts/test-usb2c41.py`, 4 jobs, 102 mutants, 600 oracle seeds). Result: pass, 102/102 killed.
- Projection: 38,000,000 B. Actual growth: 57,984,114 B (1.53x). Guard limit (1.25x): 47,500,000 B. Tree after: 106,049,219 B.
- Cause: my projection error, not unexpected growth. I projected the cost of the four mutation workers alone (38.4 MB, unit-cost
  table `hosted_gate_workers_4x`) instead of the whole gate (`hosted_gate_run_full` 56.4 MB = workers 38.4 MB + main dir 18.0 MB).
  Measured: workers 39,378,405 B (4 x 8.9-10.8 MB), gate main dir 18,604,941 B (trace 9.7 MB, test binary 7.5 MB, rlib 1.2 MB).
  Sum 57,983,346 B, 1.03x the unit-cost total. Nothing grew that the table did not already say.
- Consequence for the rest of the plan (corrected unit costs): later gate runs reuse the workers in place (no second 39 MB) and cost
  about 12-19 MB each (200-seed traces); production BIOS+UEFI kernel/ISO builds 36 MB (60 MB with one rebuild); native QEMU runs with
  compressed traces 5-15 MB; evidence 2 MB. Projected final tree: 106 + 12..19 + 36..60 + 5..15 + 2 = 161..202 MB, below the stop
  line (243 MB) and the hard cap (270 MB), above the 180 MB forecast only if the upper bounds all occur.
- No deletion, move or pruning was done. Resuming needs the owner to accept this record (guard: remove `final/STOP`).

# Storage investigation 2: step inc1-survey-gate-1 exceeded 1.25 x its projection

- Step: survey gate (`scripts/test-usb-survey.py`, 2 jobs, 49 mutants). Result: FAILED, 4 surviving mutants (kept as evidence).
- Projection 16,000,000 B; actual growth 26,436,894 B (1.65x); tree after 225,310,859 B (stop line 243,000,000 B).
- Cause: my projection error again, and again against a number in the unit-cost table: a Rust test binary is 7.5 MB
  (`rustc_test_binary`). Measured: main directory 8.73 MB (test binary 7.37 MB, rlib 1.34 MB) and two new mutation workers
  17.7 MB (each a 7.37 MB test binary, a 1.34 MB rlib and sources). Sum 26.4 MB = the table's unit costs, not unexpected growth.
- Remaining work, from unit costs this time: final C4.2a gate 12-14 MB (main dir only; workers reused), final survey gate 8.7 MB
  (workers reused), native matrix 2.4 MB, evidence and documentation about 3 MB: 26-28 MB. Projected final tree 251-253 MB:
  above the 243 MB stop line by 8-10 MB, below the 270 MB cap.
- No deletion, move or pruning was done. Crossing the stop line needs the owner's explicit approval.
