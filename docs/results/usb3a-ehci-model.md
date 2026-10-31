# USB.3-A: hosted EHCI qualification and firmware-handoff model

**Verified for PR review, not accepted or merged.** A hosted-only, `no_std`, `#![forbid(unsafe_code)]` Rust model of the
Intel EHCI controllers observed on the Dell OptiPlex 9020. It decides whether a controller matches the modelled shape,
whether a step could be planned from what is known about it, and what abstract writes the firmware handoff, SMI masking,
schedule stop, halt and reset sequence would call for. **It performs none of them**: a write is an `Event` value and the crate
has no way to execute it. It is not referenced by the kernel build, any ISO or any Makefile input, and it changes no
normal VOS behavior. Verification is hosted only. Physical hardware is NOT TESTED for this change, and no QEMU device
was operated either.

Baseline: `main` at `602a1dc7f784e4017d9dc2f0624889f700a9c5a2`, branch `feat/usb3a-ehci-model`. Scope and inputs come from
the owner's USB.3-A authorization and the [physical survey evidence](usb3a-ehci-model/evidence/dell-survey-v21-evidence-record.md).

## What the model is (and is not)

| Part | Content |
| --- | --- |
| `kernel/rust/usb/ehci_init/facts.rs` | `Fact<T>`: `Observed`, `Inferred`, `Unknown`, `Contradictory`, and `reconcile`. `EhciFacts`: identity, CAPLENGTH, port and companion counts, the legacy-capability offset, owner bits, USBLEGCTLSTS, USBCMD, USBSTS, CONFIGFLAG, and one inferred `firmware_active`. |
| `types.rs` | Phases, steps, operations, observations, abstract `Event`s, accepted-transition `Rule`s, and named refusal/failure `Reason`s. |
| `machine.rs` | `qualify`, `readiness`, and the `Machine` state machine with its lifecycle invariants (see the module header). |
| `dell.rs` | Fixtures for `8086:8c2d` (00:1a.0) and `8086:8c26` (00:1d.0) with per-field provenance (`Observed`, `ReadOnce`, `Inferred`, `Unobserved`). |
| Not present | Any backend, PCI/MMIO/port/DMA access, schedule programming, enumeration, hub or HID handling, interrupts, xHCI routing writes, and any kernel glue. |

Qualification and readiness are separate. `qualify` only answers "is this the modelled Intel EHCI" from vendor, device, class,
version and port count, so a controller still qualifies when unrelated facts are unknown. `readiness(facts, step)` answers
"could this step be planned": it needs exactly that step's facts to be `Observed` and consistent. Executing a step additionally
needs a **fresh** live observation of the register it will modify (made after the last write touching it), so a static
survey value alone never yields a write.

## Abstract sequence

| Step (from phase) | Needs | Abstract event | Success when observed | Failure |
| --- | --- | --- | --- | --- |
| Handoff (qualified) | legacy capability, owner bits | set the OS-owned semaphore (byte at `cap + 3`) | BIOS bit clear and OS bit set, twice with time between | no release in 1000 ticks (`firmware-no-release`); firmware clears the OS bit (`firmware-revoked`). The model never clears the BIOS bit or forces ownership. |
| Mask SMI (handed-off) | USBLEGCTLSTS | write with all enable bits clear and status bits 0 | all enables clear | still set after 100 ticks (`smi-not-disabled`) |
| Stop schedules (smi-masked) | CAPLENGTH, USBCMD, USBSTS | USBCMD with periodic and asynchronous enables clear | both schedule status bits clear | 20 ticks (`schedule-stop-timeout`) |
| Halt (quiet) | same; schedules observed off | USBCMD with Run/Stop clear | HCHalted set | 20 ticks (`halt-timeout`); never proceeds to reset |
| Reset (halted) | same; companions observed 0; halted and Run/Stop clear observed | USBCMD with HCRESET set | HCRESET clear, halted, CONFIGFLAG clear | 250 ticks (`reset-timeout`); CONFIGFLAG still set after HCRESET clears (`reset-unverified`) |

Controllers without a legacy capability skip the first two steps (`handoff.not-needed`, `smi.not-needed`); an already-owned
controller, already-masked SMIs, already-off schedules and an already-halted controller each skip their write. A host system
error or a failed read ends any active phase. Every `Failed` and `Refused` phase is terminal and accepts nothing; nothing is
retried automatically. Time is abstract (one tick is a millisecond of the backend's clock).

**Reset side effect, not modelled further:** a reset clears CONFIGFLAG, so the EHCI owns no ports afterwards. Setting it again
and everything after it (schedules, enumeration) is outside this increment.

## Verification

| Check | Result |
| --- | --- |
| Hosted gate `scripts/test-usb3a.py` | **PASS** locally: [result](usb3a-ehci-model/evidence/gate-result.json). It also runs as one step of the existing Build smoke job (see CI below). |
| Static isolation | Model is `no_std` and `forbid(unsafe_code)`; 13 banned constructs (unsafe, extern, asm, std, alloc, `core::ptr`, volatile, `core::arch`, `no_mangle`, `#[link]`, register-style `fn read*`/`write*`, `static mut`, I/O traits) are absent; `Makefile`, `user/`, `arch/`, `kernel/` and `tools/` do not mention the crate, and no `#[path]` module of the production Rust root reaches it. |
| Compile-fail controls | 4/4: machine state cannot be forged, a machine cannot be built by hand, an `Event` has no way to be performed, and unsafe code is rejected. |
| Rust tests | 17/17: both Dell fixtures and their provenance; qualification versus readiness; the full abstract sequence on each Dell controller (exact events: `0x68` semaphore, SMI control value 0, USBCMD `0x01`, `0x00`, `0x02`); firmware non-release and revocation; SMI refusal and timeout; schedule, halt and reset timeouts; unverified reset; host error and read failure; missing, inferred, contradictory and malformed facts; stale observations; ordering and terminal phases; no-legacy and already-owned paths; companion refusal; a seeded safety property over 3,000 streams of 60 operations (60% of the operations guided deep into the protocol) checked against an independent tracker; rule and reason coverage. |
| Independent reference | A second implementation written from the register definitions (`scripts/ehci_init_oracle.py`) agrees with the Rust model on 300 exported traces, 18,300 steps, covering every kind of write and failure; it detects a corrupted trace. |
| Bounded mutation check | 17 mutants of single safety decisions (register bit positions, ordering, freshness, timeouts, refusal rules): 17/17 killed, 1 only by the Python reference (`handoff-timeout-moved`, because the Rust tests use the model's own constant). No larger campaign was run. |
| Production kernel build | Not run. A standalone hosted crate that no build input names needs no kernel build; the static gate checks the build inputs directly. |
| CI | The first CI run on the original head passed all three required checks without this gate. A follow-up commit adds exactly one step, `Verify USB.3-A hosted EHCI qualification and handoff model`, to the existing Build smoke job (no new workflow or job); its GitHub Actions step result is recorded in the PR discussion. |

Failed attempts: the first gate run failed because one compile-fail control expected an error code that rustc does not print for
that error; the expectation was corrected and the run repeated (the failed run's output was kept in the build tree).

## Dell fixture provenance

Source: one owner photograph of the survey screen; image `vos-usb-survey-dual.iso` built from `602a1dc7f784`. The
photograph is **not** published here. The transcription is of a photograph, not an independent register measurement. See the
[evidence record](usb3a-ehci-model/evidence/dell-survey-v21-evidence-record.md) and the
[transcription](usb3a-ehci-model/evidence/dell-survey-v21-transcription.md).

- Observed: vendor, device, class, version, 2 ports, 0 companions, legacy capability at `0x68`, BIOS-owned 1 / OS-owned 0,
  USBCMD `0x11`, USBSTS `0x600a` (8c2d) and `0x6008` (8c26), CONFIGFLAG 1.
- Read once, not cross-checked: USBLEGCTLSTS `0x000a2005` (8c2d) and `0x00002005` (8c26).
- Inferred: firmware is actively using each controller.
- Unobserved, left `Unknown`: CAPLENGTH, HCSPARAMS/HCCPARAMS raw, raw USBLEGSUP, USBINTR, FRINDEX, list base registers, and
  what sits behind each connected root port. The model does not identify a keyboard or mouse from a root port.
- DMA isolation is not established (IOMMU translation off), and the model says nothing that suggests otherwise.

One consequence the fixtures show: on the Dell, handoff and SMI masking can be planned from the survey, but schedule stop,
halt and reset are blocked until CAPLENGTH is observed.

## Limits and assumptions

- Hosted model only; the readiness rules and timeouts follow the EHCI 1.0 text and common practice, and have not been checked
  against real silicon. The stability requirement for the handoff (two matching observations) is a conservative choice of this
  model, not a specification rule.
- The survey did not print the unobserved fields above; a read-only survey v2.2 could, with separate approval.
- A real backend still has to decide how observations are made, how long a tick is, and how to recover when a controller ends in
  `Failed` mid-handoff (there is no machine reset path in VOS today).
- Writes to PCI configuration space or EHCI registers on the Dell remain unauthorized.

## Path toward USB keyboard and mouse

1. (Owner) Inspect the BIOS USB options without changing them. This cannot be assumed to expose devices to the xHCI.
2. This model (done, hosted).
3. Optional read-only survey v2.2 for the unobserved EHCI fields and the RMRR device scopes.
4. A real backend and the first physical handoff, halt and reset on one EHCI: separate authorization, recovery design,
   and a way to observe each step.
5. EHCI schedules and DMA ownership in kernel-owned memory (no IOMMU isolation is available on this machine).
6. Hub enumeration (probably the internal hub) with split transactions for low- and full-speed devices behind it.
7. HID boot keyboard to `vsh` through the existing input seam, then the mouse to the GUI pointer session.
