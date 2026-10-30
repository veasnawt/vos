# VOS USB.2-C3b.2 Gate 0: hardware-write readiness review

**Read-only assessment, 2026-10-29.** Nothing was implemented, committed or
pushed. No PCI, MMIO or physical-hardware access was performed. C3b.2
implementation remains unauthorized.

**Recommendation: CONDITIONAL GO** to authorize a QEMU-only C3b.2 implementation,
subject to the six conditions in §7.

---

## 1. Baseline integrity

| Check | Result |
| --- | --- |
| Upstream fetched | `origin/main` = `6eb9a30be63e29667a75b9a04c4cafe994f25e3c` (merge of PR #32; parents `55f6693` and `7d206d2`). |
| Merged source equals the accepted candidate | **Yes.** Tree `510a1000…` is identical for merge `6eb9a30` and candidate `7d206d2`. |
| Git integrity | `git fsck --connectivity-only`: clean. |
| Worktrees | All 7 are clean (r2b, c1, c2, c3a, c3a2, c3a3, c3b1), with 0 stashes. None were modified. |
| C3b.1 evidence | Manifest 370/370 files match. `kernel-3` `vos.bin` and archive, `kernel-3-repro` and the verify kernel all match their recorded hashes. |
| Storage | C: has 23.29 GB free, which is 1.82 GB above the 20 GiB reserve. The C3b.1 build tree (513 MB) is untouched. This Gate 0 used 16 MB under `build/usb2c3b2-gate0/` (sources, extracts, this report). |

---

## 2. Authoritative specification findings

Sources, with hashes and provenance, are recorded in
[`sources.json`](sources.json):

- **xHCI 1.2.** *eXtensible Host Controller Interface for USB (xHCI)
  Requirements Specification*, Revision 1.2, May 2019. **Primary**, hosted by
  Intel.
- **PCI LB 3.0.** *PCI Local Bus Specification*, Revision 3.0, February 3, 2004.
  **Primary text from a third-party mirror**; the official copy is PCI-SIG
  members-only, so authenticity was not verified against PCI-SIG.
- **QEMU 10.2.1 source** (installed: 10.2.1+ds-1ubuntu3.2). Model behavior,
  **not normative**.
- **EDK2 XhciDxe (master) and Linux 6.18 `xhci.c`.** Implementation practice,
  **not normative**.

**Not obtained, so these remain explicit gaps:**

- **PCI Express Base Specification (any revision).** It is members-only. The
  only copies found were on unverified download sites, which I did not use.
- **UEFI Specification `ExitBootServices()`.** uefi.org refused automated
  access with HTTP 403.

No secondary source was substituted for either gap.

### 2.1 Normative requirements (xHCI 1.2 unless noted)

| # | Requirement | Citation |
| --- | --- | --- |
| S1 | HCRST: "Software shall not set this bit to '1' when the HCHalted (HCH) bit in the USBSTS register is a '0'. Attempting to reset an actively running host controller may result in undefined behavior." | §5.4.1, Table 5-20, bit 1 (p. 394) |
| S2 | "This bit is cleared to '0' by the Host Controller when the reset process is complete. Software cannot terminate the reset process early by writing a '0' to this bit and shall not write any xHC Operational or Runtime registers until while HCRST is '1'." **Reads are not prohibited.** | §5.4.1 (p. 394) |
| S3 | "**PCI Configuration registers are not affected by this reset.** All operational registers, including port registers and port state machines are set to their initial values." This resolves the open item from C3b.1 Gate 0: BME survives HCRST. | §5.4.1 (p. 394) |
| S4 | "Any transaction currently in progress on the USB is immediately terminated. A USB reset shall not be driven on USB2 downstream ports, however a Hot or Warm Reset shall be initiated on USB3 Root Hub downstream ports." | §5.4.1 (p. 394) |
| S5 | "After initial power-on or HCRST ... all of the Operational and Runtime Registers shall be at their default values, as defined in sections 5.4 and 5.5." Core-well and Aux-well registers are both reset by HCRST. The USB Legacy Support registers are an exception. | §4.23.1 (pp. 340–341) |
| S6 | CNR: "Software shall not write any Doorbell or Operational register of the xHC, other than the USBSTS register, until CNR = '0'. This flag is set by the xHC after a Chip Hardware Reset and cleared when the xHC is ready ...". A Chip Hardware Reset is "a PCI reset input or an optional power-on reset input" (§4.2, footnote 6). Whether **HCRST** re-asserts CNR is not stated normatively. HCRST is only "similar to a Chip Hardware Reset". | §5.4.2, Table 5-21, bit 11 (p. 399); §4.2 (p. 80) |
| S7 | **No normative time limit for HCRST completion.** The only related timing is that R/S = 0 halts within 16 ms (§5.4.1, bit 0), and §4.6.1.1 (p. 107) says to assert HCRST if CRR does not negate within about 5 s. | §5.4.1; search of the full text |
| S8 | **Bus Master Enable:** "The xHC should be Halted, i.e. with the Run/Stop (R/S) bit set to '0', and HCHalted (HCH) verified as being '1' before system software disables bus master activity by clearing the BME bit. If the BME bit is set to '0' when the xHC is running, the xHC may treat this as a Host Controller Error, asserting HCE ('1') and immediately halt ... Recovery from this state will require an HCRST." **This is the normative basis for the order: halted → BME off → HCRST.** | §4.21.2 (p. 336) |
| S9 | RsvdP: "Software shall preserve the value read for writes to bits." RsvdZ: "Software shall use '0' for writes." USBCMD bits 6:4 and 12+ are RsvdP. "System software shall mask all reserved fields ... to '0' before evaluating a register." Refusing when a reserved bit is set is stricter than this rule and is fail-closed. | §5.1.1, Table 5-3 (pp. 370–371); Table 5-20 |
| S10 | CRCR Command Ring Pointer (63:6): "**Reading this field always returns '0'**." RCS, CS and CA also read 0. Only CRR is meaningful on read. | §5.4.5, Table 5-24 (pp. 402–403) |
| S11 | Defaults after HCRST: USBCMD 0. USBSTS has HCH = 1 and CNR "Default = 1" (but see S6). DNCTRL 0, CRCR 0, DCBAAP 0, CONFIG 0. IMAN 0 (IP and IE = 0). ERSTSZ, ERSTBA and ERDP are 0. **IMOD.IMODI defaults to 4000h**, not 0. | §§5.4.1–5.4.7, 5.5.2.1–5.5.2.3 (pp. 393–428) |
| S12 | USBSTS: HSE is set on "PCI Parity error, PCI Master Abort, and PCI Target Abort", and the xHC clears R/S. HCE means an internal error that "requires software to reset and reinitialize". EINT is the OR of IP 0→1 transitions and does not itself interrupt. | §5.4.2 (pp. 398–399) |
| S13 | Legacy ownership (USBLEGSUP) is optional normative. With no Legacy Support capability, the xHCI defines no BIOS/OS semaphore. | §7.1 (pp. 518–521) |
| P1 | PCI LB 3.0, Command bit 2: "Controls a device's ability to act as a master on the PCI bus. A value of 0 disables the device from generating PCI accesses." Bits 11–15 are reserved. Bit 10 is Interrupt Disable. | §6.2.2, Table 6-1 (pp. 218–219) |
| P2 | MSI is "a PCI DWORD memory write transaction". With P1, BME = 0 therefore prevents MSI, as derived for conventional PCI. The explicit PCIe statement on BME and MSI was **not verified**. | §6.8 (p. 231) |
| P3 | "Configuration writes are not posted. Read transactions (Memory, I/O, or Configuration) are not posted." "A read transaction must push ahead of it through the bridge any posted writes ... and pull out ... any posted writes that originated on the opposite side." To confirm a write reached its target, a driver "must complete a read to the same device". | Appendix E, rules 2 and 5 (p. 310); §3.2.5 (pp. 61–62) |
| P4 | Status register writes are write-1-to-clear. A 16-bit Command write at offset 04h therefore cannot touch Status. A 32-bit write at 04h would write both. | §6.2.3 (p. 219) |

**Not verified from primary text.** The explicit PCIe statement on BME and
MSI/MSI-X, and the PCIe Device Status **Transactions Pending** semantics, were not
verified (secondary descriptions say: "set while non-posted requests are
outstanding"). The UEFI obligations at ExitBootServices were not verified either.

### 2.2 QEMU-specific observations (not normative)

Line numbers refer to the QEMU 10.2.1 sources in `downloads/qemu/`.

| # | Observation | Where | Consequence |
| --- | --- | --- | --- |
| Q1 | HCRST is **synchronous**. The USBCMD write calls `xhci_reset()` inside the same access, which sets USBCMD = 0 and USBSTS = HCH. **CNR is never set.** | `hcd-xhci.c` 2975–2989, 2693–2737 | The poll completes on its first read. QEMU cannot exercise the timeout path, which needs a model or fault injection. |
| Q2 | Reset clears CRCR, DCBAAP, CONFIG, DNCTRL and every interrupter: IMAN, **IMOD → 0**, ERSTSZ, ERSTBA, ERDP. | 2703–2732 | IMOD differs from S11 (4000h), so IMOD must not be verified. |
| Q3 | **CRCR reads return the stored pointer** (`crcr_low & ~0xe`, `crcr_high`), contrary to S10. | 2933–2938 | C3a.3's command-ring residue walk and C3b.1's CRCR check work only because QEMU deviates from the spec. On compliant hardware they would see 0. This is a portability finding for C3a, not a QEMU safety issue. |
| Q4 | Reset calls `xhci_port_update()` on every port. That sets `PORTSC = PP | PLS=RxDetect` and then **CSC through `xhci_port_notify` before the running check**. No event is queued because the controller is halted. | 2631–2662, 2615–2627 | After a QEMU reset, every idle port reads CSC = 1. The post-reset port check must not require the change bits to be clear. |
| Q5 | BME is implemented by enabling or disabling the device's bus-master alias region (`pci_set_master`). All xHCI DMA goes through it (`xhci.as = pci_get_address_space`). | `pci.c` 131–135, 1809–1812; `hcd-xhci-pci.c` 182 | With BME = 0, any QEMU xHCI DMA fails with a MEMTX error → `xhci_die()` → **USBSTS.HCE = 1**, where the spec would set HSE. This gives an observable for any DMA attempt after W1. |
| Q6 | QEMU DMA is synchronous. **Transactions Pending is never set**: `PCI_EXP_DEVSTA_TRPND` is defined but never written. | `pcie.c`, `pci_regs.h:537` | The TP check is vacuous on QEMU. It is kept as a refusal for correctness, but it proves nothing about real hardware. |
| Q7 | The Command write mask is IO, MEMORY, MASTER, INTX_DISABLE and SERR. The PCIe capability exists only on an express bus (q35). | `pci.c` 1045–1048; `hcd-xhci-pci.c` 170–173 | Writing 0x0003 changes only BME. The profile already requires the PCIe capability. |

### 2.3 Firmware-ownership evidence (implementation, not normative)

- **EDK2 behavior.** EDK2 XhciDxe registers an ExitBootServices handler
  (`Xhci.c` 1917–1951). It stops polling, calls `XhcHaltHC`, clears BIOS
  ownership and restores the original PCI attributes. It does **not** reset the
  controller or clear BME.
- **Match with observation.** This matches the observed post-ExitBootServices
  state: halted, ring pointers still programmed, BME = 1.
- **Caveat.** The source reviewed is EDK2 master, not the exact ovmf
  2025.11-3ubuntu7.3 source.
- **Why the controller needs containment.** A halted controller with BME = 1
  and live ring pointers is the situation C3b exists to contain.
- **Remaining gap.** The UEFI normative obligations could not be checked (see
  above).

---

## 3. Proposed write contract

There are exactly two device writes, in this order, once per boot, only from a
fresh `DryRunComplete` in the same session.

| | W1: disable bus mastering | W2: request controller reset |
| --- | --- | --- |
| Target | PCI configuration of the **single qualified BDF**, offset 0x04 | BAR0 + CAPLENGTH + 0x00 (USBCMD) through the C1 lease mapping |
| Width | **16 bits.** P4: never 32 bits, which would write the RW1C Status bits. | 32 bits |
| Value | `cmd_read & ~0x0004`. Profile: `0x0007 → 0x0003`. All other bits are preserved, including Interrupt Disable (bit 10), SERR and reserved bits. | `usbcmd_read | 0x2`. Profile: `0x0 → 0x2`. RsvdP is preserved by read-modify-write (S9), and the profile requires USBCMD = 0. |
| Offset validation | BDF equals the C1-qualified function. The offset is a compile-time constant. | CAPLENGTH read in every pass. The profile requires 0x40, so the operational base is the BAR plus 0x40. USBCMD must lie inside the C1 BAR lease (the QEMU register space is 0x4000 bytes; the bound is checked by the existing lease). |
| Immediate preconditions (TOCTOU re-read **just before** the write) | config read16 @0x04 == 0x0007; status read16 @0x06 == 0x0010; USBSTS == 0x1 | USBCMD == 0; USBSTS == 0x1 (HCH = 1, CNR = 0, HSE = 0, HCE = 0); PCI command == 0x0003 |
| Normative basis | S8 (halted before BME clear); P1 | S1 (HCH = 1), S6 (CNR = 0), S9 |
| Read-back | Non-posted config read (P3): command == 0x0003, status == 0x0010. Then a full R1 pass equal to A except BME, with USBSTS still exactly 1: **no HCE (Q5 DMA-attempt signal), no HSE**. | Poll USBCMD.HCRST and USBSTS.CNR with reads only (S2), up to `POLL_BOUND` = 1024. Each read also flushes the posted W2 (P3). |
| Completion | `BusMasterOff` | HCRST = 0 **and** CNR = 0, then the verification below |

**Post-reset verification.** It is read-only. Every item must hold.

1. USBCMD == 0 and USBSTS == 0x1.
2. DCBAAP == 0, CONFIG == 0 and DNCTRL == 0.
3. CRCR reads 0. Note that S10 makes this vacuous on compliant hardware. On QEMU
   it checks the internal pointer (Q3).
4. For every interrupter, IMAN == 0, ERSTSZ == 0, ERSTBA == 0 and ERDP == 0.
   IMOD is **not** checked (S11 vs Q2).
5. For every port: CCS = 0, PED = 0, PR = 0 and OCA = 0. **Change bits are
   ignored**, because QEMU sets CSC (Q4).
6. PCI: command == 0x0003, status == 0x0010, BARs, MSI and MSI-X unchanged, and
   PCIe TP clear.
7. The PMM, devmap and residue registry are unchanged, and the lease is released.

**Never written.** The following are never written:

- Any other PCI configuration offset.
- Any other operational, runtime or doorbell register.
- PORTSC.
- MSI or MSI-X.
- USBSTS: no RW1C clearing.
- The interrupters.

There is no masking write; D1 is valid because interrupts are verified disabled.

**Additional checks proposed beyond C3b.1:**

- **(a)** Immediate TOCTOU re-reads before each write.
- **(b)** QEMU-only defense in depth. In addition to the 1b36:000d identity,
  check that the CPUID hypervisor bit is set and that the vendor signature is
  "TCGTCGTCGTCG" or "KVMKVMKVM\0\0\0".
- **(c)** Subsystem vendor/device match the profile.
- **(d)** Bus/device/function and BAR unchanged since qualification, re-read.
- **(e)** USBSTS.HCE = 0 after W1. Q5 makes this a DMA-attempt detector on QEMU.
- **(f)** Freeze flag held from rehearsal through verification.
- **(g)** No delay is required for QEMU (Q1). Real-hardware delays (Linux and
  EDK2 use 1 ms for some Intel parts) are out of scope and are **not** claimed.

---

## 4. Preconditions and refusal matrix

Every condition must hold at A, R0 and R1, plus the immediate re-reads.

| Group | Refuse when | Source |
| --- | --- | --- |
| Authorization | `vos.usb2-contain-write=1` (proposed name) is absent; or combined with `vos.usb2-contain=1`, `vos.usb2-residue=1`, `vos.usb_probe=1` or C1/C2 options; or already ran this boot | design |
| Platform | Not a hypervisor or not TCG/KVM (new); not UEFI with boot services exited and exactly one EFI map (bit 0) | design |
| Controller | Not exactly one qualified xHCI (bit 1); not the exact QEMU profile, including CAPLENGTH 0x40, extended capabilities and protocol ports (bit 2); the PCI subsystem ID does not match the profile (**new**; not checked today); a Legacy Support capability is present (S13, bit 2) | C1, S13 |
| PCI | Command ≠ 0x0007 (bit 3); status ≠ 0x0010 (bit 4); no PCIe capability (bit 5); TP set (bit 6) | P1, P4 |
| Interrupts | IMAN.IE or IP, USBCMD.INTE, USBSTS.EINT, MSI/MSI-X enabled, or INTx status set (bit 7) | S12, D1 |
| Run state | R/S = 1 or HCH = 0 (bit 8); USBCMD ≠ 0 (bit 9); USBSTS ≠ exactly 0x1, which covers CNR, HSE and HCE (bit 10) | S1, S6, S8 |
| Devices | Any port CCS/PED/PR/OCA/change bit (bit 11); slots or device contexts (bit 12); scratchpads (bit 13) | design |
| Firmware memory | Residue incomplete, any hazard, reservation or second ownership (bit 14); nothing inspected in place (bit 15) | C3a.3 |
| Stability | A pass differs (bit 16); plan ≠ five steps (bit 17); a pass unreadable (bit 18); TOCTOU re-read mismatch (new) | design |

---

## 5. Failure-state and timeout analysis

**Principle.** After W1 has been issued, every outcome is absorbing for the
boot.

- **BME is never set again by VOS.** Re-enabling it would let a controller in an
  unverified state issue DMA to addresses that VOS neither owns nor validated:
  the firmware rings are excluded but still referenced, and the post-reset state
  is unknown.
- **No VOS driver exists to own the controller.** S8 says recovery from BME-off
  error states requires HCRST, and a full recovery requires a Chip Hardware
  Reset, that is, a reboot.
- **Firmware memory stays excluded for the whole boot** in every state,
  including `Contained`.

| Situation | State | Action | Reversible? |
| --- | --- | --- | --- |
| Refusal before W1 (any matrix entry) | `Refused` | Nothing written. Normal boot continues. | n/a (no effect) |
| BME still set after W1 read-back | `BusMasterStuck` | Stop. **No W2** (S1/S8: resetting with BME still on gives no containment). Report. | Device untouched beyond W1. Nothing to undo. |
| Command or status changed in bits other than BME | `IntegrityFault` | Stop. No W2. The boot check fails (visible, not silent). | No |
| Controller state changes between R0 and W1 | `Refused` (CHANGED) | Nothing written. | n/a |
| State changes between W1 and W2 (R1 mismatch) | `ContainedBmeOnly` | Stop. No W2. BME stays 0. | **Irreversible by design** |
| TP set after W1, or HCE/HSE set after W1 (an attempted or failed DMA, Q5/S12) | `ContainedBmeOnly` plus a DMA-attempt flag | Stop. No W2. Report. | Irreversible |
| HCRST still 1 after `POLL_BOUND` | `ResetStuck` | Stop. **Write nothing more** (S2). Reads are allowed but cease. BME stays 0. | Irreversible; needs a reboot |
| HCRST 0 but CNR still 1 after the bound | `ResetStuck` | As above. S6 forbids operational writes, and there are none. | Irreversible |
| Reset completed but verification fails (pointer, CONFIG, IMAN, ERST or PCI) | `ResetUnverified` | Stop. Report the exact register. | Irreversible |
| Unexpected DMA during the session: PMM, page hashes or trace events | Detected by the external observer (gdbstub page hashes; QEMU trace) | The test fails. In-kernel detection is limited to HCE/HSE/TP and residue digests. | — |
| Incomplete firmware-ownership evidence (no exit, multiple maps, legacy capability, residue incomplete) | `Refused` | Nothing written. | n/a |
| AHCI memory exposed (armed AHCI pages allocatable) | Outside C3b.2 (D6) | **The native test is a NO-GO and fails.** The kernel does not act. | — |
| Lease, devmap, PMM or registry mismatch after the session | `IntegrityFault` | The boot check fails. | — |
| Success | `Contained` | The controller is reset with BME = 0 and no driver. Its memory stays excluded. | Until reboot |

**Timeout semantics.** xHCI gives no normative HCRST time (S7).

- **QEMU (Q1).** The reset completes on the first poll, so for QEMU 1024 read
  iterations is ample.
- **Measured at runtime.** The implementation records the poll count.
- **Not exercisable in QEMU.** The timeout path cannot be reached in QEMU, so it
  must be covered by hosted model tests and a C-level fault injection that
  returns HCRST = 1 or CNR = 1 forever.
- **Real hardware is not claimed.** Linux allows 10 s or 250 ms, and EDK2 uses
  an initial stall. The iteration bound is not a time guarantee.

---

## 6. Resource assessment (proposal; ceilings are not changed)

Measured C3b.1 symbol sizes in kernel-3:

| Symbol | Size |
| --- | --- |
| `vos_xhci_contain_v1` | 1,926 B |
| closure | 1,783 B |
| `xhci_contain_boot` | 1,320 B |
| `Live::observe` | 874 B |
| `refusals` | 332 B |
| `Observed::eq` | 330 B |
| `fingerprint` | 194 B |
| rodata names | 347 B |

The existing `pci_write16` (88 B) is reused.

| Component (estimate) | Bytes |
| --- | --- |
| Write tokens: config W1 (one call site of `pci_write16`) and the MMIO USBCMD store through the lease | 120–200 |
| W1 read-back and immediate re-reads | 150–250 |
| Execute path: W1 → R1 → W2, with HCE/TP checks | 300–450 |
| Reset poll and post-reset verification (USBCMD/USBSTS, DCBAAP, CONFIG, DNCTRL, CRCR, 8 ports, interrupters, PCI) | 500–800 |
| Linking the existing failure model (`bus_master_outcome`, `reset_outcome`; currently dead-stripped) | 250–400 |
| Hypervisor check (CPUID) | 60–120 |
| Report fields, state names and verdict strings (C) | 300–450 |
| **Total** | **≈1.7–2.7 KiB** |

**Proposed C3b.2 ceilings, for owner approval:**

- **Code + rodata growth:** 3,072 B. Cumulative C3b would be 8,899 + 3,072 =
  11,971 B, inside the provisional 12,288 B allowance, which is **not** treated
  as approved.
- **Permanent RW:** 32 B (one-shot flag, outcome state, poll count).
- **Largest stack frame:** 512 B, with total chain growth ≤ 256 B. The existing
  chain sums to about 2.1 KiB plus the C1 `qualify` frame. Total chain depth is
  to be measured with an instrumented stack-paint test, which was not done in
  C3b.1.
- **Storage:** a fresh build tree provisioned like C3b.1, at 600 MiB with the stop
  line at 566.2 MB and verified trace compression. C3b.1 actually used 513 MB.
  C: has 1.82 GB above the reserve, so 600 MiB leaves about 1.2 GB.
  Antigravity owns storage, so this needs approval; nothing will be cleaned.

---

## 7. Implementation and verification plan (for the implementation stage)

**Design.** All of these are to be reviewed at implementation:

- **Type-state.** `Rehearsal::complete()` yields an `Armed` token only from
  `DryRunComplete`. `Armed` is consumed by `disable_bus_master()`, which returns
  `BmeOff` or a failure. `BmeOff` is consumed by `request_reset()`, which returns
  `Contained` or a failure. No token is `Clone` or `Copy`, and none can be
  constructed outside the module.
- **Write capabilities.** Two single-purpose C adapters, each referenced from
  exactly one call site:
  - `xhci_contain_w1(bdf)`, fixed at offset 0x04 and 16 bits.
  - `xhci_contain_w2(lease)`, fixed at USBCMD and 32 bits.
- **No generic write in the core.** The portable Rust core keeps
  `forbid(unsafe_code)` and has no generic write.

| Area | Tests |
| --- | --- |
| Hosted Rust | Type-state ordering, as compile-fail controls: no W2 without `BmeOff`, no reuse of `Armed`, no construction from outside. An exact write-list oracle: an independent model of the contract (offsets, widths, values, order) compared with a recording `Sink`. The failure model for every state above. The post-reset verifier against S11 with the Q2/Q4 deviations. A randomized refusal oracle (as in C3b.1) plus the new conditions. |
| Mutations (each must be detected) | Swapped W1/W2 order; a 32-bit W1; W1 clearing an extra bit; W2 literal 0x2 without read-modify-write; skipped immediate re-read; skipped R1; skipped HCE check; BME re-enabled on failure; W2 after `BusMasterStuck`; poll bound ignored; IMOD checked; change bits checked; writes during HCRST = 1; a second execution in one boot. |
| ASan/UBSan (real PMM/devmap/qualification/residue C) | Eligible → `Contained`. Each refusal writes nothing. Fault injection for config read-back mismatch, BME stuck, HCE after W1, HCRST stuck, CNR stuck and verification mismatch. Each test records exactly the writes allowed for its state, for example none after `BusMasterStuck`. Freeze held through the session, plus integrity checks. |
| Static | `nm -u`: exactly one `pci_write16` reference, in the W1 adapter. No `pci_write8/32`. A source scan finds no other write primitive. A map-based link guard. |
| QEMU write trace: exact oracle | After VOS's first serial byte, the allowed device-write events are exactly: (1) one `pci_cfg_write` to the xHCI BDF `@0x4 <- 0x3`, with the CF8/CFC `memory_region_ops_write` size 2; then (2) one `memory_region_ops_write` to BAR0+0x40, size 4, value 0x2, together with `usb_xhci_oper_write 0x0 0x2` and `usb_xhci_reset`. The only allowed consequences are `usb_xhci_port_link` and `usb_xhci_port_notify` (CSC) for each port (Q4). Anything else fails: any other xHCI config or MMIO write, any write before W1 or between W1 and R1, a second reset, `usb_xhci_run`, doorbell, slot, ep or xfer events, or any DMA event. Register provenance: the post-session snapshot must equal the model, and the last-write source must be VOS only for USBCMD and Command. |
| Read-back and timeout negative controls | A QEMU-side negative is impossible (Q1), so hosted and C fault injection cover it. Natively, a **deliberately ineligible** run (`+kbd`) must show **zero** writes, which proves the refusal gate is enforced in the real kernel. |
| BIOS/UEFI matrix | UEFI idle and idle+disk: `Contained`, with exactly W1 then W2. UEFI with kbd/kbd2 attached: refused, 0 writes. BIOS idle, disk and kbd (running): refused, 0 writes, with firmware events recorded separately (as in C3b.1). Also the no-controller, NEC and two-controller cases. Option conflicts with each of the other options refuse with 0 writes. The C3a/C3a.3 `r`/`q`/`d`/`p`/`c` modes are unchanged. `t` (the C3b.1 dry-run) is unchanged and still writes 0. |
| PMM and firmware DMA | Guard-window bitmaps and alloc unchanged. Firmware page hashes are unchanged from guard entry to after the reset and through the GUI run. With BME = 0 and reset, no DMA is possible, so any page change is a failure. Only the kernel alias is mapped. No excluded frame is ever allocated. |
| AHCI residual | The armed AHCI pages stay unallocatable in every boot (D6). Exposure is a NO-GO. |
| Regressions | USB.1, USB.2-A (full), USB.2-B, C1, C2, C3a, C3a.2, C3a.3, C3b.1, R1, R2-B (hosted and native), and GUI/shell/apps/Rixie/storage on both firmwares. |
| Evidence | Genuine QEMU screenshots: GUI and shell after a `Contained` boot. Serial logs. Traces preserved by verified gzip with hashes. Measurements against `6eb9a30` with the same toolchain. Failures retained. Nothing deleted. |

**Recommendation: CONDITIONAL GO** for authorizing the C3b.2 implementation,
**QEMU-only** (UEFI, single idle qemu-xhci). The proposed sequence agrees with
the primary xHCI 1.2 text on every point that matters for QEMU:

- halted before the BME clear (S8);
- HCRST only when HCH = 1 and CNR = 0 (S1, S6);
- HCRST does not touch PCI configuration (S3);
- reads only during reset (S2);
- registers return to their defaults after reset (S5, S11).

**Conditions:**

1. **Specification gaps accepted or closed by the owner.** Three items:
   - The PCIe Base text (BME/MSI and Transactions Pending) is not verified.
   - The UEFI ExitBootServices obligations are not verified.
   - The PCI LB 3.0 text comes from an unverified third-party mirror.

   Either supply the documents or explicitly accept these gaps for QEMU scope.
2. **Budget approved:** 3,072 B code + rodata, 32 B RW, 512 B frame, chain growth
   ≤ 256 B, and storage provisioning for a fresh build tree.
3. **Contract amendments from this review adopted:**
   - post-reset verification adjusted for S10/S11 versus Q2–Q4 (CRCR read
     semantics, IMOD not checked, port change bits ignored);
   - immediate TOCTOU re-reads before each write;
   - HCE/HSE checked after W1;
   - the hypervisor-signature check;
   - a 16-bit W1 only;
   - read-modify-write for W2.
4. **Separate execute option.** It must be one-shot and exclusive with every
   other C-series option.
5. **Exact write-trace oracle as the acceptance gate.** Precisely the two writes,
   in order, plus the documented QEMU consequences.
6. **No claims beyond QEMU.** The reset timing, CNR behavior, TP semantics and
   DMA-drain properties of real controllers stay unverified. Physical controller
   takeover remains unauthorized.

**A finding to carry forward, not blocking QEMU scope.** C3a.3's command-ring
residue walk and the C3b.1 CRCR check depend on QEMU returning CRCR pointers on
read, which the spec says read as 0 (S10/Q3). Any future physical-hardware
generalization of C3a must replace that evidence source, for example with
DCBAAP/ERST plus firmware-map provenance.

**Stopping here.** C3b.2 implementation requires explicit approval.
