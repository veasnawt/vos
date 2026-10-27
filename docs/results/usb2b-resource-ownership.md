# USB.2-B: non-activating resource ownership foundation

**Ready for independent PR review within the fixture scope below.** Real PMM
frames, kernel page tables, Rust machine code and kernel worker reaping are
exercised. This is not an operational xHCI backend. No PCI command, controller
register, DMA address, ring, interrupt or USB device is activated. Physical
hardware is NOT TESTED; QEMU success is not hardware qualification.

Baseline: accepted PR #24, `f24b3a803a65324f31cdb4c43fd4f504394a7cdb`.
Separate checkout `/home/veasnawt/src/vos-usb2b`, branch
`feat/usb2b-resource-ownership`. Original USB/D1 checkouts remain unchanged.
See [progress](usb2b-resource-ownership/progress.md),
[evidence index](usb2b-resource-ownership/evidence.json),
[exact inventory](usb2b-resource-ownership/changed-files.txt) and
[non-self hash manifest](usb2b-resource-ownership/manifest.json).
The [USB.2 architecture](../plans/usb2-rust-xhci.md) remains the design;
this increment neither replaces its lifecycle model nor authorizes USB.2-C.

## Ownership audit and implementation

| Resource | Allocator / owning record | Release condition and failure behavior |
| --- | --- | --- |
| PCI/BAR metadata | Existing USB.1 discovery / copied Rust parser | Metadata is not a resource grant. No new production BAR/aperture qualifier. |
| Qualified fixture | Private region ID 1: 16 KiB at 4 GiB | Aligned offset/length inside the fixed region; UC only. Devmap rejects RAM, aliases and addresses beyond MAXPHYADDR. No arbitrary input PA or dereference. |
| Scoped mapping | Existing device window / devmap scope record | One exclusive tail lease, supervisor W+NX, non-global, guard page. Matching generation, kernel CR3, IF=0, non-IRQ context. Later mappings refused while owned. |
| Mapping PT pages | Real PMM / scope's frame IDs and PDE indices | Only fresh PTs owned; never free shared tables. Validate registry/PTEs/PMM, clear leaves, INVLPG, detach tables, check each free, restore exact registry/VA/byte baseline. |
| Ordinary CPU page | Real PMM / control record | One 4 KiB page, not DMA-qualified. Matching token and actual allocation required; failed release retains its identifier. |
| Worker arguments | Same CPU page / control owner | Cannot free while the recorded worker is RUNNABLE, RUNNING, BLOCKED or DEAD. Uncreated worker does not pin storage. |
| Fixture worker | Existing scheduler / never-reused ID | Cooperative stop/wake; caller yields and observes THREAD_FREE, proving record/stack reaping. No live xHCI worker or new scheduler. |
| Retained resources | Authoritative C scope/control records | Rust Drop and failed cleanup retain actual physical IDs. Reacquisition stays blocked; diagnostic copies never own resources. |
| Device-visible pages | Unimplemented | No DMA/coherency/addressability or hardware-quiescence proof. None can be freed on a software flag. |

PMM allocation bits alone do not prove subsystem ownership. Generation-bound
records reject stale/double release. Rust reuses USB.2-A's non-Copy `Lease` inside
a non-Copy, !Send, !Sync `Owned` value. Shared constants/errors move verbatim to
`types.rs` with the original public re-exports. Controller, Backend namespace,
cancellation, rings and quarantine logic are unchanged. No fake live Backend
manufactures completion or quiescence evidence.

Explicit fallible release takes `&mut self`, clearing the handle only on success.
Drop performs no teardown/context switch; C ownership survives, including when
a Rust value is forgotten. Native tests observe a dropped value's retained page,
denied reacquisition and explicit non-DMA recovery. The private C ABI uses two
u64 output fields (16 bytes/alignment 8), u32 kind and i32 result. Only Control
and Mapping are supported. Kernel callers must provide valid exclusive output
storage. This is not an application API or hostile-kernel security boundary.
Unsafe Rust is confined to the C FFI; the portable core remains safe Rust.

## Transactions, CR3 and worker lifetime

All other region IDs, WC requests, unaligned/empty/overflowing extents and
out-of-region requests fail before acquisition. RAM covering the fixture fails
closed. It is suitable for the tested 128 MiB QEMU configuration, not arbitrary
PCs. The returned virtual address is never dereferenced.

A bounded fresh two-PDE span makes PT ownership independent of framebuffer
tables. The 16 KiB fixture straddles a PDE and actually allocates two tables.
Release restores the old VA cursor; repeated cycles do not consume the window.
Existing permanent framebuffer/USB.1 mappings are preserved.

Fault budgets reject before CPU allocation, first PT and second PT allocation.
The second-table failure frees the first actual PMM table and restores baseline.
Slot exhaustion and range/context refusal acquire nothing. Every free result
is checked. Under serialized IF=0 execution, PMM prechecks exclude ordinary free
failures. Unexpected corruption retains the owner and unreleased IDs, refuses
reacquisition and may leave an audited-invalid window requiring investigation
or reboot. It is never reported as successful cleanup; no automatic corrupt-table
repair is implemented.

Cleanup refuses a foreign CR3 instead of inventing a switch. Existing scheduler
transitions and `thread_leave_user_space` remain responsible for kernel context.
Removed leaves receive INVLPG; non-global translations in other address spaces
are flushed by existing CR3 switches. SMP shootdown is not implemented. Worker
stop is one bounded observation, not an arbitrary production timeout or assumed
join. Native verification observes actual blocking, waking, exit and reaping.

The audit found a **pre-existing** permanent-API limitation: `devmap_map` may
retain an empty PT after a later allocation fails; its self-test rollback does
not reclaim tables. The new scoped transaction does not use that rollback.
Broad permanent-mapper repair remains outside this increment.

## Acceptance matrix

| Gate | Verdict | Evidence and coverage |
| --- | --- | --- |
| A ownership | PASS | 1,016 ASan/UBSan assertions against actual PMM/devmap/C-Rust services; Copy/Send/Sync compile rejections; native Drop retention/recovery. |
| B mapping | PASS for fixture | Alignment, overflow, range, cache, width, RAM, slot limits, both PT failures, permanent-map preservation, guard/PTEs, stale tokens, bad-leaf and wrong-context release refusal. Production BAR qualification is deferred, not passed. |
| C page/worker | PASS for non-DMA subset | CPU/PT rollback; release refused in every live/DEAD worker state; actual creation refusal and 18 create/block/cancel/reap cycles per firmware, exact PMM/thread baseline, scheduler/canary audits. |
| D USB | PASS | Fresh 46 core tests, 4 policy tests, original Controller/Lease Send/Sync controls, expected stale-ticket mutation rejection, 40,110 sanitized original-C/actual-Rust differential cases; native read-only PCI command/BAR unchanged. |
| E kernel/ABI | PASS | Strict C, Rust layout test, five-segment/W^X/size validator, no undefined symbols/SIMD/GOT/PLT/TLS/dynamic dependency, 497,127 R1 cases and 140 retained ELF/reference matches; static and native hello ABI0, Rixie ABI3, vsh ABI4, GUI ABI5. ABI1/2 sources are byte-identical; historical runtime campaigns not repeated. |
| F firmware | PASS final; failures retained | BIOS and corrected-GOP UEFI each pass normal and opt-in boots, GUI a/b/q, apps/hello/Rixie, console restoration and process cleanup. Original pre-GRUB delay remains INCONCLUSIVE as to cause; missing-GOP media remains FAIL. |
| G preservation | PASS | Independent recomputation: 5,747 historical entries, 49 protected entries/45 unique paths, original refs/index/working sets, 13 worktrees, two stashes, retained symlink and original USB.1 checkout unchanged. |

Sanitizers instrument hosted C, not Rust or guest machine code. Hosted CPU,
physical-alias and scheduler-observation boundaries are mocks; actual worker
behavior is verified separately in VOS. Allocation budgets inject faults, not
physical RAM exhaustion. Kernel boot calls the real Rust export and actual PMM
and mapper. No unused symbol or model counter is presented as native execution.

Historical outcomes remain distinguishable:

1. Initial host compile rejected assertion parentheses under `-Werror`; corrected
   without changing expectations/flags. First successful run: 882 checks; final
   expanded run: 1,016. The failure log remains.
2. First firmware run passed both BIOS cases, then missed the 35-second UEFI GRUB
   marker. A separate 35-second observation-only diagnostic reached GRUB by
   20 seconds, without input or deadline extension. Initial delay unexplained.
3. Subsequent UEFI boot reached VOS but correctly refused GUI without a framebuffer.
   The producer omitted the accepted `grub-uefi-head.cfg`. A fresh image prepended
   that exact header and passed both UEFI cases; kernel/apps unchanged.
4. A compatibility command reused an already-created log name and stopped before
   test compilation. The valid library output was reused with distinct test-log
   names. No failure was overwritten.

After native verification, two source comments clarified legacy-map/module
boundaries. Final relinking passed layout checks; its Rust archive is identical
and all PT_LOAD descriptors/loaded bytes match the native kernel. Full ELF
hashes differ outside the loaded image only. The
[equivalence receipt](usb2b-resource-ownership/comment-clarification-equivalence.json)
justifies reuse without another native campaign.

## Memory and performance

- Successful fixture: **12 KiB**, one ordinary CPU page plus two PT pages, all
  released. The **28 KiB logical USB.2-A budget is unchanged**; four DMA-designated
  model pages remain unallocated/unpublished.
- Permanent owner records: scope 80 + control 56 = **136 bytes**. Fault budgets
  add 8 initialized bytes, partly fitting existing alignment; below 512 bytes.
  Rust Owned=32, Lease=24, Controller=424, Usage=8 bytes.
- Loaded text +4,096 bytes; rodata stays 45,056. `.data` +4; `.bss` stays
  597,824 through reused alignment space, not absence of new state. Boot BSS
  remains 20,480. Five segments reserve **851,968 bytes** versus 847,872:
  one additional real page. No heap or new static stacks; fixture workers use
  an existing guarded 16 KiB scheduler stack.
- Compiler frames: mapping acquire 144 bytes, release 48, CPU acquire 64,
  worker body 16, verifier 96. Not a full dynamic stack high-water proof.
- Seven optimized, unsanitized host samples, 1,000 cycles each: equivalent C
  service cycle median **3,110 ns** (3,039–3,155), Rust-owned cycle **3,231 ns**
  (3,138–3,546); median paired ratio **1.043**. First-PT refusal/rollback median
  297 ns (297–464), second-PT rollback 480 ns (479–484).

These are hosted algorithm costs with mocked privileged effects and real PMM
bookkeeping. Full cycles include duplicate-release checks, not pure FFI timing.
One process, no CPU pinning; scheduling/cache variation remains. The old permanent
mapper has no equivalent release, so an old/new mapper speed ratio is not meaningful.
No physical MMIO, language-speed or hardware regression claim. Raw samples,
segment data and stack reports are retained.

## Reproduction and ordinary preview

Use [CONTRIBUTING](../../CONTRIBUTING.md) prerequisites and fresh outputs:

```sh
rustc="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
out="$PWD/build/usb2b-review"
make ARCH=x86_64 BUILD="$out/kernel" RUSTC="$rustc" -j2 all
python3 -I -B scripts/test-usb-resources.py --rustc "$rustc" \
  --archive "$out/kernel/rust/libvos_kernel.a" --out "$out/host"
# Or reuse verified read-only application ELFs instead of rebuilding them.
make ARCH=x86_64 USER_BUILD="$out/user" RUSTC="$rustc" \
  user-vsh user-rust user-gui RUST_APP=rixie
python3 -I -B scripts/test-usb-resources-native.py \
  --kernel "$out/kernel/vos.bin" --apps "$out/user" \
  --efi /path/to/grub/x86_64-efi --out "$out/native"
```

The driver builds stock-GRUB graft-point images and hashes ISO payloads without
extracting duplicates. It supervises owned QEMU children and preserves failures.
No package/firmware download. Retained local previews:

```sh
cd /home/veasnawt/src/vos-usb2b
qemu-system-x86_64 -machine q35 -accel tcg -m 128M -nic none \
  -device qemu-xhci,id=xhci -boot d -serial stdio -no-reboot \
  -cdrom build/usb2b-1/native/bios.iso

# Create a private variable file; never share it with another live QEMU.
preview_vars=$(mktemp /tmp/vos-usb2b-vars.XXXXXX.fd)
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$preview_vars"
qemu-system-x86_64 -machine q35 -accel tcg -m 128M -nic none \
  -device qemu-xhci,id=xhci -boot d -serial stdio -no-reboot \
  -cdrom build/usb2b-1/native-uefi-gop/uefi.iso \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file="$preview_vars"
```

At GRUB, Enter selects normal GUI; `r` then Enter selects resource tests followed
by the same GUI. No timed human stress test. In VOS press `a`/`b` normally to
change the panel; `q` returns to vsh. Run `apps`, `hello`, `rixie`, then `help`
and `exit` in Rixie, and `exit` in vsh. Input remains PS/2, not USB HID.

Authentic post-resource compatibility captures (not new USB GUI functionality):

![BIOS](../images/usb/usb2b/vos-usb2b-bios.png)
![UEFI](../images/usb/usb2b/vos-usb2b-uefi.png)

They pair with raw serial evidence. UEFI selected 1280x800; BIOS used its existing
mode. No screenshots were generated or cosmetically altered.

## Storage, limitations and next boundary

[Storage accounting](usb2b-resource-ownership/storage.json) separates retained
new output from the conservative producer peak. The initial 84 MiB estimate
became 96 MiB of logical output to retain failed media. Filesystem block overhead
is separate: the conservative physical producer provision is 104 MiB, not a
sampled high-water measurement. No protected data was deleted, historical
funding/ledger/reservation changed, or discontinued D1 admission/attestation
revived. Accepted app/GRUB inputs were reused read-only.

Publish for owner review only: no merge/auto-merge. N6 remains incomplete.
No USB.2-C, R2, enumeration, HID or physical testing begins here.

USB.2-C needs independent BAR extent/D0/firmware ownership qualification, actual
DMA addressability/coherence/IOMMU rules, publication barriers, hardware halt/
quiescence proof, deadlines and a live backend preserving existing namespace/
quarantine semantics. The fixture token and ordinary CPU page supply none of
those hardware proofs. Safe ownership/rollback and reaping contracts can be
reused without treating a software flag as permission to free device-visible RAM.
