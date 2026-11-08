# R4-M1H: PMM small-instance memory-safety hardening

**These all PASS:**
- The out-of-bounds write in `pmm_init_in()` for valid small instances is eliminated by one bounded statement.
- A guard-paged regression shows the frozen accepted C faulting at exactly the bitmap end, in an isolated child. The fixed code completes and equals the frozen C wherever the frozen C is defined.
- Instances from 256 frames up to `PMM_MAX_FRAMES` are identical word for word.
- All 20 mutation controls are detected.
- The R4-M1 frozen-C differential still passes unchanged.
- BIOS/UEFI QEMU self-tests, PMM-exhaustion/mp/stress, and the network and USB.2-C2 DMA-teardown suites pass.
- Worker runs r1 and r2 pass.

No stack, loaded-image or performance exception is requested.

Physical hardware is **NOT TESTED**. No merge without owner approval.

The baseline is accepted main `36368bc84512a056568c9577d51239318c5edfd2` (R4-M1 merged). The branch `codex/r4-m1h-pmm-init` starts at `7533851`, whose tree `4f6b46db` is identical to main's. The source commit is `08405154` (tree `feca7fb0`).

## The defect

The accepted `pmm_init_in()` ends its classification with a defensive step 5: `clear_frames(avail, 0, PMM_LOW_LIMIT / PAGE_SIZE)`. That always clears frames 0–255, whatever the instance size.

The function accepts any nonzero multiple of 64 frames up to `PMM_MAX_FRAMES`, and its bitmaps hold `PMM_WORDS(frames)` words. For a valid 64-, 128- or 192-frame instance, step 5 therefore read-modify-writes 3, 2 or 1 words (24, 16 or 8 bytes) past the end of the avail bitmap. It clears bits in whatever memory follows.

Every other write in the function is bounded:
- the memsets cover exactly `PMM_WORDS(frames)` words;
- the free pass and `protect()` are clipped to `limit = frames * PAGE_SIZE`.

No in-tree caller is affected. The kernel instance has `PMM_MAX_FRAMES` (262,144) frames, and the self-tests and hosted tests use 16,384 and `PMM_MAX_FRAMES`.

## The fix

```c
/* 5. Defensive: nothing below 1 MiB, whatever the inputs said. Only the tracked frames: an instance smaller than
 *    1 MiB has PMM_WORDS(frames) words. min(limit, 1 MiB) / PAGE_SIZE = min(frames, PMM_LOW_LIMIT / PAGE_SIZE). (R4-M1H) */
clear_frames(avail, 0, (limit < PMM_LOW_LIMIT ? limit : PMM_LOW_LIMIT) / PAGE_SIZE);
```

The bound is exactly the smaller of the tracked frames and `PMM_LOW_LIMIT / PAGE_SIZE`. This holds because `limit = frames * PAGE_SIZE` (no overflow: frames ≤ `PMM_MAX_FRAMES`) and `PMM_LOW_LIMIT = 256 * PAGE_SIZE`.

It is written over bytes because the literal `frames < 256 ? frames : 256` form grew GCC 15's `pmm_init_in` frame from 72 to 88 B. This spelling keeps both compilers' frames unchanged (see [Resources](#resources)).

**Why behavior is preserved:**
- Below 256 frames, every tracked frame is below 1 MiB, and the free pass starts at 1 MiB, so no frame is ever marked allocatable. The in-bounds part of the old clear was already a no-op.
- From 256 frames up, the bound is 256 exactly as before.

Every other initialization behavior is unchanged: error precedence (EINVAL before EMMAP), allocation policy, reservation rules, error codes, `struct pmm` and the ABI. `pmm_init_in()` stays C. The R4-M1 Rust checks and their unsafe boundary are untouched. Ownership, allocation, freeing, zeroing, poisoning, interrupts and page tables are unchanged.

## Verification

| Gate | Evidence |
| --- | --- |
| Scope (`scripts/test-pmm-init.py`) | Replacing the new statement and comment in production `pmm.c` with the old ones reproduces the accepted main `pmm.c` byte for byte (sha256 `bb177a03…`, blob `e3b22f3c`, manifest `tests/reference/pmm-r4m1h.json`). The frozen vulnerable oracle is the existing `tests/reference/pmm-r4m1.c`. Its helpers and `pmm_init_in` (the first 3,825 bytes, up to `pmm_alloc_in`) are byte-identical to the accepted main's, and the gate checks this. |
| Negative control | In a forked child, the frozen C runs on exact 64/128/192-frame bitmaps flush against a PROT_NONE page. It must fault at **exactly the first byte past the bitmap**: the load of the read-modify-write of the word after the last tracked frame. The child's handler reports the address through a pipe, so the expected fault cannot end the suite. **3/3 fault at the bitmap end**, in the release and the ASan/UBSan builds. |
| Small instances (64, 128, 192 frames) | The production code runs on exact guarded bitmaps (canary bytes before them, a guard page after, a garbage pattern inside to check zeroing). It completes on every map and reservation table. Its return value, state, counters, hint, ignored bytes and every in-bounds word equal the frozen C, which runs on a 4-word bitmap so that it stays defined. The frozen C's writes past `PMM_WORDS(frames)` are its defect and are not treated as an output. |
| Large instances (256 frames to `PMM_MAX_FRAMES`) | Both sides run on exact guarded bitmaps and must be identical field for field and word for word. The inputs are fixed sizes (256, 320, 384, 448, 512, 1,024, 4,096, 16,384, 65,536, 262,144) and seeded sizes over maps with holes, reserved and overlapping regions, unaligned ranges, entries straddling the limit, overflowing entries (including reserved entries that wrap past 2^64 from inside the instance) and reservation tables. **0 mismatches** over 27,819 cases and 62,656,992 checks. Those include 2,570 maximum-size instances, 4,684 with frame 256 (exactly 1 MiB) allocatable, 18,504 with ignored bytes and 10,652 with overflowing entries. The digest `aa6fef2bf2d8baac` is identical in the release and ASan/UBSan builds. |
| Refused initialization | EINVAL (NULL bitmaps; 0, 1, 63, 100, 255 and over-maximum frames) and EMMAP (NULL boot info or map, no entries, version 1, entry size 16 or 28), for small and large frames, including the EINVAL-before-EMMAP precedence. Results are identical, and no bitmap word is written. |
| Mutation controls | **20/20 detected**, 9 of them by a fault on a guard page and 11 by result mismatches:<br>• restoring the unbounded clear<br>• off-by-one for small and for large instances<br>• the 1 MiB threshold halved, max instead of min, 512 frames, frames-as-bytes<br>• short or dropped avail/alloc zeroing<br>• low memory left unprotected<br>• protect and free-pass rounding, overflowing entries, ignored bytes, the reservation pass, the usable count, the limit clip<br>• two error checks<br>Excluded as unobservable at `pmm_init_in`'s boundary, with reasons in the script:<br>• dropping only one of the two redundant low-memory protections (the free pass's 1 MiB clamp or the clear)<br>• clearing one frame fewer, and `<=` at the 1 MiB threshold (those frames are never allocatable before the clear)<br>• counting usable frames from word 1 (words 0–3 are always zero) |
| R4-M1 preserved | `scripts/test-pmm-rust.py` is unchanged and passes: digest `a4f7a96ad9362c6b`, 42/42 mutants, `pmm_init_in` C frame 88 → 88. |
| Native and DMA teardown | Worker `r4-m1h-pmm-init-v1` **r1 PASS** (seal `43a4221b…`) on source commit `08405154`, eleven jobs:<br>• containment<br>• build, and a byte-identical rebuild of kernel `5727a83a…` and archive `679402a8…` (the archive is unchanged from R4-M1; GCC 15.2 in the sandbox)<br>• the init gate (same digest `aa6fef2b…`, 3/3 negative control, 20/20)<br>• the R4-M1 gate (`a4f7a96a…`, 42/42)<br>• **BIOS and UEFI QEMU at 128 MiB and 2 GiB**: the VOS_TEST image ran to `VOS-TEST: PASS` (exit 33) with the bootinfo, pmm-synthetic, pmm-kernel, vmm, kmap and user self-tests (0 failed), and the Rust PMM checks were observed and replayed identically through the frozen C<br>• the pmm-exhaust, mp (qemu64, 2 GiB, `-cpu max`) and stress images: **7/7 PASS**<br>• the network DMA-teardown suites (ARP/IPv4/UDP, virtio-net, TCP)<br>• the **USB.2-C2 native DMA matrix**: 8/8 on SeaBIOS and OVMF (normal, DMA exercise, exercise with qualification, and no xHCI), with source-built `vsh`/`hello`/`rixie`/`gui-demo`; the exercise reports `usb2c2: PASS; unpublished pages released exactly`<br>The boot-reservation native check runs in the QEMU job (r2). The serial verdict lines are in [qemu-serial-excerpts.txt](r4-m1h-pmm-init/qemu-serial-excerpts.txt). The console screenshots (BIOS `c65dc554…`, UEFI `d3d478d2…`) are retained in the campaign evidence; they are not republished, because they match the R4-M1 published console. Retained: 3,048,018 B. |
| CI equivalence | Worker **r2 PASS** (seal `27a15189…`, 2,425 s): every Build smoke step in the three Freestanding groups, including the new init step (`ci-boot` 212 s), plus the QEMU runtime job (boot images, survey, B1 boot reservations at 128 MiB and 2 GiB). The hosted-only USB C4.1/C4.2a/USB.3-A models contain no kernel code; they are not rerun on the host and are covered by the PR's exact-head CI. Retained: 1,670,368 B.<br>The root steps ran exactly as approved for r1 and r2. The plan is `15abde43…` and the root procedure `234c2568…`; BOOT `14438e12…` and `payload.py` `c6e088cd…` are unchanged. |

## Resources

| | GCC 13 base → candidate | GCC 15 base → candidate |
| --- | --- | --- |
| `pmm_init_in` / `pmm_init` whole path | 96 → 96 | 80 → 80 |
| `kernel_main`, `exception_handler` | 11,936, 4,560 → unchanged | 11,984, 4,592 → unchanged |
| `pmm_init_in` code | 1,033 → 1,048 B | 1,121 → 1,185 B |
| `vos.bin` file | 1,166,280 → 1,166,248 B | 1,170,904 → 1,170,944 B |
| Load layout | unchanged (end `0x1ec000`) | unchanged (end `0x1ed000`) |

- **Stack:** no frame or whole path changes.
- **Image:** the loaded image is unchanged at page level. Defined text grows by 15 B (GCC 13) and 64 B (GCC 15) inside the same pages.
- **Performance:** `pmm_init_in` runs once per boot; the change is one comparison.

## Not tested

- Physical hardware: **NOT TESTED**.
- A native 64–192-frame instance: the kernel creates none. The small-instance proof is the hosted guard-page regression.
- `make test-uefi` in full.

## Evidence

`docs/results/r4-m1h-pmm-init/`: the gate summary (scope, negative control, regression, mutations, preserved R4-M1 gate), the worker summary, the kernel measurements, the genuine QEMU serial verdict excerpts, and `SHA256SUMS`. The complete receipts and retained outputs are in the maintainer ledger (campaign `r4-m1h-pmm-init-v1`).
