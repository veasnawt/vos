# Storage estimation rules after Increment 1

Two projections failed the 1.25x check, both my errors against numbers already in the unit-cost table
(build/usb2c42-gate0/baseline.json). Revised budget for this increment: forecast 180,000,000 B (exceeded), stop line
262,000,000 B (raised from 243,000,000 B by the owner after investigation 2), hard cap 270,000,000 B; final tree about 250 MB.

From now on every projection is built from this checklist, written down before the step, and the guard's number is the sum:
1. A Rust test binary is 7.5 MB with debug info (about 1.5 MB with -Cstrip=symbols; the gates now strip). Count one per gate main
   directory and one per mutation worker.
2. A mutation worker is a test binary + an rlib (1.3 MB) + a source copy: about 9-11 MB each, one per --jobs, created the first
   time a --workers-dir is used (reuse in place afterwards, a different gate's workers are overwritten, not added).
3. A full hosted gate is 56 MB the first time (workers 39 MB + main directory 18 MB with the 600-seed trace of 9.7 MB), 14-19 MB
   afterwards with reused workers; a gate that fails and is rerun costs its main directory twice (failed runs are kept).
4. Every ISO flavour costs its ISO (7.7 MB BIOS, 12.3 MB UEFI) plus a copied isodir; the kernel directory with ISOs is about 65 MB
   for the default and survey flavours of both firmwares.
5. Add a 25 percent contingency for failures and reruns, and project the whole remaining plan (not only the next step) before
   each step, so that a stop line is reached on paper rather than at a failed step.
6. Guard every step that writes, including hand-run ones (the two regression ISOs of this increment were not guarded; they are
   disclosed in storage-log.txt).
