# R-series ownership inventory, R4 readiness and R5 exit matrix

**Planning record, not an implementation authorization.** It is source-bound to main `2c0890a3` (tree `f3ec9622`) plus the R3-G1 candidate, and implements the [Master Plan section 6](MASTER-PLAN.md#6-r-series-rust-first-kernel-migration). No R4 work is authorized until its entry gates and resources are approved by the owner.

 Its sources are 104 production/test C files, 7 assembly files, 20 `arch/x86_64/rust` glue modules and 51 `kernel/rust` modules.

## 1. Ownership inventory by subsystem

| Subsystem (milestone) | Rust-owned today | Remaining C / asm | Pure vs hardware/lifecycle | Oracle / regression | Constraints | Conflicts / blockers |
| --- | --- | --- | --- | --- | --- | --- |
| Boot info, Multiboot2, framebuffer tag, reservations, EFI exclusion (R1/A2/A3/B1) | `mb2_parse`, `boot_framebuffer`, `boot_resv`, `efi_exclusion` | `kernel.c` orchestration (511), `boot.S` | Orchestration is lifecycle | Frozen-C differentials in Build smoke | Runs before the PMM; no allocator | None |
| ELF (R1) | `elf_validate` | `elf.c` (204) loader glue, `loader.c`, `exec.c` (338) | Validation pure; mapping and process creation are R4 | R1 gate, exec_verify | Touches VMM and process | R4 dependency |
| PS/2 input, console (R2) | Decoder, routing, keyboard queue, ordinary poll, controller read/write/lifecycle, console line/accept/reader | `keyboard.c` (57), `pointer.c` (20), `input_console.c` (66) adapters; `user_console.c` (249) leases | Leases are ownership policy (R4-adjacent) | R2 gates (frozen C) | IRQ-free polling; single i8042 owner | R2 closed; `pointer_idle` is defined in both C and Rust, so the native proof is stale (pre-existing) |
| Networking (R3-N) | ARP/IPv4/UDP/TCP, virtio-net validation | `virtio_net.c` (279: PCI, DMA, rings), `net_*.c` thin ABI, `net_*_boot.c` test glue | Device/DMA ownership stays C by design | R3-N1/N2/N3/N3-S gates | No IOMMU; DMA ownership | None active |
| Graphics (R3-G) | `boot_framebuffer` (A3); **R3-G1 candidate:** `desktop_requested`, `desktop_compute` | `screen.c` (545: console renderer, cursor, GUI sink), `desktop.c` (`desktop_paint`/`desktop_bar`), `font8x16.c` data, `gui.c` (272: GUI service and ABI v5), `gui_verify.c` | `screen.c` text layout and clipping arithmetic is largely pure; framebuffer writes are device memory; `gui.c` is ABI and lifecycle | Host render tests (`tests/host/desktop.c`, `pointer.c`, `gui.c`), native screenshots | MMIO framebuffer (WC); pitch-aware writes; cursor underlay reads VRAM | N6.4 deferred (owner); GUI ABI frozen; historical N6 scripts pin `desktop.c` |
| Storage (R3-S) | none | `blockdev.c` (632), `virtio_blk.c` (1,508), `ext2.c` (991), `file.c` (836), storage stress | Parsing (ext2 structures) pure; virtio-blk is DMA | Host + VOS_TEST suites | DMA, cache ownership, fault injection | **BLOCKED:** SEC-02 seals cover uncommitted edits to `blockdev.c`/`ext2.c`/`virtio_blk.c` in the protected `vos-sec02-storage-range` worktree; `file.c` depends on them |
| USB (H-series, with Rust) | xHCI discovery/qualify/DMA model/containment/survey, EHCI and HID models | `xhci*.c`, `usb_dma.c`, `usb_resources.c` adapters | Hardware authority stays C | USB gates | Physical tests unauthorized | Separate H track |
| PCI, devices (R4/R5) | none | `pci.c` (109), `devmap.c` (551) | Config-space access is hardware; devmap is page-table/MMIO policy | Selftests | Page tables, PAT | R4 (VMM) |
| Memory (R4) | none | `pmm.c` (291), `vmm.c` (600), `kmap.c` (552), `usercopy.c` (92) | PMM queries pure over bitmaps; mutation is lifecycle; VMM walks touch live tables | `selftest_pmm` (605), `selftest_vmm` (514), `selftest_kmap` (898), stress | Non-preemptive kernel, IF=0 except idle `sti;hlt`; single CPU; frames zeroed; poison in VOS_TEST | None; highest scrutiny |
| Scheduler, threads, IPC (R4) | none | `thread.c` (819), `thread_switch.S`, `channel.c` (311), `irq.c` (343), `pic.c`, `pit.c` | Queue/state decisions pure; switch is asm | `selftest_sched` (689), `selftest_ipc` (576), `selftest_irq` (416), `selftest_mp` (255) | 16 threads × 16 KiB stacks; IRQ entry | R4 |
| Process, syscalls, security (R4) | none | `process.c` (617), `syscall.c` (134), `io_scope.c` (313), `app*.c`, `exec.c` | Capability and ownership decisions pure; teardown is lifecycle | `selftest_user` (1,134), accounting, io_scope tests | ABI v0–v5 frozen | R4 |
| CPU and architecture core (retained by design) | none | `boot.S`, `entry.S`, `usermode.S`, `thread_switch.S`, `gdt.c`, `idt.c`, `cpu.c`, `exception.c`, `panic.c`, `console.c` (serial), `lib.c` | Hardware entry, descriptor tables, MSRs | Boot tests | Assembly is necessary | Justify each in R5 |
| Tests in the kernel (VOS_TEST) | none | `selftest_*.c` (~12,000 lines), `storage_stress_*.c`, `*_verify.c`, `user_progs.S` | Test-only | Themselves | Not production | R5 decision: keep as a C harness or port |

## 2. Recommended next R3 boundaries (storage is blocked)

1. **R3-G2, `screen.c` pure layout arithmetic.** Candidates are the text-cell, wrap and scroll position math, clipping rectangles, and the cursor clip and underlay extent. The VRAM writes and the cursor's VRAM reads stay C. Oracles are host `screen-input.c`/`desktop.c` plus pixel identity.
2. **R3-G3, `gui.c` validation decisions.** Candidates are the create-descriptor checks, the upload bounds arithmetic and the token/rights checks. The ABI, queues and wait/wake stay C. This is the same pattern as R3-N3; it needs the GUI ABI unchanged.
3. Storage parsing (ext2 directory/inode validation) only after SEC-02 is resolved by the owner.

## 3. R4 readiness: smallest meaningful increment

**R4-M1, PMM read-only frame decisions.** Move `pmm_frame_state_in`, `pmm_free_check_in` and `pmm_check_in` (with `count_bits`) to safe Rust over the bitmap words and instance scalars, which C passes as slices and values.

- **Stays C:** `struct pmm`, the `.bss` bitmaps, `pmm_init_in`, `pmm_alloc_in`, `pmm_free_in`'s mutation, `pmm_reserve_in`, the kernel wrappers and zeroing/poisoning.
- **Why first:**
  - `pmm_free_check_in` is the security gate for every frame free: alignment, range, never-allocatable (protected) frames, double free and counter corruption. `devmap.c` also uses it to validate page-table frames.
  - `pmm_check_in` is the invariant auditor used by the DMA teardown checks.
  - All three are read-only `*_in` functions that already run on synthetic instances, so a frozen-C differential is natural.
  - No allocation, interrupt or lifetime change is needed.
- **Entry gates:**
  - frozen `pmm.c` reference;
  - an exhaustive differential on synthetic instances: 64–1,024 frames, every alignment/range/protected/allocated/free class, corrupt states (`alloc & ~avail`, wrong usable/free counts, hint violations, avail below 1 MiB);
  - ASan/UBSan with overflow-checked Rust, and guard pages around bitmaps sized exactly `PMM_WORDS(frames)`;
  - at least 25 mutation controls;
  - panic freedom; no `popcnt` assumption (baseline x86-64);
  - frames and whole paths for `pmm_free_frame` and the GUI/DMA teardown paths;
  - `selftest_pmm`, `selftest_vmm`, `selftest_kmap`, `selftest_user`, `selftest_mp` and the stress suites on BIOS/UEFI at 128 MiB and 2 GiB;
  - every DMA teardown check (network boots, USB.2-C2);
  - paired benchmarks of free-check and the full-bitmap audit, where parity is expected and inspected.
- **Concurrency and isolation rules to preserve:**
  - single CPU, non-preemptive kernel, IF=0 except idle;
  - no Rust state, no locking change;
  - same results for the same instance bits;
  - process isolation and frame ownership unchanged.
- **Following increments, each separately authorized:** R4-M2 alloc/free/reserve transitions with the hint; R4-M3 `pmm_init_in` memory-map classification; R4-V1 VMM read-only validation (`check_va`, `check_nonleaf`, canonical/range tests, `usercopy` `user_range`); R4-S1 scheduler queue decisions; R4-P1 process/capability checks (`io_scope_request_valid`, grants).
- **Resource forecast for R4-M1** (same accounting; ledger ≈ 165 K raw now):
  - admission and completion transitions ≈ 2 × 210 K guarded;
  - receipts 100 K raw (125 K guarded);
  - publication 180 K raw (225 K guarded);
  - transient 60 K and contingency 40 K;
  - total ≈ 870 K guarded, so with the remaining provision a new allocation of about 650–700 K is needed.
  - Worker: the existing design plus VOS_TEST selftest images at 128 MiB/2 GiB.

## 4. R5 completion matrix (not met today)

| Criterion | Required evidence | Status at 2c0890a3 + R3-G1 |
| --- | --- | --- |
| Inventory of every C/asm module with owner and disposition | This table, maintained per increment | **Started** (this document) |
| Verified Rust ownership of the approved migration targets | Frozen-C differential, mutants, panic-free link, symbol ownership per target | Boot, ELF, input, network protocols, virtio-net validation, framebuffer, (desktop decisions candidate) |
| Justification for each retained C/asm boundary | Per-file rationale: CPU entry, context switch, MMIO/DMA drivers, test harness | Assembly and device adapters justified informally; **not yet written per file** |
| Memory, scheduler, process, security core (R4) migrated or justified | R4 increments | **Not started** |
| Storage (R3-S) | Migration after the SEC-02 resolution | **Blocked** (SEC-02) |
| Graphics (R3-G) | `screen.c`/`gui.c` decisions | **In progress** (G1 candidate) |
| Public userspace ABI v0–v5 + pointer v1 preserved | ABI tests, app compatibility boots | Preserved so far |
| BIOS/UEFI and security regressions | Native suites, hostile-device and isolation matrices | Per increment |
| Resource and memory-safety acceptance | Stack, image, performance policy, sanitizer evidence | Per increment; exceptions owner-approved |
| Reproducible builds and complete publication evidence | Repro checks, CI, ledger | Per increment |

R5 is **not** complete. The largest blockers are R4 (not started) and storage (SEC-02).

Deferred: N6.4 (owner deferral), the storage migration (SEC-02), the USB/H-series hardware work, and the stale `test-ps2-pointer-native.py` execution proof (pre-existing).
