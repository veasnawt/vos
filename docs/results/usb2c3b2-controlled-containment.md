# USB.2-C3b.2: controlled xHCI containment (QEMU test image only)

**Verified for PR review, not accepted or merged.** C3b.1 rehearsed controller
containment without writing anything. C3b.2 is the first increment that actually
writes to a controller, under tight, independently verified limits:

- **Where it exists.** Only in a separately built, explicitly QEMU-only **test
  image** (`VOS_CONTAIN_TEST=1`). Production and physical-PC images contain none
  of the code: no symbol, no string, no write path. This is proven from the
  binaries, not from build variables.
- **When it runs.** Never by default, not even in the test image. It needs the
  one-shot option `vos.usb2-contain-execute=1` and refuses if combined with any
  other C-series option.
- **What it may write.** Exactly two things, in this order, once per boot, and
  only to the one eligible idle UEFI QEMU xHCI in a verified QEMU 10.2.1 TCG
  environment:
  - **W1:** a 16-bit PCI Command write at offset 0x04 that clears Bus Master
    Enable only (`0x0007 → 0x0003`);
  - **W2:** a 32-bit read-modify-write of USBCMD at BAR0 + CAPLENGTH setting
    HCRST only (`0x0 → 0x2`).
- **What happens after.** Each write is verified. From W1 on, every outcome is
  final for the boot. **Bus mastering is never re-enabled**, firmware memory is
  never released, and nothing is enumerated, initialized or started.

**QEMU's reset behaves differently from real xHCI hardware**, and the reset
completes inside the register write there. This report establishes nothing about
physical controllers, reset timing, Controller Not Ready behavior, Transactions
Pending or DMA drain. Physical hardware is **NOT TESTED** and unauthorized.

- **Baseline:** merged C3b.1, PR #32, `6eb9a30be63e29667a75b9a04c4cafe994f25e3c`
  (its tree equals the accepted candidate `7d206d2`).
- **Worktree:** `/home/veasnawt/src/vos-usb2c3b2`, branch
  `feat/usb2c3b2-controlled-containment`.
- **Final candidates** (source-to-binary identity in the manifest):
  - production kernel `kernel-4`: `vos.bin` `e34453f59af34904…`, byte-identical to
    the earlier production builds `kernel-2` and `kernel-3`;
  - QEMU test image `contain-test-7`: `vos.bin` `e16001918dd3473c8a751466a384ac2f5d9f3ecda7ad3e187cd8e9604456a1f8`.
- **Records:**
  [inventory](usb2c3b2-controlled-containment/changed-files.txt),
  [hash manifest](usb2c3b2-controlled-containment/manifest.json),
  [measurements](usb2c3b2-controlled-containment/measurements),
  [budget gate](usb2c3b2-controlled-containment/history/budget-gate.json),
  [storage](usb2c3b2-controlled-containment/storage.json),
  [preservation](usb2c3b2-controlled-containment/preservation.json),
  [retained failures](usb2c3b2-controlled-containment/history/failures.json),
  [Gate 0](usb2c3b2-gate0-report.md).

## Authorization, budgets and the retained gate failure

C3b.2 was authorized for QEMU 10.2.1 with UEFI only, with the two writes above,
no physical hardware, no BIOS containment and no bus-master restoration.

The first full candidate exceeded the 3,072 B code ceiling. Work **stopped** at
that gate, with every safety check still in place, and the owner approved
**Option A**: independent budgets for production and test images.

| Resource | Approved | Result |
| --- | --- | --- |
| Production code + rodata growth | 3,072 B | **+117 B** |
| Test-image code + rodata growth | 8,192 B | **+7,720 B** |
| Cumulative C3b test-image growth (vs C3a.3) | 17,408 B | **+16,619 B** |
| Permanent RW growth (this milestone) | 32 B | **0 B** |
| Largest individual frame | 512 B | **472 B Rust / 480 B C** |
| Build/evidence tree | 600 MiB | **625.2 MB** of 629.1 MB (622.2 MB build tree + 3.1 MB repository evidence) |

The gate-time candidates (10,226 B, then 8,281 B and 7,468 B) and the stop record
are retained in [history/](usb2c3b2-controlled-containment/history/). The final
candidate grew 252 B after the gate because the immediate re-reads before each
write also read every port and every terminal state now prints its name.

**Cumulative stack depth.** A static upper bound (direct calls followed exactly,
indirect calls resolved conservatively) puts the deepest chain below
`xhci_contain_execute_boot` at **4,288 B over 21 frames**, against the
production C3b.1 chain's 2,904 B and the 16 KiB kernel stack with an unmapped
guard page. It is an estimate from static information, not a measurement of a
running kernel; no stack fault occurred in any boot.

## Specification basis and remaining gaps

The [Gate 0 review](usb2c3b2-gate0-report.md) obtained the **primary xHCI 1.2
specification** (Intel, May 2019). The implementation follows it:

| Requirement | Where applied |
| --- | --- |
| §4.21.2: halt the controller and verify HCH = 1 before clearing Bus Master Enable | W1 only from a halted, idle controller |
| §5.4.1: HCRST only when HCH = 1; software must not write operational or runtime registers while HCRST = 1 (reads are allowed) | W2 once; the poll only reads |
| §5.4.1: "PCI Configuration registers are not affected by this reset" | bus mastering stays off across W2; checked after |
| §5.4.2: do not write operational registers until CNR = 0 | W2 requires USBSTS exactly 1; the poll waits for CNR = 0 |
| §5.1.1 reserved fields (RsvdP preserved) | W2 is a read-modify-write; the profile requires USBCMD = 0 |
| §4.23.1, §5.4: registers at their defaults after HCRST | post-reset verification |
| §5.4.5: CRCR pointer reads return 0 on compliant hardware | the check requires 0, which holds on both |

**Documented QEMU deviations**, deliberately not generalized:

- QEMU returns the command-ring pointer on CRCR reads, where the specification
  says 0; C3a.3's walk and C3b.1's CRCR check rely on that and would not carry
  over to a compliant controller.
- QEMU resets IMOD to 0 (the specification default is 4000h), so IMOD is not
  verified. QEMU sets the connect-status-change bit on every port during a reset,
  so port change bits are ignored (connection, enable, reset and over-current
  must be clear).
- HCRST completes synchronously inside the write and CNR is never set, so the
  timeout paths cannot occur natively; they are covered by hosted model tests
  and fault injection.
- With bus mastering off a QEMU DMA attempt raises HCE where the specification
  would set HSE; this gives an observable for any DMA after W1. Transactions
  Pending is never set by QEMU, so that check proves nothing there.

**Gaps that remain** (reported, not silently substituted):

- The PCI Express Base Specification was not available (members-only), so the
  explicit PCIe statements about Bus Master Enable and MSI/MSI-X, and the exact
  Transactions Pending semantics, are not verified from primary text.
- The UEFI specification's `ExitBootServices` obligations could not be checked
  (uefi.org refuses automated access).
- The PCI Local Bus 3.0 text came from a third-party mirror of unverified
  provenance.

## Production isolation

The executor is absent from production images by construction and by proof. A
build-directory name alone is not trusted; protection is layered:

1. `VOS_CONTAIN_TEST=1` **and** `VOS_CONTAIN_TEST_ACK=QEMU-ONLY-EXPERIMENTAL` must
   both be on the make command line. An environment or makefile value is refused.
2. `BUILD` must be a directory whose name contains `contain-test`, and
   `VOS_TEST` / `VOS_TEST_CASE` must not be combined.
3. Only kernel goals are accepted (`all`, `verify`, the image itself). `iso`,
   `uefi-iso`, `run`, `input-iso` and every other packaging target are refused.
4. **Every link checks the binary** (`scripts/check-contain-image.py`): a
   production link fails, and its `vos.bin` is removed, if any executor symbol,
   the option string or the experimental marker is present; the test link fails
   unless the marker and the full executor are. This catches flags smuggled
   through `KERNEL_RUST_EXTRA_FLAGS`, `TEST_DEFS`, stale objects or a replaced
   kernel.
5. The `iso` recipe re-checks the kernel it packages.

Evidence: 13 negative make cases are each refused for the right reason;
a production make with both halves of the executor smuggled in links, is
**rejected by the binary check, and `vos.bin` is removed**; the production kernel
has no executor symbol, no `pci_write*` reference from the xHCI code, no option
string and no marker; the three production builds are byte-identical. An
ordinary `make iso` of the production kernel produced a BIOS ISO with **no
executor trace anywhere in the image**; the same `iso` target given a test
kernel is **refused by its recipe's check and builds no ISO**; CI repeats the
ISO inspection on the production ISOs it builds. (The native runner's own
boot-menu ISOs list the execute option in their GRUB menu by design, so the
production kernel can be shown to ignore it; they are harness images, and their
production kernel payload is separately verified clean.) The test
image prints its identity on every boot:
`VOS-CONTAIN-TEST-IMAGE: QEMU-ONLY EXPERIMENTAL; NEVER FOR PHYSICAL PCS`.

**Runtime environment checks** (all four must pass, in addition to the profile):
the CPUID hypervisor bit with the `TCGTCGTCGTCG` signature (a hypervisor
signature alone is insufficient), the ACPI RSDP OEM ID `BOCHS `, the Q35 host
bridge `8086:29c0` with QEMU's `1af4:1100` subsystem, and the same `1af4:1100`
subsystem on the xHCI. A KVM signature, a hidden hypervisor bit, another OEM ID,
another host bridge or another subsystem refuses with nothing written.

## The write contract

| | W1: disable bus mastering | W2: request controller reset |
| --- | --- | --- |
| Target | PCI configuration of the single qualified function, offset 0x04 | USBCMD at BAR0 + CAPLENGTH (0x40) through the C1 lease |
| Width and value | 16 bits; `old & ~BME` (`0x0007 → 0x0003`) | 32 bits; `old \| HCRST` (`0 → 2`) |
| Immediately before | full fresh observation equal to the first, plus a re-read of command, status, USBCMD, USBSTS **and every port** | the same, with bus mastering required to be off |
| After | read-back (command, status), controller-error check, full re-observation | bounded poll (≤ 1,024 reads), then the post-reset verification |
| Adapter | `xhci_contain_w1`: its own checks, once per boot | `xhci_contain_w2`: order, lease, value, CAPLENGTH, USBCMD and bus-master checks, once per boot |

The adapters each re-check their own preconditions, so a defect in the Rust
policy cannot by itself cause a write. **Post-reset verification** requires
USBCMD = 0 and USBSTS = 1; DNCTRL, CONFIG, CRCR and DCBAAP zero; no port
connected, enabled, in reset or over-current; every interrupter's IMAN, ERSTSZ,
ERSTBA and ERDP zero; PCI command still `0x0003`, status, MSI/MSI-X and BAR
unchanged; PCIe Transactions Pending clear. **A completed reset alone is not
accepted as containment.**

### Terminal states (all final after W1)

| State | Meaning | Writes made |
| --- | --- | --- |
| refused | eligibility, environment, option conflict | 0 |
| pre-W1-drift | state changed between the rehearsal and W1 | 0 |
| W1-failed | W1 refused by its adapter, or the read-back failed | 0 or 1 |
| bus-master-stuck | Bus Master Enable still set after W1 | 1; no W2 |
| PCI-changed | status or another command bit changed | 1; no W2 |
| controller-error | HCE/HSE, run, or CNR after W1 | 1; no W2 |
| contained-bme-only | post-W1 re-observation differs | 1; no W2 |
| pre-W2-drift | state changed before W2, or W2 refused | 1; no W2 |
| reset-stuck | HCRST or CNR still set after the bound | 2; nothing more |
| reset-unverified | reset done, verification failed | 2 |
| **contained** | everything verified | 2 |

No state re-enables bus mastering, retries a write, aborts a reset, or releases
firmware memory. Several states also fail C1's own end-of-session comparison,
which reports a loud integrity failure naming the state.

## Verification

### Hosted (before any native execution)

All in [c3b2-host-3](usb2c3b2-controlled-containment/hosted/c3b2-host-3/result.json):

- **Rust executor tests: 14/14**, driving a model device with 34 injectable
  fault kinds (the verification fault has 14 sub-kinds) against an independent stage-ordered oracle, plus a 20,000-round
  randomized fault-combination test. On every run: at most one PCI and one
  USBCMD write, W1 before W2, W1 only ever clears bus mastering, bus mastering
  never set again, W2 only after a confirmed W1, every poll bounded.
- **C3b.1's 11 tests** still pass against the production core.
- **ASan/UBSan: 51 scenarios, 9,573 checks** on the real PMM, devmap,
  qualification and residue C, the real adapters and the real Rust archive,
  including a drift sweep (a device appearing at every one of the execution's
  reads) and a directed test that pins which stage guards each of the 72 port
  reads. The freeze flag is asserted held at both writes.
- **Mutation controls: 47, all detected** (21 in the Rust core, 20 in the C
  adapters, boot and environment checks, 6 in the platform Rust).
- **Compile-fail controls: 6**, among them: the executor does not exist in the
  production core, and nothing can set bus mastering or write any other register.
- **Static write-path proof:** exactly one `pci_write16` call site and one
  register store in the test image; **the only PCI write reachable from the
  executor is `pci_write16`, and its only caller in that closure is
  `xhci_contain_w1`**.
- **Independent trace verifier**, tested with 28 corrupted traces (missing,
  duplicate, reordered, mis-sized, mis-valued, mis-addressed and extra writes,
  stray or missing consequences), each rejected for the right reason, and
  against 23 real preserved C3b.1 no-write traces.

Several mutation controls survived at first, each for a reason that was a
defect in the control, not in the executor: an equivalent mutant, a control
pointed at a test whose run ends in full success, and three C-adapter controls
masked by a second check (defense in depth), now exercised layer by layer by
directed tests. See the retained failures.

### Native QEMU matrix

Environment: QEMU 10.2.1 (Ubuntu 1:10.2.1+ds-1ubuntu3.2), `q35`, TCG; OVMF
2025.11 and SeaBIOS 1.17.0; GRUB 2 Multiboot2 ISOs. Every boot also checks, from
outside the guest (GDB-stub observer and QEMU trace), that the PMM bitmaps and
firmware page hashes are unchanged from the guard's entry to the end of the GUI
run, that each firmware page has exactly one mapping (the kernel alias), that
armed AHCI pages stay unallocatable, and that no excluded frame is ever
allocated; and, except in the keyboard-attached boots, drives the GUI, `vsh`,
`hello` and Rixie. Peak PMM use is 95 frames, unchanged.

**Test image, execute option** (22 distinct cases pass; "writes" is the
independent verifier's count of xHCI configuration + register writes made by VOS):

| Case | Outcome | Writes | Run |
| --- | --- | --- | --- |
| UEFI idle xHCI | **CONTAINED**: W1 + W2 issued, every read-back verified | **1 + 1** | native-test |
| UEFI idle xHCI + virtio-blk | **CONTAINED** (storage commands run afterwards) | **1 + 1** | native-test |
| UEFI + USB keyboard | refused: interrupts usbsts ports slots residue | 0 + 0 | native-test |
| UEFI + USB keyboard + tablet | refused: same | 0 + 0 | native-test |
| UEFI no controller | refused: unqualified | 0 + 0 | native-test-rerun |
| UEFI NEC controller | refused: unqualified | 0 + 0 | native-test |
| UEFI two controllers | refused: unqualified | 0 + 0 | native-test |
| UEFI, hypervisor bit hidden | refused: environment | 0 + 0 | native-test |
| UEFI, ACPI OEM ID `EVILRS` | refused: environment | 0 + 0 | native-test-rerun-2 |
| UEFI + `vos.usb2-contain=1` | refused: conflicting option (the dry-run itself ran, wrote nothing) | 0 + 0 | native-test-rerun |
| UEFI + `vos.usb2-residue=1` | refused: conflicting option | 0 + 0 | native-test-rerun |
| UEFI + `vos.usb_probe=1` | refused: conflicting option | 0 + 0 | native-test-rerun |
| UEFI + `vos.usb2-qualify=1` | refused: conflicting option | 0 + 0 | native-test-rerun |
| UEFI + `vos.usb2c2-dma=1` | refused: conflicting option | 0 + 0 | native-test |
| UEFI, option absent (`n`, `t`, `r`) | no execution output; dry-run completes; guard valid | 0 + 0 | native-test, -rerun |
| BIOS idle xHCI | refused: not-uefi pci-command ports residue memory | 0 + 0 | native-test |
| BIOS idle xHCI + virtio-blk | refused: same | 0 + 0 | native-test |
| BIOS + USB keyboard (running controller) | refused: not-uefi interrupts running usbcmd usbsts ports residue memory | 0 + 0 | native-test |
| BIOS no controller | refused: not-uefi unqualified | 0 + 0 | native-test |
| BIOS, option absent (`n`) | no execution output | 0 + 0 | native-test |

**The contained boot, from the raw QEMU trace** (the verifier's evidence; the
exact lines are in `native/native-test/uefi-x-qemu-xhci`):

```text
memory_region_ops_write ... addr 0xcf8 value 0x80001004 size 4 name 'pci-conf-idx'   W1: bus 0, device 2, function 0, offset 4
memory_region_ops_write ... addr 0xcfc value 0x3 size 2 name 'pci-conf-data'        W1: 16-bit write of 0x0003
pci_cfg_write qemu-xhci 00:02.0 @0x4 <- 0x3
...
memory_region_ops_write ... addr 0xc000000040 value 0x2 size 4 name 'operational'     W2: 32-bit USBCMD <- HCRST (BAR0 + 0x40)
usb_xhci_oper_write off 0x0000, val 0x00000002
usb_xhci_reset === RESET ===                                                         then 64 slot_disable and 8 link/notify pairs
```

The guest's report agreed: `state=0xd writes=0x2 polls=0x1 w1_old=0x7 w1_new=0x3
w1_readback=0x3 w1_status=0x10 usbsts_w1=0x1 w2_old=0x0 w2_new=0x2 usbcmd=0x0
usbsts=0x1 dnctrl=0x0 config=0x0 ports_active=0x0 irq_regs_clear=0x1 command=0x3
status=0x10 crcr=0x0 dcbaap=0x0`. The independent verifier found exactly one
xHCI configuration write and one register write, **82 reset-consequence events**
(1 operational-write event, 1 reset, 64 slot-disables, 8 link and 8 notify), and
no other VOS-originated controller activity. After the reset the registers read
back as the reset defaults and the controller's ring pointers were zero; the
firmware pages (still excluded, still one mapping, hashes unchanged) were judged as
they were at the guard's entry. VOS then ran the GUI, `vsh`, `hello` and Rixie
normally:
[GUI](usb2c3b2-controlled-containment/native/native-test/uefi-x-qemu-xhci/gui.png),
[shell](usb2c3b2-controlled-containment/native/native-test/uefi-x-qemu-xhci/shell.png).

**Production kernel** (`kernel-4`): the existing 32-boot matrix plus the execute
option on UEFI and BIOS, **34/34 pass**. The production kernel prints nothing
for the execute option and makes no xHCI write in any boot; every other mode
(`n r q d p c t`, across the C3b.1 device matrix) behaves
as in C3b.1, including the dry-run's refusal masks. Screenshots:
[GUI](usb2c3b2-controlled-containment/native/native-prod/uefi-n-qemu-xhci/gui.png),
[shell](usb2c3b2-controlled-containment/native/native-prod/uefi-n-qemu-xhci/shell.png).

**Failed attempts, kept** (see the retained failures): the first test-image run
passed 15 of 23 cases; the other 8 were three gdb-stub handshake timeouts, one
QEMU property that does not exist, and four runner expectation defects. The
corrected runner then passed six of them; `+oem` timed out once more and passed
on a third explicit attempt after a manual run of the same configuration showed
the kernel's verdict was right. There is no automatic retry in the runner.

**Not tested natively:** a subsystem-ID mismatch (QEMU 10.2.1 has no property to
change the xHCI subsystem; covered hosted) and a real KVM environment
(`/dev/kvm` permission denied; the KVM signature is covered hosted and a hidden
hypervisor bit natively).

### Regressions

Run against the production `kernel-4` archive; full outputs are in
[hosted/](usb2c3b2-controlled-containment/hosted) and
[native/](usb2c3b2-controlled-containment/native).

| Suite | Result |
| --- | --- |
| USB.2-C3a guard (with the C3b.1 scenarios) | **PASS.** 17 residue tests; sanitizer over every C3a/C3a.3/C3b.1 scenario |
| USB.2-C3a.2 EFI exclusion sanitizer | **PASS.** 262,450 checks against the new archive (its pure tests are unchanged inputs, reused from C3a.3) |
| USB.2-C3b.1 hosted | **PASS.** 11 tests, 17 mutations, 3 compile-fail controls, no `pci_write*` reference in the production archive or the xHCI objects (one mutation anchor made specific, see failures) |
| USB.2-C1 qualification | **PASS.** 17 tests |
| USB.2-C2 DMA ownership | **PASS.** 9 tests, 6 compile-fail, 3 mutations, sanitizer |
| USB.2-B ownership | **PASS.** Sanitizer and Rust tests |
| USB.2-A portable core (full) | **PASS.** 46 tests, 2/2 compile controls, freestanding builds, stale-handle mutation, 0 unsafe blocks |
| USB.1 probe and input | **PASS** |
| R1 ELF bounds | **PASS** |
| R2-B PS/2 pointer | **PASS.** Hosted differential; native `normal` boots on BIOS and UEFI; native `verify` boots on BIOS and UEFI (16 native cases each, verify kernel built from this tree) |
| Boot, GUI, shell, native apps, Rixie, storage; PMM, firmware-memory, AHCI | **PASS** in both native matrices |

The R2-B native runs and the storage they needed crossed the 566.2 MB stop line
with the owner's approval (logged in `storage-log.txt`), staying within 600 MiB.

## Retained failures

See [failures.json](usb2c3b2-controlled-containment/history/failures.json).

## Limitations and residual risks

- **No hardware effect beyond QEMU.** Reset timing, CNR, bus-master and
  Transactions Pending semantics, DMA drain and MSI behavior of physical
  controllers are unverified. QEMU's synchronous reset proves nothing about them.
- **Real KVM was not tested** (permission denied for `/dev/kvm`); the KVM signature is
  covered by the C harness and a hidden-hypervisor-bit native boot. A native
  subsystem-ID refusal cannot be tested because QEMU 10.2.1 offers no property
  for it; it is covered hosted.
- **The specification gaps above** (PCIe Base, UEFI, a mirrored PCI text).
- **BIOS controllers are never contained**; BIOS boots refuse.
- **No AHCI action.** Armed AHCI pages stay unallocatable (D6).
- **Firmware memory stays excluded** for the whole boot, including after a
  successful containment; nothing reuses it.

## Assessment

**CONDITIONAL GO for merging C3b.2**, on these conditions:

1. **CI is green** (Build smoke, including the new C3b.2 step, and Gitleaks).
   Results are reported on the pull request.
2. **The remaining specification gaps are accepted for QEMU scope**: the PCI
   Express Base and UEFI `ExitBootServices` texts were not available, and the
   PCI Local Bus text is from a mirror. The xHCI 1.2 primary text, which governs
   the sequence, was reviewed.
3. **The scope is understood.** Merging adds a QEMU-only test image path and
   changes the production kernel by 117 B; production and physical-PC images
   cannot contain the executor, and that is checked from the binary on every
   link. Nothing here shows that the sequence is safe on a real controller,
   which can differ in reset timing, Controller Not Ready behavior, pending
   transactions and DMA drain.
4. **Physical hardware, BIOS containment, USB.2-C4 and HID stay unauthorized.**

What the evidence supports: in a verified QEMU 10.2.1 TCG + UEFI environment,
with exactly one qualified idle xHCI, VOS makes exactly two writes in the
authorized order, verifies each, ends every failure in a final state without
re-enabling bus mastering, and leaves firmware memory, the PMM and the AHCI
boundary intact. What it does not support: any claim about physical hardware.

## Reproduction

```sh
rustc="$HOME/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc"
out=build/usb2c3b2-review
make ARCH=x86_64 BUILD=$out/production RUSTC="$rustc" verify
make ARCH=x86_64 VOS_CONTAIN_TEST=1 VOS_CONTAIN_TEST_ACK=QEMU-ONLY-EXPERIMENTAL \
  BUILD=$out/contain-test RUSTC="$rustc" verify
python3 -I -B scripts/test-usb2c3b2-trace.py
python3 -I -B scripts/test-usb2c3b2.py --production-build $out/production --test-build $out/contain-test \
  --rustc "$rustc" --out $out/c3b2
python3 -I -B scripts/test-usb2c3a2-native.py --kernel $out/contain-test/vos.bin --flavour test \
  --expect execution --compress-traces --apps <verified ELFs> --efi <x86_64-efi GRUB modules> \
  --disk <read-only ext2 disk> --out $out/native-test
python3 -I -B scripts/test-usb2c3a2-native.py --kernel $out/production/vos.bin --flavour production \
  --expect containment --compress-traces --apps <verified ELFs> --efi <x86_64-efi GRUB modules> \
  --disk <read-only ext2 disk> --out $out/native-production
```
