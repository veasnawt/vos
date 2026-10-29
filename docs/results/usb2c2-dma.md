# USB.2-C2: Rust-owned DMA memory and quarantine foundation

**Verified for PR review, not accepted or merged.** C2 is a software
resource-ownership milestone, not a controller-activation milestone. It adds a
fixed-table Rust pool for DMA-designated pages, covering allocation, zeroing,
preparation, exact release, rollback and bounded quarantine, together with an
opt-in exercise (`vos.usb2c2-dma=1`) that runs in normal BIOS/UEFI QEMU boots.

**Nothing is published to a device.** No address is programmed into hardware and
no page's ownership passes to a controller. C2 writes no PCI configuration
register, xHCI operational or runtime register, doorbell, firmware ownership
semaphore, or ring pointer. Verification is QEMU-only; physical hardware is NOT
TESTED.

Baseline: accepted `main` at PR #27, `84a0c15c223001411499f396a6a89cc98684d6d3`.
Worktree `/home/veasnawt/src/vos-usb2c2`, branch `feat/usb2c2-dma`. See the
[exact inventory](usb2c2-dma/changed-files.txt),
[hash manifest](usb2c2-dma/manifest.json),
[measurements](usb2c2-dma/measurements.json),
[pre-implementation budget proposal](usb2c2-dma/history/budget-proposal.json),
[storage](usb2c2-dma/storage.json) and [preservation](usb2c2-dma/preservation.json)
records.

## Architecture

| Layer | Responsibility |
| --- | --- |
| `kernel/rust/usb/xhci/dma.rs` (canonical tree, safe, portable) | `Pool` holds 8 fixed records, at most 4 live pages and 4 quarantine places. `Page` is a non-Copy, !Send/!Sync handle built on the canonical `Lease`, `Kind` and `Error`; no type is duplicated. Tokens carry a generation. `Frames` is the platform trait. Accounting and diagnostic `Entry` copies are also defined here. |
| `arch/x86_64/rust/xhci_dma.rs` | `Pmm: Frames` is implemented over the C adapter. The boot's single xHCI pool lives in `.data` and is touched only by the once-per-boot exercise on the serialized boot thread with IF=0. That exercise is the private `vos_usb_dma_exercise_v1` ABI with a 104-byte report whose layout is asserted on both sides. |
| `arch/x86_64/usb_dma.{c,h}` | A narrow PMM adapter: allocate one frame, CPU-zero and verify it through the kernel RAM alias, free it. It operates only on frames in its own 8-entry allow-list and requires kernel CR3, IF=0 and a non-IRQ context. It also holds exercise-only fault hooks, `usb_dma_boot`, and the boot check. USB.2-B's `usb_resources.c` is unchanged. |

**No new system-wide DMA subsystem, heap, thread, page table or devmap mapping.**
Frames are reached through the existing kernel RAM alias.

**Address spaces are kept distinct.**
- **CPU physical frames:** used by the PMM and the pool's private records.
- **CPU virtual views:** exist only inside the C adapter.
- **Device-visible DMA addresses:** not established. VOS has no IOMMU/DMAR qualification and coherency is unknown, so no API produces one.
- **Safe API:** never returns a raw frame. A compile-fail control rejects any read of `Page.lease`. Diagnostic copies include frame numbers for identification only; they are not authority.

## Ownership lifecycle

```text
Free -> Allocated -> Zeroed -> Prepared --release--> Free (generation + 1)
         |  zero fails / release fails / unqualified frame
         v
   Quarantined (<= 4) --exhausted--> Retained (pool closed)   never freed or reused
Prepared --publish(PublishProof)--> Published    (unreachable: PublishProof is uninhabited)
Published --release_published(QuiescenceProof)--> (unreachable: uninhabited)
```

**Published state.** It is designed but unreachable in C2, by construction:
`PublishProof` and `QuiescenceProof` are empty enums, so no caller can build one
(compile-fail control, E0599). The planned invariants:
- no CPU writes except through the ring protocol;
- never freed, reused or zeroed;
- release only after a quiescence protocol proves the device stopped (halt, reset complete, bus mastering off, plus an ordering proof).

**Ownership rules enforced:**
- **Ordered transitions:** `prepare` requires Zeroed, `zero` requires Allocated, and release requires an unpublished state.
- **Stale or foreign handles:** checked by token, generation, frame and kind.
- **Double release:** a compile error (E0382); a stale generation is refused at run time.
- **Kinds:** only DMA kinds (Command, Event, DCBAA, ERST) are accepted.
- **Frames:** must be page-aligned, inside the verified PMM range [1 MiB, 1 GiB), below the device limit, and not a duplicate of a live frame.
- **Unqualified frames:** returned at once, or retained if they cannot be returned.
- **Set allocation:** `allocate_set` is transactional; on failure it releases in reverse order.
- **Zeroing failure:** retains the page, because its content is unknown.
- **Release failure:** retains the page, because it has not been proven freed.
- **Cancellation:** releases only unpublished pages and never touches quarantined ones.

**Quarantine has no release path.** A quarantined page stays owned for the rest
of the boot; no timer, cancellation or flag releases it. When the quarantine is
exhausted, the pool fails closed: the page becomes Retained, the pool is closed,
and every later allocation is refused.

## Firmware state

C1 showed that SeaBIOS and OVMF leave bus mastering enabled and their own
CRCR/DCBAAP/ERST pointers programmed. C2 does not assume a reset or idle
controller.

- **No controller access.** The exercise never touches it; the trace shows zero
  xHCI register reads unless C1 qualification is also selected.
- **No overlap observed.** In every native boot, C2's frames
  (`0x1da000`–`0x1dd000`, retained `0x1da000`) did not coincide with the firmware
  ring/context pages (SeaBIOS `0x7fdf000`; OVMF `0x674a000`/`0x6756000` and
  neighbours).
- **Open kernel-wide risk.** Whether the kernel PMM can hand out memory that
  firmware rings still point at (for example OVMF boot-services data, which
  becomes available after ExitBootServices) is **not established**. That applies
  to all kernel allocations, not just C2, and C3 must resolve it before any
  controller runs.

## Acceptance gates

| Gate | Result | Evidence |
| --- | --- | --- |
| A. Rust state machine | **PASS.** 9 tests cover lifecycle and exact accounting; budget, kind and closed refusals; ordered transitions; stale-generation and foreign handles; frame validation (misaligned, below 1 MiB, at 1 GiB, beyond the device limit, duplicate) with return or retention; failures at every set step (allocation → exact rollback, zeroing → retention, rollback release failure → retention); quarantine exhaustion failing closed; cancellation. 6 compile-fail controls: Page not Copy/Send/Sync (E0277), double release (E0382), raw frame access (E0616), no `PublishProof` value (E0599). 3 mutation controls, each detected (exit 101): generation check removed, quarantine unbounded, rollback removed. | [c2-host](usb2c2-dma/hosted/c2-host/result.json) |
| B. Hosted C/Rust | **PASS** under ASan/UBSan: the real PMM plus `usb_dma.c` against the actual Rust archive. 32,815 adapter checks cover the allow-list, real zeroing of every byte, unknown-frame and double-free refusal, wrong CR3/IF/IRQ, and injected failures at each operation index with exact PMM accounting. Six forked exercise scenarios: clean pass (allocated 12, prepared 12, released 11, quarantined 1), repeat refusal, zeroing failure retains, release failure retains, allocation failure rolls back exactly, bad context, null and misaligned report. C sanitizers do not instrument the Rust code. | [sanitizer](usb2c2-dma/hosted/c2-host/sanitizer.txt) |
| C. BIOS/UEFI runtime | **PASS 8/8.** Each firmware runs normal, the exercise, the exercise with C1 qualification, and the exercise with no xHCI attached. Every exercise boot releases its unpublished pages exactly and retains one deliberately quarantined frame (PMM −1, frames held 1). | [native](usb2c2-dma/native/result.json) |
| D. Regression | **PASS.** USB.2-A 46/46. USB.2-B 1,016 assertions plus Copy/Send/Sync controls. USB.2-C1: 17 Rust tests, 2/2 mutations, 76 sanitized assertions, and the unchanged C1 native matrix 14/14. USB.1 host suites. R1: 497,127 cases and 283 files. R2-B: differential plus the native pointer matrix 4/4. GUI, vsh, apps, hello and Rixie pass in every full boot. | [hosted](usb2c2-dma/hosted), [regressions](usb2c2-dma/regressions) |
| E. Budgets | **PASS against the limits recorded before implementation** (table below). The C1 20 KiB exception was not used. | [proposal](usb2c2-dma/history/budget-proposal.json) |
| F. Evidence and preservation | **PASS.** Git identities, logs, negative controls, original failures, accounting, measurements, storage, screenshots and the manifest are retained. | [history](usb2c2-dma/history) |

### Zero hardware publication

Every boot was traced. Events after the kernel's first serial byte are attributed
to VOS, and the firmware's own writes before it serve as positive controls.

| Mode/device | BIOS | UEFI | xHCI MMIO writes | Config writes | State events (reset/run/doorbell/ring fetch/event/slot/transfer) | xHCI reads |
| --- | --- | --- | --- | --- | --- | --- |
| normal / qemu-xhci | PASS | PASS | 0 | 0 | 0 | 0 |
| exercise / qemu-xhci | PASS | PASS | 0 | 0 | 0 | 0 |
| exercise + C1 / qemu-xhci | PASS | PASS | 0 | 0 | 0 | 18 (C1) |
| exercise / no xHCI | PASS | PASS | 0 | 0 | 0 | 0 |

**Independent checks:**
- **PCI identity and BAR:** unchanged in QMP `query-pci` before and after.
- **Controller registers:** HMP snapshots, taken after the shell exited with read tracing off, equal the last value firmware wrote to each programmed register.
- **No device activity:** no device ring fetch or DMA event appears after the boundary.

### Resources

Measured against a kernel built from `84a0c15`; limits were recorded before
implementation.

| Measure | Result | Limit |
| --- | ---: | ---: |
| Text + rodata (input sections) | **9,039 B** (text 8,414, rodata 625) | 12,288 B |
| Loaded PT_LOAD growth, page-rounded | 12,288 B (code segment) | 16,384 B |
| Permanent RW data | 344 B (`.data` 272: the pool and fault table; `.bss` 72: the allow-list) | 512 B |
| DMA-designated pages live | ≤4 (16 KiB) | 4 |
| Quarantine records | 4, failing closed | 4 |
| Retained per opt-in boot | 1 page (deliberate) | 1 |
| New page tables, threads, heap | 0, 0, 0 | 0 |
| Largest Rust frame | 440 B (`vos_usb_dma_exercise_v1`) | 512 B |
| Largest C frame (C2 code) | 160 B (`usb_dma_boot`) | 512 B |

Growth by contributor:
- **Rust text:** 6,145 B (pool generics plus the exercise).
- **Adapter text:** 2,189 B.
- **`kernel.o`:** 80 B text and 621 B of merged strings.

**Timing:** an allocate + zero + free cycle takes a median of 383 ns in an
optimized, unsanitized host build with real PMM work. It is not a hardware
latency.

## Retained failures

These are kept under [history](usb2c2-dma/history):

1. **Pure Rust test file:** a borrow-checker compile error in a test helper call.
2. **New hosted harness:** a linker error from a missing `boot_mmap_entry` stub.
3. **First hosted run:** my expected totals were wrong (11/10/10). The exercise correctly reported 12 allocated, 12 prepared and 11 released.

Production code was unchanged by all three. The trial hosted run is retained as well.

## Reproduction

```sh
rustc="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
out=build/usb2c2-review
make ARCH=x86_64 BUILD=$out/kernel RUSTC="$rustc" verify
python3 -I -B scripts/test-usb2c2.py --archive $out/kernel/rust/libvos_kernel.a --rustc "$rustc" --out $out/host
python3 -I -B scripts/test-usb2c2-native.py --kernel $out/kernel/vos.bin \
  --apps <dir with verified vsh/hello/rixie/gui-demo ELFs> --efi <x86_64-efi GRUB modules> --out $out/native
```

Build smoke CI runs `test-usb2c2.py` alongside the C1 and USB.2-B host suites.

## Residual risks for USB.2-C3

1. **No device-visible address, by design.** C3+ needs a DMA/platform qualifier (IOMMU/DMAR state, coherency, address width) before any page can move to Published.
2. **Firmware state.** Firmware leaves bus mastering on and its rings programmed. C3 must quiesce the controller (halt, reset, confirmed stop) before any VOS page is published, and must not attach to firmware rings.
3. **Possible PMM overlap with firmware DMA memory.** Whether the PMM can allocate memory that the firmware rings still reference is unestablished and affects the whole kernel.
4. **Quarantine only covers C2's pages.** It never frees, but it cannot help with firmware-owned memory.
5. **One profile.** All results come from the QEMU profile; there is no physical-hardware claim.
