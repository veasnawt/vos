# N3 — Storage (Roadmap Proposal)

| | |
|---|---|
| Status | **N3.0 complete (2026-10-18): storage architecture designed, kernel map extended to 4 MiB of 4 KiB pages. N3.1 complete (2026-10-18): read-only, polled virtio-blk driver (section 5). N3.2 and later are [Proposal] and need explicit approval.** |
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
| **N3.1** PCI and virtio-blk, read-only, polled *(done)* | Config-space scan (mechanism #1), modern virtio capability walk, feature negotiation, one split virtqueue in PMM frames, `IN` requests with a bounded poll, `VIRTIO_BLK_F_RO` honoured. The test disk is a generated raw image, not an ext2 image (no ext2 code exists yet). | Section 5 |
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
- At the end of N3.0 no storage code existed and everything in `docs/architecture/storage.md`
  was a proposal (N3.1: section 5).
- All evidence comes from QEMU TCG.

## 4. Decisions (approved with N3.1, 2026-10-18)

| ID | Decision |
|---|---|
| D-S1 | virtio-blk with the modern PCI transport, polled at first |
| D-S2 | A restricted, read-only ext2 profile is the target of the first file system. No ext2 code in N3.1. |
| D-S3 | The first storage driver lives in the kernel, behind an internal block-device interface |
| D-S7 | Provisional lowercase system directories `/system`, `/applications`, `/users`, `/configuration`, `/temporary`; nothing in the block layer knows them |
| D-S8 | ABI v2 is reserved for file system calls; it is not defined or implemented yet |

Still open:
- D-S4 (write strategy), after N3;
- D-S5 and D-S6 (case sensitivity, normalisation), before N3.5;
- D-S9 (cache size and limits), in N3.2.

---

## 5. N3.1 result: read-only virtio-blk driver (complete)

### 5.1 Baseline

At `53b4c46` on `main` (clean tree), from a clean build, the N3.0 baseline reproduced exactly:
- `make test-all` exited 0 with 52 runs passing and none failing;
- 11 fault comparisons matched;
- `test-user-elf`: 284 passed, 0 failed;
- i386 `vos.bin` `0760e7eb…afb9dd`, `hello.elf` `c6c64161…cd06` and production x86_64
  `vos.bin` `162f22e1…4395f` were as at the end of N3.0.

### 5.2 Specifications used, and assumptions checked before coding

Sources:
- OASIS *Virtual I/O Device (VIRTIO) Version 1.2* (csd01), fetched 2026-10-18:
  - 2.1 device status;
  - 2.2 feature bits;
  - 2.7 split virtqueues: alignment, power-of-two sizes, "a suitable memory barrier before
    the idx update";
  - 3.1.1 the initialization sequence;
  - 3.3.1 reset;
  - 4.1.2 PCI IDs: drivers MUST match 0x1040–0x107f, MAY match the transitional IDs;
  - 4.1.4 capabilities: reserved BAR values and unknown types ignored, first instance of a
    type used, alignment;
  - 4.1.4.3 common configuration: the read-back of 0 after a reset;
  - 4.1.4.9 the PCI configuration access capability, which every modern device MUST present;
  - 4.1.5.1.3 virtqueue configuration;
  - 5.2 virtio-blk: 512-byte sectors, request layout and status values, and "a driver MUST
    NOT submit a request which would cause a read or write beyond capacity".
- The Linux UAPI headers on the host (`linux-libc-dev`: `virtio_pci.h`, `virtio_ring.h`,
  `virtio_blk.h`, `virtio_config.h`) for register offsets and bit numbers. Only constants were
  taken from them; no code was copied.

| Assumption found | Consequence |
|---|---|
| The kernel maps only phys 0–1 GiB. QEMU's firmware places virtio's 64-bit memory BAR elsewhere, and mapping MMIO would need new upper-half mappings, a memory-management change. | The driver reaches the common, notification and device structures through the **PCI configuration access capability** (4.1.4.9), a window in configuration space. No MMIO is mapped and the N3.0 kernel map is unchanged. Every window access is checked to lie inside a structure named by another capability, as the specification requires. |
| The kernel runs with interrupts off and has no device interrupts yet. | Completions are polled. Interrupts are suppressed twice: `VRING_AVAIL_F_NO_INTERRUPT`, and PCI INTx disabled in the command register. The PIC keeps every line but IRQ0 masked. |
| Reads beyond capacity are forbidden to drivers. | Device I/O errors are produced by QEMU's `blkdebug` driver at a chosen sector, not by out-of-range requests. |
| No IOMMU. The device sees guest-physical addresses. | Devices that require `VIRTIO_F_ACCESS_PLATFORM` are refused. DMA memory is a dedicated pool, which is not a security boundary (section 5.6). |
| QEMU's default `virtio-blk-pci` on the `pc` machine is transitional (ID 0x1001). | Transitional devices are accepted (allowed by 4.1.2) but always driven through the modern capabilities with `VIRTIO_F_VERSION_1` required. The legacy I/O-port interface is never touched. The tests use `disable-legacy=on` (ID 0x1042) and, separately, the default transitional device. |

### 5.3 Transport, features and capacities

| Item | Value |
|---|---|
| Discovery | PCI bus 0 only, configuration mechanism #1. At most 32 devices × 8 functions (functions 1–7 only for multi-function devices). Vendor 0x1AF4, device 0x1042, or 0x1001 with subsystem ID 2. The first match is used. |
| PCI setup | Bus master enabled, INTx disabled. I/O and memory decoding are left as the firmware set them. |
| Required capabilities | Common configuration (≥ 0x38 bytes, 4-aligned), notification (2-aligned, with multiplier), device configuration (≥ 8 bytes, 4-aligned), PCI configuration access. The capability walk is bounded (48 visits, loop detection, pointers within 0x40–0xFC). |
| Features | Required: `VIRTIO_F_VERSION_1`. Accepted if offered: `VIRTIO_BLK_F_RO`. Declined: all others, including `FLUSH`, `DISCARD`, `WRITE_ZEROES`, event index, indirect descriptors and packed rings. Refused device: `VIRTIO_F_ACCESS_PLATFORM` offered. |
| Initialization | Reset with a bounded wait for status 0; ACKNOWLEDGE; DRIVER; read the 64 feature bits; write the accepted subset and read the device's record of it back; FEATURES_OK and read it back; queue 0 set up; capacity read under `config_generation` (at most 8 tries); DRIVER_OK; then the status must read exactly 0x0F. On a failure after the reset, the FAILED bit is set and the device is reset. |
| Queue | Queue 0; the device's size must be a power of two ≥ 16 (32768 at most); the driver uses 16 entries. One 4 KiB frame holds the descriptors (offset 0), the available ring (256) and the used ring (1024). |
| Requests | 4 slots of 3 fixed descriptors (16-byte header, device-readable; data, device-writable; 1 status byte, device-writable). At most 8 sectors (4 KiB) per request. Type `VIRTIO_BLK_T_IN` only. |
| DMA pool | 6 frames from the PMM at initialization, kept for the device's lifetime: ring, headers and status bytes, and 4 data frames. |
| Completion | Polling the used ring, at most `VBLK_POLL_LIMIT` = 50,000,000 polls per request. This is an iteration bound, not a time bound. The longest successful wait observed in this milestone's runs was 6,097 polls. One full-length timeout cost roughly 10 s of guest time under QEMU TCG on this host, estimated from a mutation run with 7 timeouts that took 96 s (section 5.10). |
| Memory ordering | `mfence` after the descriptors and ring entry, before the `idx` store, and again before the notification. A fence after reading the used `idx`, before reading its entries. |

### 5.4 Interfaces

- **`blockdev.h`:**
  - `struct blockdev` holds name, capacity in sectors, `max_sectors`, `device_read_only` and a
    single operation, `read(lba, count, buf, cap)`. There is **no write operation**.
  - Error codes:

    | Code | Meaning |
    |---|---|
    | `BLK_ENODEV` | no usable device |
    | `BLK_EINVAL` | invalid request |
    | `BLK_ERANGE` | outside the device |
    | `BLK_EIO` | device I/O error |
    | `BLK_EUNSUPP` | device reported unsupported |
    | `BLK_ETIMEDOUT` | no completion in time |
    | `BLK_EBUSY` | all slots in use |
    | `BLK_EBROKEN` | device reset, needs reinitialization |
    | `BLK_EPROTO` | the device broke the queue protocol |

  - `blk_strerror` names them.
- **`virtio_blk.h`:**
  - `vblk_init`, `vblk_reinit`, `vblk_info`, `vblk_blockdev`;
  - `vblk_read` (synchronous);
  - `vblk_submit` / `vblk_wait`, up to 4 requests outstanding;
  - `vblk_audit`;
  - pure checks shared with the tests: `vpci_parse_caps`, `vblk_choose_features`,
    `vblk_choose_queue_size`, `vblk_dma_ok`, `vblk_window_ok`, `vblk_buffer_ok`.
- **`pci.h`:** 8/16/32-bit configuration reads and writes, and the bounded scan of bus 0.
- **Validation order for a read:**
  1. device state;
  2. 1–8 sectors;
  3. buffer capacity;
  4. buffer: every page present, writable, supervisor, and not a DMA frame (`vmm_lookup` on
     the kernel root);
  5. range: `lba < capacity` and `count <= capacity - lba`, which is overflow-safe;
  6. a free slot.

  A refused request never reaches the device or the buffer.
- `kernel_main` calls `vblk_init` once after the scheduler starts. An absent or refused device
  is logged, and the kernel continues without block storage.

### 5.5 Errors and timeouts

| Event | Result | Device afterwards |
|---|---|---|
| Status `VIRTIO_BLK_S_IOERR` | `BLK_EIO`, no data copied | ready |
| Status `VIRTIO_BLK_S_UNSUPP` | `BLK_EUNSUPP` | ready |
| Status byte unwritten or unknown | `BLK_EPROTO` | reset, broken |
| Used ring advances past the outstanding requests | `BLK_EPROTO` | reset, broken |
| Used entry naming a descriptor the driver does not own (out of range, not a chain head, a free slot, already completed) | `BLK_EPROTO` | reset, broken |
| Used length larger than the request's buffers, or not exactly data + status on success | `BLK_EPROTO` | reset, broken |
| No completion within the poll bound | `BLK_ETIMEDOUT` | reset, broken |
| Any request while broken | `BLK_EBROKEN` | broken until `vblk_reinit`, which resets and initializes again with the same pool |

After a reset, the specification guarantees that no virtqueue is live (3.3.1). The pool frames
are never freed while a device exists, so even a device that ignored the reset could only
write frames that belong to the driver.

### 5.6 DMA assumptions and limits

- VOS has **no IOMMU support**. A virtio device can read and write any guest-physical memory.
- The dedicated pool and the pre-publication check of every descriptor (`vblk_dma_ok`)
  constrain what the *driver* asks the device to do. They are **not a security boundary**
  against a faulty, compromised or malicious device, or against a hypervisor bug.
- DMA addresses are physical addresses below 1 GiB (PMM frames), handed to the device
  unchanged.
- The data is copied from the pool to the caller only after the completion and status are
  validated. The data frame is zeroed before each request.

### 5.7 Read-only guarantees and their boundaries

What is guaranteed, and how it was checked:
1. **No write path in the code.**
   - The block-device interface has no write operation.
   - The driver only ever builds `VIRTIO_BLK_T_IN` requests.
   - The only other type is a test-only hook (`VOS_TEST` builds) that refuses every type
     except one undefined, even request type, 0xFE, which carries no `OUT` bit. The tests
     show it refusing `OUT`, `FLUSH`, `DISCARD` and `WRITE_ZEROES`.
   - `FLUSH`, `DISCARD` and `WRITE_ZEROES` are never negotiated.
2. **Read-only at the device.** With `readonly=on`, QEMU offers `VIRTIO_BLK_F_RO`, which the
   driver accepts and reports (`device_read_only`). By the specification, the device then
   fails any write and writes no data.
3. **Read-only on the host.**
   - The test disk is generated into `build/vblk/` and checked against its known SHA-256.
   - Its file mode is read-only, and QEMU opens it read-only.
   - Its hash is checked again after all runs.
   - The writable-device run uses a copy, whose hash is checked afterwards.
   - No host disk is ever attached.

Boundaries:
- A successful read proves nothing about write protection. The writable-device run shows that
  the driver wrote nothing in that run, not that a write is impossible.
- Kernel code with a bug, or with malicious intent, can still write the ring. Then only the
  device-level read-only flag (if offered) and the host's read-only file stop a write.
- A device that does not offer `VIRTIO_BLK_F_RO` has no device-level barrier.

### 5.8 QEMU configurations

All runs use the default `pc` machine and are bounded by `exit-test.sh` timeouts (90 s for disk
runs). A timeout is never a pass.

| Run | Image | Device | Checks |
|---|---|---|---|
| `boot-128M` (and every other existing run) | main test image | none | "no virtio-blk device on PCI bus 0"; every request `BLK_ENODEV` |
| `vblk` | `vblk` | `virtio-blk-pci,disable-legacy=on`, `readonly=on` | ID 0x1042 (modern), 2051 sectors, queue 16 of 256, `VERSION_1 RO`; all live tests |
| `vblk-2G` | `vblk` | as `vblk`, 2 GiB RAM | all live tests |
| `vblk-max` | `vblk` | as `vblk`, `-cpu max` (SMEP, SMAP, UMIP) | all live tests |
| `vblk-transitional` | `vblk` | default `virtio-blk-pci` (ID 0x1001), `readonly=on` | driven as modern; all live tests |
| `vblk-writable` | `vblk` | writable copy, no `readonly` | `VERSION_1` without RO; all live tests; the copy's SHA-256 unchanged |
| `vblk-ioerr` | `vblk-ioerr` | `blkdebug` read error at sector 1000, `readonly=on` | I/O-error tests |
| `vblk-iommu-platform` | main test image | `iommu_platform=on` | refused: "device requires VIRTIO_F_ACCESS_PLATFORM"; kernel continues |
| `vblk-queue8` | main test image | `queue-size=8` | refused: "queue too small"; kernel continues |
| (after all runs) | — | — | `build/vblk/disk.img` still has SHA-256 `075bb733…4d0c77` |

**Test disk.** `scripts/mkvblkdisk.c` writes 2051 sectors (1,050,112 bytes). Each sector holds:
- "VOSBLK01";
- its own sector number;
- the total;
- 488 bytes of a seeded xorshift stream.

The output is a pure function of the size: two runs are identical. GRUB delivers the same file
as the module `vblk-fixture`. That path (the boot CD read by the firmware) is independent of
the driver, so every sector read through virtio-blk is compared with bytes the driver never
touched.

### 5.9 Tests

**Every test image** runs `selftest_vblk`'s synthetic part:
- capability parser on 17 hand-built configuration spaces (loops, bad pointers, reserved BARs
  and unknown types ignored, missing or short structures, misalignment, overflow, first
  instance wins);
- transport window bounds;
- feature choice (VERSION_1 required, only RO accepted, ACCESS_PLATFORM refused);
- queue-size choice (0, non-powers of two and too small refused);
- buffer checks (NULL, empty, user address, read-only data, guard page, wrapping).

These images also require "no usable device", with every request answered by `BLK_ENODEV`.

**The `vblk` image** (91 cases) adds:
- **Device:** ID, negotiated features, status 0x0F, queue size, capacity = fixture = 2051,
  6 pool frames allocated, audit clean, DMA-range checks, the pool refused as a buffer.
- **Contents:**
  - sectors 0, 1 and 2050;
  - requests ending exactly at the capacity, and across sector 1024;
  - **every sector** in chunks of 1–8 compared with the fixture and with its own header.
- **Validation:** past-the-end, running-past-the-end and wrapping ranges refused; lengths 0
  and 9; small capacity; NULL, user, read-only, guard-page, DMA-pool and wrapping buffers. All
  of this without a request reaching the device or the buffer, and with invalid slots in
  `vblk_wait` refused.
- **Queue:** 4 requests outstanding; a fifth gets `BLK_EBUSY`; completions collected in
  another order than submitted (2, 0, 3, 1) with exact data; the queue usable afterwards with
  no frame used.
- **Unsupported:** the hook refuses OUT, FLUSH, DISCARD and WRITE_ZEROES. Type 0xFE completes
  with `BLK_EUNSUPP` and the device keeps working.
- **Timeout:** an un-notified request times out after exactly the configured 20,000 polls. The
  device is then reset and refuses requests until `vblk_reinit`, which recovers it without new
  frames.
- **Protocol:** six forged used-ring entries, each refused with its own diagnostic, each
  followed by a recovery:
  - surplus completions;
  - a free slot's descriptor;
  - a non-head descriptor;
  - an oversized length;
  - an unwritten status;
  - zero length.
- **Repetition:** 3000 pseudo-random reads of 1–8 sectors, all exact; no frame, slot or
  descriptor leaked.
- **Processes:** a kernel thread reads 200 × 4 sectors, yielding between reads, while memtag1,
  memtag2 and irqregs run under timer preemption. All reads are exact. The programs kept
  running and being preempted, and they share no frame with each other or with the DMA pool.
  The timer kept ticking, everything returned to baseline, and the scheduler, process,
  channel and vblk audits are clean.

**The `vblk-ioerr` image** (58 cases): the injected error is reported as `BLK_EIO`:
- for sector 1000;
- for multi-sector requests covering it;
- in the full scan, for the chunk that covers it;
- 50 more times in a row.

The neighbouring sectors read exactly, the device is never reset for an I/O error, and nothing
leaks.

**Changes to existing tests:**
- **`selftest_pmm`:** the whole-bitmap oracle comparison now treats the virtio-blk pool frames
  as legitimately allocated before the test. They must be `ALLOCATED`; every other frame must
  match as before.
- **`app-reject`:** feeds the kernel's own ELF to the loader as "hello". With the new code and
  its debug sections, the test kernel file grew past the loader's 1 MiB limit (1,122,840
  bytes), which changed the refusal reason. The module is now `objcopy --strip-debug` of the
  same kernel, with the same program headers and contents (285,064 bytes). The test's expected
  diagnostic ("PT_LOAD outside [0x400000, 0x40000000)") is unchanged.
- **`boot-128M`:** now also requires the "no virtio-blk device" line and the vblk self-test.

### 5.10 Mutations

Twenty-one mutations of `virtio_blk.c`. Each was applied to the clean tree at `d9c07a4`, built
successfully, run in the named QEMU configurations (exit status and markers checked, 90 s
limit) and restored with `git checkout`.
- A build failure or a timeout is never counted as a detection.
- 18 mutations were caught by concrete failing assertions.
- 2 were not detected.
- 1 is not counted (it can only hang).

| # | Mutation | Runs | Result |
|---|---|---|---|
| V1 | Sector bound off by one (`lba > capacity`, `count > capacity - lba + 1`) | vblk | caught: "range: the first sector past the end … refused", "refused requests never reached the device" |
| V2 | Used-entry ownership check removed (free or completed slots accepted) | vblk | caught: "completion naming a free slot: refused (descriptor ownership)" |
| V3 | Chain-head check removed | vblk | caught: "completion naming a non-head descriptor: refused" |
| V4 | Power-of-two queue check removed | main | caught: "queue: refuses sizes that are not powers of two" |
| V5 | "Queue too small" check removed | main, main + `queue-size=8` | caught by both: the unit check, and the refused-device expectations |
| V6 | `VIRTIO_BLK_S_IOERR` not mapped to `BLK_EIO` | vblk-ioerr | caught: "the injected media error at sector 1000 is reported as BLK_EIO" |
| V7 | Request status ignored entirely | vblk, vblk-ioerr | caught by both: UNSUPP, unwritten status, I/O error |
| V8 | Poll bound not honoured (64 × the limit) | vblk | caught: "times out after exactly the poll bound" |
| V9 | DMA range check weakened (`<= PAGE_SIZE` instead of `<= PAGE_SIZE - len`) | vblk | caught: "DMA check: inside one pool frame only" |
| V10 | Data descriptor one page past its frame (range check kept) | vblk | at 90 s: assertions failed, then the run timed out (not counted). Rerun with a 500 s limit: finished in 96 s, `VOS-TEST: FAIL`, 31 failed assertions (wrong data, final audit) — **caught**. The pool frames are physically adjacent, so the shifted address usually lies inside the *neighbouring* slot's frame: the range check cannot see that, only the data comparison can. |
| V11 | As V10 with the pre-publication DMA check also removed | vblk | caught: sector contents differ from the fixture |
| V12 | Driver features never written to the device | vblk | caught: the device's record of the driver features differs, so the device is refused ("virtio-blk device ready" fails) |
| V13 | Every offered feature accepted | main, vblk | caught by both: "accepts only VERSION_1 and RO", "negotiated exactly VERSION_1 and, if offered, RO" |
| V14 | `VIRTIO_F_ACCESS_PLATFORM` accepted | main, main + `iommu_platform=on` | caught by both |
| V15 | FEATURES_OK not read back | main, vblk | **missed**: no QEMU configuration clears FEATURES_OK for VERSION_1 + RO, so the check is never exercised (section 5.13) |
| V16 | Buffer validation removed from `vblk_read` | vblk | caught: "buffer: capacity below the request, or NULL, refused" |
| V17 | Status byte not preset to the "unwritten" marker | vblk | caught: "completion without a written status byte: refused" |
| V18 | No device reset after a timeout or protocol error | vblk | caught: "after the timeout the device is reset (status 0)" |
| V19 | Visited bitmap of the capability walk removed | main | **missed, equivalent**: the 48-visit bound still detects every loop with the same diagnostic |
| V21 | Capability visit bound lowered to 4 | main | caught: the valid 5-capability list is refused |
| V20 | Poll loop unbounded | vblk | **not counted**: it hangs and only the harness timeout ends it. V8 is the assertion-detectable form of this defect. |

### 5.11 Regression

From clean builds of the final tree (the state committed as `d9c07a4`; the documentation
commit changes no code):

| Run | Result |
|---|---|
| Reproducibility | Two clean builds of the production x86_64 `vos.bin`: identical (`efb1ddcc…cb57c`) |
| `make test` | exit 0; 59 QEMU runs passed (51 before + 8 virtio-blk runs), 0 failed; 11 fault comparisons; `test-user-elf` 288 passed, 0 failed (+4 for the two new test kernels) |
| `make ARCH=x86_64 test` | identical to `make test` |
| `make ARCH=i386 test` | exit 0; smoke test passed |
| `make test-all` | exit 0; 60 runs passed, 0 failed; 11 fault comparisons; 288 passed, 0 failed |
| `git diff --check` | clean |
| i386 `vos.bin` | `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd` (unchanged) |
| `hello.elf` | `c6c6416195a0f18d680d7f8f19be52ce02ff02912ef3bac45324fbe88006cd06` (unchanged) |
| Test disk | `075bb733…4d0c77` after every run. The writable copy was unchanged too. |

The driver commit alone (`d3b22dd`, before the tests) passed `make ARCH=x86_64 test` in a
separate worktree: 51 runs, 11 fault comparisons, 284 user/ELF checks.

Also covered, unchanged:
- **N2:** process isolation, IPC and stress (the `mp`, `ipc` and `stress` images);
- **N3.0:** the 4 MiB kernel map;
- W^X and guard-page cases;
- the 41-case isolation suite under qemu64, 2 GiB and `-cpu max`;
- the genuine NMI.

ABI v0 and v1 are unchanged: no file under `user/` or `docs/abi/` changed, and `hello.elf` is
byte-identical.

### 5.12 Memory headroom

`__kernel_phys_end` (limit of the 4 KiB-mapped area: `0x400000`):

| Image | N3.0 | N3.1 | Room left |
|---|---|---|---|
| production `build/x86_64` | `0x18e000` | `0x192000` (+16 KiB) | 2488 KiB |
| main test image | `0x1fb000` | `0x203000` | 2036 KiB |
| `vblk` test image | — | `0x209000` (largest) | 2012 KiB |
| `vblk-ioerr` test image | — | `0x206000` | 2024 KiB |

The main test image now ends above 2 MiB, so it would no longer have fit the pre-N3.0 map.

At run time, the driver holds 6 PMM frames (24 KiB) while a device is present (also when a
device was refused after the pool was allocated). The kernel image adds no new page tables and
no MMIO mapping.

### 5.13 Remaining risks and limits

- **No IOMMU** (section 5.6). A malicious or faulty device can write any guest memory.
- **Polling with interrupts off.** While a request waits, nothing else runs, user processes
  included. The bound is a poll count, not wall time: a hung device stalls the system for up to
  50,000,000 polls before the reset. N3.4 replaces this with interrupt completion.
- **One device and one queue.** Only PCI bus 0 is scanned, and no bridges are followed.
- **Untested window variants.** The configuration-access window is slow (several port
  accesses per register). It has only been tested against QEMU 10.2.1's implementation.
- **FEATURES_OK refusal untested live.** The path where the device clears `FEATURES_OK` is not
  exercised: no QEMU configuration refuses VERSION_1 + RO (mutation V15, section 5.10).
- **Slow full timeouts.** A full-length timeout (50,000,000 polls) took roughly 10 s of guest
  time under TCG. A misbehaving device can stall the system that long per request.
- **No interrupt or MSI-X path.** The device's interrupt behaviour is suppressed, not tested.
- **Pool held by a refused device.** A device refused after the pool was allocated keeps its 6
  frames. This is bounded and accounted for by the PMM test.
- **Read-only boundaries** as in section 5.7.
- **QEMU only.** All evidence comes from QEMU TCG: no KVM, no real hardware.

## 6. Recommended N3.2 scope [Proposal]

The block layer between the driver and a future file system:
- a block-device registry, which can hold one device at first;
- requests in file-system blocks (1 KiB or 4 KiB) mapped to sectors, with alignment checks;
- a fixed-size block cache in PMM frames, with its size chosen from the measured budget
  (D-S9). It is read-only, has an explicit eviction policy and an audit, and caches no failed
  read;
- device errors mapped to file-system-level errors;
- a per-caller limit on outstanding requests.

Tests in N3.2:
- cache hits and misses against the fixture;
- eviction;
- exhaustion;
- I/O errors passing through uncached;
- an audit after every suite.

Still not in N3.2: ext2 parsing (N3.3), interrupts (N3.4), file system calls (N3.5) and any
write.
