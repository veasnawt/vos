# N3 — Storage (Roadmap Proposal)

| | |
|---|---|
| Status | **N3.0 complete (2026-10-18): storage architecture designed, kernel map extended to 4 MiB of 4 KiB pages. N3.1 and later are [Proposal] and need explicit approval.** |
| Date | 2026-10-18 |
| Starting point | N2 complete at `8e30f9b` (`docs/plans/n2-foundation.md`, section 3.7) |
| Target | `ARCH=x86_64`; the i386 kernel stays as it is |
| Design | `docs/architecture/storage.md` (layers, device, format, layout, permissions, crash behaviour, decisions D-S1–D-S9) |

Labels: **[Proposal]** a recommendation awaiting approval; **[Open]** a question that needs a
decision.

---

## 1. Goal (bounded) [Proposal]

N3 ends when VOS can **read files from a read-only disk image through a validated stack**:
virtio-blk → block layer → ext2 reader → handle-relative file calls. Program data may then come
from the disk instead of GRUB modules. There is no persistent write in N3. Every structure read
from the disk is treated as untrusted input.

**Not in N3:**
- writes of any kind, and therefore no crash-consistency machinery beyond refusing unclean
  volumes;
- mounting more than one volume type;
- a user-space storage service;
- the GUI, networking and new language runtimes.

## 2. Stages [Proposal, after N3.0]

| Stage | Content | Acceptance (examples) |
|---|---|---|
| **N3.0** Storage architecture and memory preparation *(done)* | Design document. Second 4 KiB page table in the W^X kernel map (phys 0–4 MiB), with tests. | Section 3 |
| N3.1 PCI and virtio-blk, read-only, polled | Config-space scan (mechanism #1), modern virtio capability walk, feature negotiation, one split virtqueue in PMM frames, `IN` requests with a bounded poll, `VIRTIO_BLK_F_RO` honoured. Test images from `mke2fs -d` attached with `readonly=on`. | Sector reads match the host image; a write attempt is refused before it reaches the device; a missing device, a wrong ID or a timeout gives an error, never a hang; DMA pool audited |
| N3.2 Block layer | Block-device interface, validated block I/O (range, alignment, read-only), fixed-size cache in PMM frames, error mapping, an audit | Out-of-range and overflowing requests refused; cache bounded and frames accounted; device errors surface as errors |
| N3.3 ext2 reader | Restricted profile (`storage.md` section 5.3), every field validated, bounded walks | Valid images read exactly; a set of host-corrupted images (superblock, group descriptors, inode, block pointers, directory records, loops) rejected for the intended reason, without a panic |
| N3.4 Interrupt completion | INTx through the PIC, waiters on wait queues instead of polling | Other processes keep running during disk waits; a killed waiter leaves no stale request |
| N3.5 File ABI v2 (read-only) | Handle-relative open, read, stat, enumerate and close; the namespace from trusted configuration; the ABI note version 2 | v0/v1 programs unchanged (`hello.elf` byte-identical); rights and `..` escapes refused; concurrent readers |
| N3.6 Closure | Stress with concurrent readers, kills during I/O, exhaustion; documentation | Full regression; audits after every suite |

The order may change at review. Writes (D-S4) are a separate milestone after N3.

---

## 3. N3.0 result: architecture and memory preparation (complete)

### 3.1 Baseline (before any change)

At `8e30f9b` on `main` (clean tree, `origin` = `git@github.com:veasnawt/vos.git`), from a clean
build:
- `make test-all` exited 0 with 51 QEMU runs passing and none failing;
- all 10 fault comparisons matched;
- `test-user-elf`: 282 passed, 0 failed;
- i386 `vos.bin` `0760e7eb…afb9dd` and `hello.elf` `c6c64161…cd06`, as recorded at the end of
  N2.

### 3.2 Why the kernel map covered only 2 MiB

The W^X map (`kmap.c`, N2.0) points PDPT[510] at its own page directory:
- **PD[0]** led to the *only* 4 KiB page table, covering phys 0–2 MiB;
- **PD[1..511]** were 2 MiB pages, RW NX.

Per-page permissions (text R-X, read-only data R NX, unmapped guard pages) exist only inside a
4 KiB table. A kernel image extending past 2 MiB would have put text, read-only data or guard
pages into a 2 MiB RW NX page:
- text there could not execute;
- read-only data would be writable;
- guard pages would be mapped.

A linker assertion (`__kernel_phys_end <= 0x200000`) and a `kmap_init` check stopped such a
build. The limit came from that single table, not from GRUB, the boot tables or the PMM.

**What image growth affects:**

| Area | Finding |
|---|---|
| Bootloader | GRUB loads the ELF segments at LMA 1 MiB with no 2 MiB limit. It places the Multiboot2 information and modules after the image, so they move up with it. |
| Boot page tables | `boot.S` maps phys 0–1 GiB with 2 MiB pages (identity and higher half), so the 32-bit and early 64-bit paths reach any image below 1 GiB. |
| Reservations | `boot_reserve` reserves `[__kernel_phys_start, __kernel_phys_end)` generically. It refuses modules that overlap the image or the information block, or that lie below 1 MiB. Nothing assumes 2 MiB. |
| Page-table ownership | `kpd` and the kernel page tables are `.bss` arrays inside the reserved image: never PMM-owned, accepted by `vmm`'s `table()` check as in-image frames. |
| Higher-half mapping | PML4[511] and PDPT[510] are unchanged. Every process copies PML4[511] by value (`process.c`), so all CR3s share the one kernel directory and see any change to it at once. |
| VMM audit | Hard-coded "only PD[0] may be a table" and "leaves of PD[0] compare with `kmap_expected_pte(i1 << 12)`". These were generalised (section 3.5). |

### 3.3 Memory budget

| Image | `__kernel_phys_end` before | after N3.0 | Room below the 4 KiB-mapped limit |
|---|---|---|---|
| production `build/x86_64` | `0x18d000` | `0x18e000` | before 460 KiB (to 2 MiB), after 2504 KiB (to 4 MiB) |
| main test image `x86_64-test` | `0x1f2000` | `0x1fb000` | before 56 KiB, after 2068 KiB |
| largest test image (`wx-alias-exec`) | `0x1f4000` | `0x1fd000` | before 48 KiB, after 2060 KiB |

Production grows by 4 KiB (the second page table). The test images grow by 36 KiB, from the
new tests' static buffers and code.

The largest static objects in the test image:

| Object | Size |
|---|---|
| `stacks` (15 thread stacks of 16 KiB plus guards) | 300 KiB |
| `selftest_procs` | about 39 KiB |
| `muts.0` | about 33 KiB |
| `ebuf`, `alloc_snapshot`, `kernel_alloc`, `kernel_avail` | 32 KiB each |

**N3 needs** (estimates, `storage.md` section 11):
- code for PCI, virtio, the block layer and ext2;
- a few static tables;
- run-time frames from the PMM: the virtqueue, request headers and a fixed cache pool.

Only the code and static tables count against the image limit, which now has more than 2 MiB
to spare.

### 3.4 Mapping decision

| Option | Change | Gain | Cost and risk | Verdict |
|---|---|---|---|---|
| **A: second 4 KiB page table** | `kpt` becomes two contiguous tables (PD[0..1]), the 2 MiB pages start at PD[2]. Checks, audit and linker assertion generalised. | Image limit 2 → 4 MiB for code, data, stacks and guards alike | 4 KiB of `.bss`; the same permission model, checks and transition; phys 2–4 MiB now in 4 KiB instead of 2 MiB pages (a few more TLB entries) | **Chosen** |
| B: stacks in separately mapped memory | Allocate stack frames and map them into a new guarded kernel region | Removes 300 KiB of `.bss`; stacks no longer bound to the image | Needs upper-half mapping support in `vmm` (today it refuses the upper half by design), new audit rules for the region, and changes to thread creation: a memory-management restructure | Not now |
| C: move test-only buffers | Allocate `ebuf`, `muts`, `alloc_snapshot`, `selftest_procs` from the PMM at run time | About 140 KiB, test images only | Touches many tests; no gain for production | Not now |

### 3.5 What was implemented

**`kmap.h` and `kmap.c`:**
- Constants: `KMAP_SMALL_TABLES` = 2, `KMAP_SMALL_SPAN` = 4 MiB, `KMAP_LARGE_PAGES` = 510.
- `kpt` holds 1024 entries (two tables, contiguous).
- `kmap_expected_pte` covers phys 0–4 MiB. `kmap_expected_pde(i)` is a 2 MiB page only for
  i ≥ 2.
- `check_structure`:
  - requires PD[t] to point to exactly table t (`pt_pa + t * 4 KiB`, supervisor, writable,
    no NX);
  - requires PD[2..511] to be the 2 MiB pages;
  - checks all 1024 leaves.
- `translate` follows only the given table for each PD index.
- The running-kernel check now also requires every 4 KiB page from the image end to 4 MiB to
  translate to its own frame, supervisor, RW NX ("kernel alias above the image"). These are
  the frames the PMM hands out.
- Log line: `kmap: phys 0-4 MiB as 4 KiB pages: … 510 x 2 MiB RW NX`.

**`vmm.c` audit:**
- 4 KiB tables are allowed under PD[0..1] of the kernel directory and refused under PD[2..].
- Their leaves are compared with `kmap_expected_pte((i2 << 21) | (i1 << 12))`.

**`linker.ld`:** the assertion is now `__kernel_phys_end <= 0x400000`. `kmap_init` keeps its
own check.

**Comments and tests:**
- the `thread.h` comment;
- the boot-log marker in `arch.mk`, which checks the new log text;
- a new fault case `wx-small-alias-exec`.

Unchanged:
- the transition: build, pre-check, TLB priming, switch, flush, CPU probes, live check,
  rollback;
- W^X;
- guard pages;
- `vmm_map` rules;
- the PMM and its reservations;
- the boot path;
- ABI v0 and v1;
- every i386 file.

### 3.6 Tests added

**`selftest_kmap`** (49 → 78 cases):
- **Live layout:**
  - PD[0] and PD[1] point to the two tables, and PD[2] is a 2 MiB page;
  - every 4 KiB page of phys 0–4 MiB has its expected translation;
  - every page from the image end to 4 MiB is a 4 KiB, supervisor, RW NX page;
  - the audit counts 510 large pages and 1024 minus 20 guards leaves.
- **Real CPU access:** a free frame in [2 MiB, 4 MiB) above the image is allocated, written
  and read back through its 4 KiB alias, then zeroed and released.
- **Verifier on table copies** (the copies are now two contiguous static tables):
  - a writable executable page in the second table;
  - a read-only executable page;
  - a user page;
  - a missing page;
  - a page on the wrong frame;
  - PD[1] user-accessible;
  - PD[1] pointing to the first table;
  - PD[1] as the old 2 MiB page;
  - PD[2] shifted;
  - PD[2] as a table.
- **Running-check defects:**
  - the top page unmapped, read-only or executable;
  - PD[1] redirected to the first table.
- **Audit on a copy reached as a kernel half** (PML4[511] → PDPT[510] → copy):
  - the exact copy is accepted;
  - refused: W+X, read-only executable, user page, missing page, wrong frame, a second
    executable alias of the text, PD[1] as a 2 MiB page, a table under PD[2].

**Other self-tests:**
- **`selftest_vmm`:** phys 4 MiB − 4 KiB is a 4 KiB RW NX page; the 2 MiB pages start at
  4 MiB.
- **`selftest_pmm`:** the frame of the second page table is protected.
- **`selftest_user`:**
  - through a process root, every 4 KiB kernel page of phys 0–4 MiB matches the kernel root
    and none is user-accessible;
  - with the process's CR3 loaded, the kernel reads the same value at phys 4 MiB − 4 KiB;
  - a user-mode read of that alias faults with #PF P=1 U=1.
- **Fault case `wx-small-alias-exec`:** the kernel calls a frame in the new range (filled with
  `ret`) through its alias. The CPU raises #PF error 0x11 with RIP = CR2 = the alias, checked
  by `check-fault.sh`.

Existing coverage that now also includes the new range:
- process spaces are audited with the kernel half before they run (`verify_space`), and after
  every suite;
- the stress, IPC and multi-process images switch CR3 constantly;
- the NMI and IST tests run on the new map.

### 3.7 Mutations

Thirteen source mutations of the new mapping code. Each was applied to a clean tree, built
successfully, run in the main test image with exit-status and marker checks, and restored with
`git checkout`. None timed out, and no build failure was counted as a detection. Every
mutation was caught:

| # | Mutation | Caught by |
|---|---|---|
| M1 | Top page of the second table made R-X in the expected layout | `kmap_init` pre-check: "executable outside the kernel text"; boot panics |
| M2 | Only PD[0] built as a table | pre-check: "PD[1] does not point to the kernel page table" |
| M3 | `translate` follows PD[1] to any table | self-test "running check: only the given second table is followed" |
| M4 | Audit compares leaves of PD[0] only | self-test "audit refuses a missing page in the second table" |
| M5 | Audit allows a table under PD[2] | self-test "audit refuses a page table under PD[2]" |
| M6 | Audit uses the old leaf formula `i1 << 12` | boot audit fails; panic |
| M7 | Running check "kernel alias above the image" removed | self-test "the frames above the image must stay mapped" |
| M8 | `kmap_expected_pde(1)` a 2 MiB page | self-test "audit refuses the old layout (PD[1] a 2 MiB page)" |
| M9 | User bit on the second table's leaves | pre-check: "boot module: user-accessible"; boot panics |
| M10 | `check_structure` checks PD[0] only | self-test "verifier refuses a user-accessible second table" |
| M11 | `check_structure` checks the first table's leaves only | self-test "verifier refuses a writable, executable page in the second table" (only the running check still refused it, with a different diagnostic) |
| M12 | 2.5 MiB `.bss` array, linker assertion raised to 8 MiB | `kmap_init`: "kernel image: extends beyond the 4 KiB-page span"; boot panics |
| M13 | As M12 with the `kmap_init` size check also removed | pre-check: "stack guard page: reachable" (a guard now under a 2 MiB page) |

With only the 2.5 MiB array added (no other change), the link fails with the assertion message
"kernel image exceeds the 4 MiB covered by kmap.c". This is the intended detection, recorded
separately from the mutation count.

### 3.8 Regression

From clean builds of the final tree (`bbaff41`):

| Run | Result |
|---|---|
| Reproducibility | Two clean builds of the production x86_64 `vos.bin`: identical (`162f22e1…4395f`) |
| `make test` | exit 0; 51 QEMU runs passed, 0 failed; 11 fault comparisons matched; `test-user-elf` 284 passed, 0 failed |
| `make ARCH=x86_64 test` | identical to `make test` |
| `make ARCH=i386 test` | exit 0; smoke test passed |
| `make test-all` | exit 0; 52 runs passed, 0 failed; 11 fault comparisons; 284 passed, 0 failed |
| `git diff --check` | clean |
| i386 `vos.bin` | `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd` (unchanged) |
| `hello.elf` | `c6c6416195a0f18d680d7f8f19be52ce02ff02912ef3bac45324fbe88006cd06` (unchanged) |

Where the numbers changed:
- **Runs (+1):** the new fault case `wx-small-alias-exec`.
- **`test-user-elf` (282 → 284):** the static checks run once more, against the new case's
  kernel image.

The kernel-only commit (`b936b9d`, before the new tests) also passed the whole
`make ARCH=x86_64 arch-test` sequence on its own.

The suite covers the configurations and properties already established:
- the 41-case isolation suite under qemu64, 2 GiB and `-cpu max`;
- the IRQ, multi-process, IPC and stress images with their audits;
- the genuine NMI;
- all fault comparisons.

ABI v0 and v1 are unchanged (no file under `user/` or `docs/abi/` was touched).

### 3.9 Risks and limits

- Phys 2–4 MiB moved from one 2 MiB TLB entry to up to 512 4 KiB entries. This has not been
  measured, and there is no performance claim.
- The second table must stay physically contiguous with the first; `check_structure` enforces
  it. A future third table means changing one constant, but the verifier copies in
  `selftest_kmap` are static, so test images grow by 4 KiB per table.
- Nothing tests an image that actually exceeds 4 MiB. The linker assertion and the `kmap_init`
  check are the protection. Both were exercised by mutation (section 3.7).
- No storage code exists yet. Everything in `docs/architecture/storage.md` is a proposal.
- All evidence comes from QEMU TCG.

## 4. Decisions needed before N3.1

From `docs/architecture/storage.md` section 12:
- **D-S1:** the device (virtio-blk recommended);
- **D-S2:** the format (read-only ext2 profile recommended);
- **D-S3:** placement (in the kernel first);
- **D-S7:** the layout;
- **D-S8:** the file ABI as v2.

D-S4 (the write strategy) can wait until after N3. D-S5 and D-S6 (case sensitivity,
normalisation) are needed by N3.5.
