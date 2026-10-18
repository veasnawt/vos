# N3 — Storage (Roadmap Proposal)

| | |
|---|---|
| Status | **N3.0 complete (2026-10-18): storage architecture designed, kernel map extended to 4 MiB of 4 KiB pages. N3.1 complete (2026-10-18): read-only, polled virtio-blk driver (section 5). N3.2 complete (2026-10-18): block layer, read-only cache, DMA-frame lifecycle, cooperative waiting (section 6). N3.3 and later are [Proposal] and need explicit approval.** |
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
| **N3.2** Block layer *(done)* | Block-device registry and interface, validated block I/O, fixed-size read-only cache in PMM frames, device-independent errors, audits; DMA-frame release for refused and stopped devices; cooperative waiting in the driver | Section 6 |
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

## 6. N3.2 result: block I/O layer and read-only cache (complete)

### 6.1 Baseline and starting point

At `ebfb672` on `main` (clean tree), from a clean build, the N3.1 baseline reproduced:
- `make test-all` exited 0 with 60 runs passing and none failing;
- 11 fault comparisons matched;
- `test-user-elf`: 288 passed, 0 failed;
- the i386 `vos.bin`, `hello.elf`, production x86_64 `vos.bin` (`efb1ddcc…cb57c`) and test
  disk hashes were as at the end of N3.1.

The longest single wait in that run was 9,915 polls, against 6,097 recorded in N3.1. This is
run-to-run variance under TCG, still four orders of magnitude below the bound.

**Where the layer attaches.** N3.1 had a `struct blockdev` with one `read` pointer, published
by the driver, and error codes shared between driver and callers. N3.2 replaces it with:
- a registry and an operations table, so the block layer sees no virtio structure;
- driver-private result codes, renamed `VBLK_E*`. The names `BLK_EIO`, `BLK_EPROTO`, … in
  section 5 are now `VBLK_EIO`, `VBLK_EPROTO`, … The block layer maps them to its own codes
  (section 6.4).

### 6.2 Block-device interface and registry (`blockdev.h`, `blockdev.c`)

- **Registry:** at most `BLK_MAX_DEVICES` = 4 devices.
  - `blk_register(name, ops, ctx, &id)` checks the operations (`info` and `read` required),
    and that the device reports a non-zero capacity, the read capability and requests of at
    least one cache line.
  - `blk_unregister(id)` is refused (`BLK_EBUSY`) while a line of the device is loading or
    pinned.
  - `blk_count`, `blk_nth` and `blk_describe` report name, driver, state, capacity, sector
    size (512), largest request (64 sectors), capabilities (`BLK_CAP_READ`,
    `BLK_CAP_DEVICE_RO`) and epoch.
- **IDs:** registry slot + 1 in the low 4 bits, and a 28-bit generation above it. A slot's
  generation changes when it is released, so an ID never names a later device. ID 0, unused
  slots, slots ≥ 5 and wrong generations are all `BLK_ENODEV`.
- **Driver operations** (`struct blk_ops`):
  - `info`: state `READY`/`UNAVAILABLE`, sectors, largest request, capabilities, epoch;
  - `read`;
  - `owns_frame`: driver-private frames, never accepted as caller buffers.

  The driver reports its state on every request, so failure, reset and shutdown are explicit
  and never cached by the layer.
- **Requests:**
  - `blk_read(id, lba, count, buf, cap)`: 1–64 sectors;
  - `blk_read_block(id, size, block, buf, cap)`: file-system blocks of 512, 1024, 2048 or
    4096 bytes, with overflow-checked block-to-sector arithmetic;
  - `blk_write(…)`: always `BLK_EUNSUPP`, an explicit refusal (`BLK_ENODEV` for an invalid
    ID).
- **Validation order:**
  1. ID;
  2. device state;
  3. count and capacity of the buffer;
  4. range: `lba < sectors` and `count <= sectors - lba`, which is overflow-safe;
  5. buffer: every page present, writable, supervisor, and neither a cache frame nor a driver
     frame.

  A refused request reaches neither the device nor the buffer. No alignment is required of
  buffers, because data is always copied.
- **Audits:** `blk_audit()` checks registry counts, cache ownership, frame states, line keys,
  capacities and checksums. `blk_cache_frames()` lists the cache frames for the PMM oracle.

### 6.3 Cache

| Property | Value |
|---|---|
| Size | `BLK_CACHE_LINES` = 32 lines of `BLK_LINE_SECTORS` = 8 sectors, one 4 KiB PMM frame per line: **128 KiB**. Allocated when the first device registers, freed when the last unregisters. About 0.4 % of the frames of a 128 MiB guest (31,933 free frames at the time of the PMM test). |
| Line | Line *n* of a device holds sectors [8*n*, 8*n* + 8) clipped to the capacity. The 2051-sector test disk's last line (256) holds 3 sectors (2048–2050); the RAM test device's (128) holds 3 sectors (1024–1026). A load reads exactly the line's sectors, never past the capacity. |
| Key | Registry slot, its generation, the driver's epoch and the line number |
| States | `FREE`, `LOADING` (one thread is filling it: never returned, never evicted), `VALID` |
| Replacement | A free line first, else the least recently used valid line that is not pinned (a use counter per access; ties to the lowest index). If every line is loading or pinned: `BLK_EBUSY`. |
| Integrity | A 64-bit FNV-1a checksum over the line's sectors at load, checked on every hit. A mismatch drops the line and reloads it, at most 8 times per request. |
| Hits | Copy from the line into the caller's buffer. No pointer to cache storage is ever handed out, so eviction can never invalidate a caller's data. |
| Failed loads | The line goes back to `FREE`, so failed reads never create valid entries. After `BLK_EIO` the requested sectors are read one by one, uncached, into the caller's buffer, and only sectors that really fail return `BLK_EIO`. Other errors (`BLK_ETIMEDOUT`, `BLK_EUNAVAIL`, …) are returned as they are. |
| Reset during a load | If the epoch changed or the device became unavailable while a line loaded, the data is discarded (`BLK_EUNAVAIL`). |
| Concurrency | The kernel is not preemptible and runs with IF = 0, so every state change between explicit waits is atomic. A thread that needs a line another thread is loading blocks on a wait queue until that load ends. It waits at most 1024 times per request, then gets `BLK_EBUSY`. A woken thread re-checks that its device is still registered. |
| Pins | Test hooks only (`blk_test_pin`). They model future holders such as a file system's metadata and are used to test exhaustion. |

**Future 1, 2 and 4 KiB file-system blocks.** `blk_read_block` maps a block to
`size / 512` sectors at `block * size / 512` (overflow-checked). A 4 KiB block whose number is
aligned is exactly one cache line. A 1 or 2 KiB block lies within one line. Blocks crossing
the capacity are refused with `BLK_ERANGE`; on the test disk that is the 4 KiB block 256.

### 6.4 Errors and resource ownership

| Block error | Meaning | From the virtio driver |
|---|---|---|
| `BLK_EINVAL` | malformed request (count, block size, buffer, capacity) | `VBLK_EINVAL` |
| `BLK_ERANGE` | sectors outside the device | `VBLK_ERANGE` |
| `BLK_ENODEV` | no registered device with this ID | — |
| `BLK_EUNAVAIL` | the device exists but cannot serve requests | `VBLK_ENODEV`, `VBLK_EBROKEN` |
| `BLK_EBUSY` | a bounded resource is exhausted (slots, cache lines, registry, waits) | `VBLK_EBUSY` |
| `BLK_EIO` | the device failed the transfer or misbehaved | `VBLK_EIO`, `VBLK_EPROTO` (the device is then also unavailable) |
| `BLK_ETIMEDOUT` | no completion in time (the device was reset) | `VBLK_ETIMEDOUT` |
| `BLK_EUNSUPP` | unsupported operation (every write) | `VBLK_EUNSUPP` |

No virtio meaning crosses the boundary. A protocol violation is simply an I/O error followed
by an unavailable device.

**Ownership:**

| Resource | Owner | Released |
|---|---|---|
| 32 cache frames | the block layer, while ≥ 1 device is registered | when the last device unregisters |
| 6 DMA frames | the virtio driver, while the device may use them | after a confirmed reset with bus mastering off (refusal, shutdown) |
| Caller buffers | the caller | always; written only after validation, and only with data whose load succeeded |

### 6.5 DMA-frame lifecycle

**Finding.** In N3.1 a device refused during initialization kept its 6 frames. `give_up()`
reset the device but never freed the pool, so the frames were retained without need: after a
confirmed reset no virtqueue is live (virtio 1.2, 3.3.1). This was an unnecessary hold, not a
safety reservation.

**N3.2 rule.** The pool is freed only when the device can no longer write it:
1. a reset the device confirms (status reads 0);
2. **then** PCI bus mastering switched off;
3. then the frames freed.

If the reset is not confirmed, the frames stay allocated and are marked *quarantined*, because
the device might still use an earlier queue.

| Event | Device state | DMA pool | Block layer |
|---|---|---|---|
| Refused during initialization (feature, queue, transport, configuration) | `FAILED` | freed after the confirmed reset; quarantined otherwise | not registered |
| Timeout or protocol violation | `BROKEN` (reset) | kept for `vblk_reinit` | registered, `UNAVAILABLE`; every request `BLK_EUNAVAIL`, cached lines included |
| `vblk_reinit` | `READY` (or `FAILED`) | reallocated if it was freed; bus mastering on again | registered (or re-registered with a new ID); the epoch changes, so every old line is stale |
| `vblk_shutdown` | `STOPPED` | freed after the confirmed reset | unregistered: the ID dies, and the cache is freed when no device is left |
| Shutdown with an unconfirmed reset | `BROKEN` | quarantined | unregistered |
| Requests outstanding | — | — | `vblk_reinit` does nothing and `vblk_shutdown` returns `VBLK_EBUSY`. A frame in use is never released. |

Checks:
- `vblk_audit` checks the pool bookkeeping (allocated, listed, quarantined) and the
  registration against the state.
- The PMM oracle comparison treats exactly the pool (while held) and the cache frames as
  legitimately allocated. Every other frame must match the memory map, so a leaked frame fails
  it.

### 6.6 Reset and invalidation

- The virtio device's epoch is its number of successful initializations, so every
  `vblk_reinit` makes all of its cached lines stale.
- A stale line is dropped when it is next seen (`stale_drops`). It is never returned.
- While the device is `BROKEN`, the layer refuses even requests whose lines are cached
  (`BLK_EUNAVAIL`). Cached data is not served from a device in an unknown state.
- `blk_invalidate(id)` drops a device's lines explicitly (`id` 0: all devices). It is refused
  while a line is loading or pinned.

### 6.7 Polling and responsiveness

The five questions asked for N3.2:

1. **Why interrupts must stay disabled.** The kernel is not preemptible. All kernel code runs
   with IF = 0, except the idle thread's `sti; hlt`. IRQs are taken only in ring 3 or in idle,
   and preemption happens only on the return to ring 3 (N2.1–N2.3). Enabling interrupts while
   a driver polls would deliver IRQ0 in ordinary kernel code, which the design excludes.
   Besides, the device's own interrupt is not connected: INTx is disabled, the PIC line is
   masked and MSI-X is not set up.
2. **Can a cache miss monopolize the CPU?** In N3.1, yes. A miss polled with IF = 0 until
   completion, and a hung device held the CPU for the whole bound (about 10 s under TCG at
   50,000,000 polls). Measured in N3.2 with the old behaviour (yielding disabled): a CPU-bound
   program got **0 ticks** during a hung request. Hits never touch the device.
3. **Can a waiting request yield or sleep?** It can yield. `thread_yield` is the cooperative
   switch the scheduler already provides, and a thread may block or yield inside kernel code,
   as `channel_recv` does. Sleeping (`thread_sleep`) would cost at least one tick (10 ms) per
   request even when the device answers in microseconds, so it is not used.
4. **What does virtio offer?** Used-buffer notifications by interrupt (INTx or MSI-X), with
   suppression flags, or polling of the used ring. The safe incremental step, without
   interrupts, is polling that yields.
5. **What interrupt-driven I/O needs (N3.4):**
   - routing the device's INTx line (configuration offset 0x3C) through the 8259, or MSI-X;
   - an IRQ handler that reads the ISR to acknowledge and wakes the waiters on a wait queue;
   - waiters that block (`thread_block`) instead of polling.

   This fits the existing model, because IRQs arrive when the CPU is in ring 3 or idle. When
   no thread is runnable, idle's `sti; hlt` takes the device interrupt. N3.4 also needs a
   per-request timeout from timer ticks, and a rule for a process killed while its thread
   waits.

**Implemented (narrow, no architectural change).** `vblk_wait` now yields every
`VBLK_YIELD_POLLS` = 4096 polls. Interrupts stay disabled in the kernel, and the kernel stays
non-preemptible.
- After each yield the waiter re-checks the device state, the epoch, its slot and the slot's
  owner (the thread that submitted it). Another thread may have completed the slot in its own
  `reap`, or reset the device. A reset ends the wait with `VBLK_EBROKEN` and the slot is never
  touched again.
- A second bound, `VBLK_WAIT_TICKS` = 200 timer ticks, ends a wait once other threads have run
  (ticks advance only while ring 3 or idle runs). Without it, a hung device would cost a time
  slice per yield, about 12,000 slices for the poll bound.
- Requests complete typically within 300–10,000 polls, so most never yield.

**Measured** in the `vblk` run (a request the device never sees, beside the CPU-bound program
memtag1):
- without yielding, the program ran 0 of 0 ticks;
- with yielding every 256 polls and a 30-tick bound, it ran 30 of 30
  ticks (15 yields), and the wait ended at the tick bound.

**What this does not solve.** Between yields the CPU is still held for up to 4096 polls with
interrupts off. A finite polling bound alone does not make the system responsive: with nothing
else runnable the waiter keeps polling, and with a single CPU-bound program each yield costs
the waiter a full time slice. Interrupt-driven completion (N3.4) is the real fix.

### 6.8 Performance measurements (QEMU TCG; relative only)

| Measurement | Value |
|---|---|
| 32 one-sector misses vs. the same 32 as hits (TSC cycles per read) | miss 687,269, hit 22,594: a hit costs about 1/30 of a miss |
| Full scan of the 2051-sector disk through the cache (requests of 1–64 sectors) | 257 device reads: each line exactly once (256 full + the 3-sector line); 225 evictions; every sector equal to the fixture |
| Re-reading the cached tail | 0 device requests |
| Longest single device wait in the final `vblk` run | 4,096 polls |
| Block-layer counters at the end of the `vblk` run | 605 requests, 211 hits, 1,031 misses, 1,030 device reads (one miss was refused with `BLK_EBUSY` before reading), 811 evictions, 7 load errors, 7 fallback reads, 36 stale drops, 1 corrupt drop, 3 load waits, 1 busy, 47 refused (RAM test device and virtio-blk together) |

### 6.9 Hardware configurations

| Configuration | Status |
|---|---|
| QEMU `pc`, virtio-blk-pci modern (0x1042), read-only | supported, tested (128 MiB, 2 GiB, `-cpu max`) |
| QEMU virtio-blk-pci transitional (0x1001), modern interface | supported, tested |
| Writable virtio-blk backing | supported read-only, tested; the image hash is unchanged afterwards |
| Device requiring `VIRTIO_F_ACCESS_PLATFORM`; queue smaller than 16 | refused, tested; the pool is released |
| Devices on PCI buses other than 0, behind bridges; more than one virtio-blk device; MSI-X; interrupts | not supported |
| AHCI, NVMe, IDE disks, real hardware, KVM | not supported, not tested |

### 6.10 Tests

**`selftest_blk`, every test image (55 cases)**, with a test-only RAM device of 1027 sectors.
Its contents are a function of the sector and a per-device salt, and its reads can fail, yield
or change the epoch on request.
- **Registry:**
  - refusal of bad operations, zero capacity and too-small requests;
  - identity and description;
  - zero, wrong-generation, out-of-range and unused IDs give `BLK_ENODEV` for every
    operation;
  - 4 devices, then `BLK_EBUSY`;
  - dead IDs after unregistration, and a new generation for a reused slot;
  - cache frames allocated with the first device and freed with the last.
- **Validation:** 20 refusals (counts, capacities, NULL, user, read-only, guard-page,
  cache-frame and wrapping buffers, past-the-end and wrapping ranges, bad block sizes,
  overflowing block numbers, partial blocks, writes, an unavailable device), none reaching
  the device or the buffer.
- **Contents and counters:**
  - first sector (one miss, one 8-sector load);
  - hit in the same line;
  - a request across a line boundary (one hit, one miss);
  - the final partial line loaded with exactly 3 sectors;
  - 64-sector requests at an unaligned start and ending at the capacity;
  - 512/1024/2048/4096-byte blocks;
  - 20 identical repeated reads;
  - the device was never asked for an impossible range.
- **LRU:** 32 misses fill the cache without eviction. A touched line survives. The 33rd line
  evicts the least recently used one, which reloads from the device.
- **Exhaustion:** all 32 lines pinned gives `BLK_EBUSY` with no device read. Invalidation and
  unregistration are refused, hits are still served, and unpinning one line recovers.
- **Failures:**
  - an I/O error on a miss gives `BLK_EIO` with the line not cached;
  - the other sectors of the failing line are served one by one, uncached;
  - covering requests fail;
  - a timeout is not retried sector by sector;
  - an unavailable device is reported;
  - after recovery the line caches;
  - after an epoch change the cached line is dropped and reloaded;
  - a reset during a load discards the data;
  - a corrupted cache line is detected by its checksum and reloaded.
- **Concurrency:** two threads need the same line at once. One loads it, the other waits on
  the wait queue and then hits, and the device reads it once.
- **Two devices:** the same line numbers stay apart (separate misses, each device's own data).
  Invalidating or unregistering one leaves the other's lines.
- **Cycles:** 20 register/read/unregister cycles with no frame leaked.

**`selftest_blk` on the virtio-blk device** (`vblk` image, 73 cases in total):
- registration and description;
- a full scan through the cache equal to the fixture, with exact miss, device-read and
  eviction counts;
- the cached tail served without the device;
- 4 KiB and 1 KiB blocks equal to the fixture, with the blocks crossing sector 2051 refused;
- reinitialization invalidates;
- a timeout on a miss gives `BLK_ETIMEDOUT`, then `BLK_EUNAVAIL` even for cached sectors,
  then recovery without trusting old lines;
- 5 shutdown/reinitialization cycles, each returning and retaking 6 DMA + 32 cache frames
  with new IDs and exact data;
- hit/miss timing;
- two reader threads doing 300 random reads through the cache beside three running programs,
  with no shared frames and clean audits.

**`selftest_blk` on the `vblk-ioerr` image:** the injected error fails line 125's load. Sectors
1001–1007 are served uncached, line 124 caches normally, and covering requests and the 4 KiB
block fail.

**`selftest_vblk` additions** (now 97 cases on `vblk`):
- driver-to-block error mapping;
- registration state;
- **reset while waiting:** a thread that yielded during its request returns `VBLK_EBROKEN`
  when another thread's protocol error resets the device;
- **responsiveness:** 0 of 0 ticks without yielding versus most of the 30-tick bound with
  yielding;
- **refused devices** (`vblk-iommu-platform`, `vblk-queue8`): the pool is freed after the
  confirmed reset, also over 10 retries, with status 0 and bus mastering off.

**Harness:** every run that requires the vblk self-test line now also requires
`selftest: blk`. The `vblk` run also requires the shutdown line and the 0-tick measurement.

**Changed existing test (N3.0 `selftest_kmap`).** The verifier test removed the *last* page of
the second page table and expected "kernel alias above the image". With N3.2 the test kernel
grew, so GRUB placed the 1 MiB fixture module across that page in the `vblk` image. The
verifier then correctly named the module first. The test now picks the highest page of the
second table above the image that holds no module or boot information. The assertion and its
expected diagnostic are unchanged.

### 6.11 Mutations

Twenty-three mutations of `blockdev.c` and `virtio_blk.c`. Each was applied to the clean tree
at `8430280`, built successfully, run in the named configurations (exit status and markers
checked, 120 s limit) and restored with `git checkout`.
- A build failure or a timeout is never counted as a detection; none occurred.
- **22 were caught** by concrete failing assertions.
- **1 was not detected** (equivalent).

| # | Mutation | Runs | Result |
|---|---|---|---|
| B1 | Capacity check removed from `blk_read` | main, vblk | caught by both: "sectors past the end and wrapping ranges refused", "refused requests reach neither the device nor the buffer" |
| B2 | Range check as `lba + count > sectors` (overflows) | main | caught: the wrapping range is accepted, "the layer never asked the device for an impossible range" |
| B3 | Block-number overflow check removed | main | caught: "overflowing and partial blocks refused" |
| B4 | ID generation ignored | main | caught: wrong-generation and stale IDs accepted |
| B5 | Cache key ignores the device | main, vblk | caught by both: "line 0 of two devices: two misses, each device's own data" |
| B6 | Cache key ignores the line number | main | caught: hit/miss counts and contents across line boundaries |
| B7 | Stale line returned after a reset (epoch not compared) | main, vblk | caught by both: "after a device reset the cached line is stale", "after a reinitialization the cached line is dropped" |
| B8 | Failed load marked valid | main, vblk-ioerr | caught by both: "I/O error on a miss: … the line is not cached" |
| B9 | Pinned (in-use) line evictable | main | caught: "every line pinned: a miss is refused with BLK_EBUSY" |
| B10 | A loading line reused as a victim | main, vblk | caught by vblk ("two kernel threads: 300 random reads … all exact"). The main image has no test where another thread needs a victim while a line loads, so main did not detect it. |
| B11 | Cache frames not freed with the last device | main | caught: "every cache frame returned", "20 … cycles: no frame leaked" |
| B12 | DMA pool frames not freed | main + `iommu_platform=on`, vblk | caught by both: the PMM oracle ("every frame matches the oracle"), the refused-device and shutdown-cycle checks |
| B13 | Driver timeout mapped to `BLK_EIO` | main, vblk | caught by both: the mapping table, and "a timeout on a miss is BLK_ETIMEDOUT" |
| B14 | Fallback swallows per-sector errors | main, vblk-ioerr | caught by both |
| B15 | Used-ring descriptor ownership unchecked | vblk | caught: "completion naming a free slot: refused" |
| B16 | Last line not clipped to the capacity | main, vblk | caught by both: "final partial line … exactly its 3 sectors" |
| B17 | Hit-side boundary check (`off + k > nsect`) removed | main, vblk | **not detected, equivalent**: the request range check at entry already guarantees that a hit never asks for sectors past the line's end. The check is defence in depth. |
| B18 | No re-check after a yield | vblk | caught: "the yielded waiter notices the reset and returns VBLK_EBROKEN" |
| B19 | Tick bound removed | vblk | caught: "the wait ends at the 30-tick bound" |
| B20 | Never yield | vblk | caught: the reset-while-waiting test and the responsiveness test |
| B21 | Refused device keeps its pool (reset without release) | main + `iommu_platform=on`, main + `queue-size=8` | caught by both: "refused device: its DMA pool is freed …", "bus mastering switched off" |
| B22 | Checksum not verified on hits | main | caught: "a changed cache line is detected on the next hit and reloaded" |
| B23 | Cache frames accepted as caller buffers | main | caught: "… cache-frame and wrapping buffers refused" |

### 6.12 Regression

From clean builds of the final code (`8430280`; the documentation commit changes no code):

| Run | Result |
|---|---|
| Reproducibility | Two clean builds of the production x86_64 `vos.bin`: identical (`83fdfc54…f9bbd0`) |
| `make test` | exit 0; 59 QEMU runs passed, 0 failed; 11 fault comparisons; `test-user-elf` 288 passed, 0 failed |
| `make ARCH=x86_64 test` | identical to `make test` |
| `make ARCH=i386 test` | exit 0; smoke test passed |
| `make test-all` | exit 0; 60 runs passed, 0 failed; 11 fault comparisons; 288 passed, 0 failed |
| `git diff --check` | clean |
| i386 `vos.bin` | `0760e7eb8ecfcb8545f4126f5a369b0fadfc5163ad9e95c5fba2a81d45afb9dd` (unchanged) |
| `hello.elf` | `c6c6416195a0f18d680d7f8f19be52ce02ff02912ef3bac45324fbe88006cd06` (unchanged) |
| Test disk | `075bb733…4d0c77` after every run |

The run counts are those of N3.1. N3.2 added no QEMU configuration; it extends the existing
runs, and every disk run and the no-disk runs now also require the block-layer self-test. The
block-layer commit alone (`31eead8`, before the new tests) passed `make ARCH=x86_64 test` in a
separate worktree: 59 runs, 11 fault comparisons, 288 checks.

Also covered, unchanged:
- **N2:** isolation, IPC, stress, the scheduler and interrupts (the `mp`, `ipc`, `stress` and
  `irq` images, the 41-case suite under qemu64, 2 GiB and `-cpu max`);
- **N3.0:** the 4 MiB kernel map tests;
- **N3.1:** the driver tests, with the device configurations of section 5.8;
- the genuine NMI;
- the static user/ELF checks.

Physical frames are accounted for by the PMM oracle comparison in every image: every frame
matches the memory map except exactly the DMA pool and the cache frames. ABI v0 and v1 are
unchanged (no file under `user/` or `docs/abi/` changed).

### 6.13 Memory and headroom

`__kernel_phys_end` (limit of the 4 KiB-mapped area: `0x400000`):

| Image | N3.1 | N3.2 | Room left |
|---|---|---|---|
| production `build/x86_64` | `0x192000` | `0x193000` (+4 KiB) | 2484 KiB |
| main test image | `0x203000` | `0x21b000` | 1940 KiB |
| `vblk` test image (largest) | `0x209000` | `0x229000` | 1884 KiB |
| `vblk-ioerr` test image | `0x206000` | `0x21e000` | 1928 KiB |

The test images grew by the block-layer tests' static buffers (64 KiB request buffers, 24 KiB
for the concurrent readers) and code. The production image grew by 4 KiB.

At run time, with a registered disk the storage stack holds 38 PMM frames (152 KiB): 32 cache
frames and 6 DMA frames. Without a usable disk it holds none (N3.1: a refused device kept 6).
In the `vblk` image the PMM self-test found 31,869 free frames (N3.1: 31,933). The difference
is the 32 cache frames plus the 128 KiB of image growth.

### 6.14 Remaining limitations

- **Responsiveness:** yielding helps, but there are still bursts of up to 4096 polls with
  interrupts off. Interrupt-driven completion is N3.4 (section 6.7).
- **No IOMMU.** The DMA pool constrains the driver, not the device.
- **One virtio-blk device on PCI bus 0, one queue, 4 request slots.**
- **Fixed cache:** 128 KiB, no read-ahead, no per-caller quota. Pins exist only as test
  hooks.
- **Waits on loading lines:** bounded by count (1024), not by time.
- **No caller is killed in a wait** in N3.2, because there are no file system calls yet. N3.5
  must define what happens when a process is killed while its thread waits for a line.
- **FEATURES_OK refusal still untested live** (as in N3.1).
- **QEMU TCG only.**

### 6.15 Dependencies before native file operations

1. An ext2 reader (N3.3) on `blk_read_block`, which treats the disk as untrusted input.
2. Interrupt-driven completion (N3.4) before user programs may cause disk I/O, so one
   program's file read cannot stall the others.
3. The ABI v2 design (D-S8) with handle-relative calls, D-S5 and D-S6 (names), and a defined
   behaviour for a process killed during I/O (N3.5).
4. A per-process limit on outstanding requests and cache use.

## 7. Recommended N3.3 scope [Proposal]

A read-only ext2 reader in the kernel on top of `blk_read_block`, restricted to the profile of
`storage.md` section 5.3:
- superblock and group descriptors validated;
- inodes;
- direct, single, double and triple indirect blocks with bounded walks;
- linear directories;
- every field checked before use.

Tests:
- `mke2fs -d` images built reproducibly on the host (to be verified), with a host-side listing
  as the fixture;
- a set of deliberately corrupted images (superblock, group descriptors, inode, block
  pointers, directory records, loops), each refused for its intended reason without a panic.

Still not in N3.3:
- mounting into any namespace;
- file system calls (N3.5);
- interrupts (N3.4);
- any write.
