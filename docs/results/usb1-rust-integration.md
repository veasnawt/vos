# USB.1 integration with the Rust-enabled kernel

Status: verified integration candidate for owner PR review; not merged by this
work. USB.2 and USB keyboard/mouse support are not implemented.

## Baseline and provenance

The owner merged [R1 PR #21](https://github.com/veasnawt/vos/pull/21).
Current accepted main for this integration is
`7c8082b98751f65ab0bcdf884339a6bf8298e2ce`; its reviewed R1 head is
`df4ad8d3164d27e1254e4514341726b7b18955d3`.

A new independent checkout, `/home/veasnawt/src/vos-usb1-integration`, and
`feat/usb1-rust-integration` branch start at that main. A normal, non-fast-forward
merge of original USB.1 commit
`bd22803b8b5635b776c848d08d1494f65b49dd21` preserves its provenance as a second
parent. There were **zero manual conflicts**. Git combined the object list in
`arch/x86_64/arch.mk` and the initialization calls in `arch/x86_64/kernel.c`.
The remaining 62 of the original 64 candidate files match USB.1 byte for byte.
No R0/R1 implementation was rewritten.

The [original USB.1 report](usb1-xhci.md), [64-file inventory](usb1/changed-files.txt),
63-entry manifest and historical failure records remain unchanged. That manifest
describes the original commit, not the two now-combined source files. New evidence
and current source identities are recorded separately in the
[integration manifest](usb1-integration/manifest.json) and
[merge review](usb1-integration/merge-review.json).

## Preserved behavior and limits

`vos.usb_probe=1` is an exact opt-in boot token. The probe scans PCI bus 0 only,
inspects at most four matching xHCI endpoint functions, validates an assigned
32/64-bit memory BAR and supported capability structure, and reads seven fixed
capability DWORDs. It reuses supervisor-only UC device mappings, with at most
one mapped page per controller retained for the boot lifetime. Its interfaces
provide no PCI configuration-data or MMIO write operation. Device mappings retain
the existing kernel RW policy; “read-only” describes probe operations.

There is **no reset, firmware ownership handoff, bus-master enablement, interrupt
setup, DMA, rings, USB transfer or device enumeration**. Firmware may already
enable bus mastering; the probe preserves that state. PCI mechanism-1 address
selection still writes the existing configuration-address port. BAR aperture is
not destructively sized in the guest, and operational/runtime/doorbell/extended
registers are not dereferenced. QMP independently reports the QEMU BAR aperture.

The original `vos.input_diag=1` diagnostics remain silent by default and report
the same GUI/console initialization results without changing authority or input
semantics. Normal input is PS/2. Removing i8042 while attaching `usb-kbd` correctly
fails native console authority; discovery does not make that USB keyboard usable.
This does not establish the cause of any physical computer's boot failure.

## Fresh verification on the integrated tree

| Check | Actual result |
| --- | --- |
| USB probe C ASan/UBSan | 45,713 assertions PASS: bounded scan, malformed metadata/capabilities, mapping failures and opt-in behavior |
| Keyboard/editor and input diagnostics ASan/UBSan | PASS; diagnostic default-silent, enable and disable behavior preserved |
| Console handoff ASan/UBSan | 49,499 assertions PASS |
| GUI bootstrap ASan/UBSan | 523 assertions PASS; 64 actual ELF rejections, 64 modeled process-admission failures and service-start failure |
| Rust-backed primary `user-test` | PASS; native application ELF/static negative checks |
| Actual Rust bounds helper linked to sanitized hosted C | 497,127 boundary cases PASS against C and u128 oracles |
| Rust/C ELF differential | 287 files PASS: 201 accepted, 86 rejected, identical result codes and image fields |
| Primary Rust/C checker exact output comparison | 287/287 PASS |
| Deny-all mutation controls | 2/2 rejected as expected |
| Kernel image audit, production and selftest | Original strict layout check PASS: five PT_LOADs, no RWX, page-tiled reservation below 4 MiB; no undefined/runtime relocation, allocator, dynamic/TLS/unwind or FP/SIMD findings |
| Existing ELF selftest, BIOS and UEFI | 35 cases and 69 corrupted-executable rejections per firmware; expected QEMU exit 33 |
| Actual Rust execution trace | 78 filtered execution records per firmware at the real 18-byte `vos_rust_elf_inside` function |
| Normal pointer GUI / apps / hello / Rixie / shell, BIOS and UEFI | PASS: a/b/q, two console restorations and vsh process reclamation |

Rust machine code itself is not sanitizer-instrumented. The original USB hosted
bootstrap regression uses the documented C reference validator; the additional
primary-tool, differential and native checks above exercise the actual Rust path.
The production Rust archive is byte-identical to accepted R1:
`abb3773f1b21e291a2eb1943f9a5a7e697f151f1ca79bcf2113ae9bfbfeb692b`.
All 119 tracked userspace/ABI files are unchanged against accepted main.
R0 panic-bridge evidence is reused because its source, compiler contract and Rust
archive are unchanged; deliberate Rust panic was not rerun here.

The original USB verifier also passes **ten native cases**, five per firmware:

| Profile | Expected behavior, verified on BIOS and UEFI |
| --- | --- |
| Control: no probe token, no xHCI | No USB/diagnostic output; normal GUI and apps/hello/Rixie/shell lifecycle |
| Probe + xHCI + attached usb-kbd | One valid capability inspection; unchanged PCI command/BAR; GUI startup (no USB input claim) |
| Probe, xHCI absent | Explicit bus-0 not-found diagnostic; normal GUI/application interaction |
| i8042 disabled, text + xHCI + usb-kbd | PS/2 initialization and vsh authority fail closed |
| i8042 disabled, GUI + xHCI + usb-kbd | Worker and ELF initialization reported; GUI/console grants fail closed, not false USB HID success |

Two additional screenshot sessions retain xHCI, omit `usb-kbd`, and use the
normal PS/2 path. Both verify GUI a/q, apps, hello, Rixie help/exit, restored shell
and process reclamation. QMP PCI metadata is identical before and after these
interactions. Serial logs independently verify R0's seed/salt/result computation.
No network or host disk is attached. QEMU uses q35/TCG, 128 MiB and private OVMF
variable stores; tests run sequentially and terminate only their owned guests.

An initial independent binary-audit invocation expected the wrong symbol name,
`vos_rust_kernel_probe`; accepted R0 exports `vos_rust_probe`. The stopped audit
and diagnostic output are preserved. Only the audit predicate was corrected;
no implementation or native test was changed or retried to conceal a defect.

Evidence: [USB outcomes](usb1-integration/usb-results.json),
[Rust host results](usb1-integration/rust-host-results.json),
[Rust native results](usb1-integration/rust-native-results.json),
[source/binary review](usb1-integration/source-binary-review.json),
[capture results](usb1-integration/capture-results.json),
[build identities](usb1-integration/build-identities.json) and
[exact integration inventory](usb1-integration/changed-files.txt).
Raw serial, command and PCI records accompany these files.

## Authentic visual evidence

![Native BIOS GUI after read-only xHCI inspection](../images/usb1/vos-usb1-integrated-bios.png)

[UEFI screenshot](../images/usb1/vos-usb1-integrated-uefi.png).
Both are direct QMP PNG captures of actual VOS at 1280x800 after the `a` key
changes the native GUI panel. Their pixels happen to be identical; separate
firmware commands and serial logs establish each run. The images demonstrate
continued native graphics/input behavior, not USB HID operation. xHCI inspection
is established by the accompanying serial and QMP records, not the picture alone.

## Reproduction and ordinary preview

Use the existing bare-metal rustc with `x86_64-unknown-none`, GCC/binutils,
Python, GRUB BIOS/EFI producers, xorriso, QEMU and OVMF. No new package installation
was needed. [Executed commands and local producer paths](usb1-integration/commands.json)
record the exact inputs. Set `RUSTC` and `GRUB_EFI_DIR` to their installed paths;
the historical EFI producer is a read-only local input, not a portable prerequisite.

```sh
cd /home/veasnawt/src/vos-usb1-integration
python3 -I -B scripts/test-usb-probe.py \
  --output build/usb1-reproduction \
  --rustc "$RUSTC" --grub-efi-dir "$GRUB_EFI_DIR"
```

Select a fresh output root; never overwrite retained results. The scripts in
[commands.json](usb1-integration/commands.json) reproduce the Rust regressions and
the separate capture sessions. CI adds the portable USB host-only check to the
existing Rust kernel/user-ELF build workflow. Live PR CI is separate evidence;
local results do not claim GitHub checks have passed.

For a human preview of the retained verified media (no timed input requirements):

```sh
cd /home/veasnawt/src/vos-usb1-integration
qemu-system-x86_64 -machine q35 -accel tcg -m 128M -nic none \
  -device qemu-xhci -cdrom build/usb1-integration-1/usb/probe-bios.iso \
  -boot d -serial stdio -monitor none -no-reboot
```

For UEFI, create a new private variable store; do not reuse a verification file:

```sh
preview_vars=$(mktemp build/usb1-preview-vars.XXXXXX.fd)
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$preview_vars"
qemu-system-x86_64 -machine q35 -accel tcg -m 128M -nic none \
  -device qemu-xhci -cdrom build/usb1-integration-1/usb/probe-uefi.iso \
  -boot d -serial stdio -monitor none -no-reboot \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$preview_vars"
```

The graphical `gui-demo` starts after the read-only USB diagnostics and normal
boot checks. Press `a`/`b` normally to change its panel, `q` to return to vsh,
then try `apps`, `hello`, and `rixie` (`help`, then `exit` returns to vsh).
Input in these commands is PS/2; do not add `usb-kbd` or disable i8042 to test
usability. These profiles contain four normal apps and no GUI stress/probe module.
The separate Rust regression uses the accepted pointer demo; the screenshots
above use the simpler original USB.1 GUI profile. No disk fixture is required.

## Preservation, resources and follow-up

The [preservation audit](usb1-integration/preservation.json) verifies the original
919-file workspace and 972-file USB checkout, original HEAD/index/refs,
3,856 historical entries across 13 roots, 13 worktrees, two stashes and the
retained synthetic symlink. The R0/R1 checkouts remain clean and unchanged.
Historical controllers, failed/interrupted evidence and reservations are
untouched. No legacy D1 admission/attestation, funding, retirement or cleanup
operation was performed. [Resource measurements](usb1-integration/resources.json)
record the new independent checkout's ordinary disk usage and available capacity.

Physical hardware, non-bus-0 topology, arbitrary BAR apertures/versions and USB
devices are NOT TESTED/supported by this increment. Faulty physical MMIO can
still fail to complete; metadata validation does not establish hardware safety.
No USB.2, controller operation, HID, networking, GUI feature or additional Rust
migration is included. No correctness discrepancy was found in the integration.

Before a separately authorized Rust-first USB.2 design, resolve: which bounded
controller-ownership/reset subset is appropriate; C/Rust register/unsafe-MMIO
boundaries; DMA allocation/address-width/cache ordering and ring ownership;
polling versus interrupts; timeout, rollback and unconfirmed-reset quarantine;
and how later HID input will join the existing input owner without granting
applications MMIO or DMA authority. These are design questions, not implemented
USB functionality or permission to begin USB.2.
