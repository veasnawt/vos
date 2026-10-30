#!/usr/bin/env python3
"""USB.2-C4.1 hosted verification of the xHCI initialization protocol model.

Everything here is host-only: no QEMU, no kernel build, no device, PCI or MMIO access. The model lives
in kernel/rust/usb/xhci_init/ and is not referenced by any kernel, ISO or Makefile input.

Gates, in order (the first failure stops the run and keeps its logs):
  1. static isolation: the model has no unsafe/extern/asm/std/alloc, is #![no_std] and #![forbid(unsafe_code)],
     and no kernel, build or ISO input mentions it;
  2. compile-fail controls: the old broad Program action is gone, state cannot be forged from outside the
     crate, and unsafe code is rejected;
  3. the Rust test-suite (deterministic scenarios, refusal/failure matrices, seeded randomized transitions
     with a device-safety property, write-order checks, rule/reason completeness);
  4. the independent Python oracle replays exported traces and must agree on every step, and it is shown to
     detect corrupted traces;
  5. mutation controls: every mutant of the model must be killed by the Rust tests or by the oracle.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODEL = ROOT / 'kernel/rust/usb/xhci_init'
TESTS = ROOT / 'tests/rust'
ORACLE = ROOT / 'scripts/xhci_init_oracle.py'
SUPPORT = TESTS / 'xhci_init_support.rs'
SUITE = TESTS / 'xhci_init_model.rs'

# (name, what it breaks, old text, new text) -- every `old` must occur exactly once in machine.rs
M = []


def mut(name, why, old, new, file='machine.rs'):
    M.append((name, why, old, new, file))


mut('erstba-skips-bme-verification', 'ERSTBA accepted without a verified BME read-back',
    "            Phase::BusMastering => {\n                if !self.bme_verified {\n                    return Err(Reason::BusMasterUnverified);\n                }\n                let p = self.pages[PageKind::Erst as usize];",
    "            Phase::BusMastering => {\n                let p = self.pages[PageKind::Erst as usize];")
mut('erstba-allowed-without-bme', 'ERSTBA accepted in Programmed (BME still clear)',
    "            Phase::BusMastering => {\n                if !self.bme_verified {", "            Phase::BusMastering | Phase::Programmed => {\n                if !self.bme_verified {")
mut('bme-set-while-programming', 'BME enable accepted before the static registers are complete',
    "            Phase::Programmed => {\n                if self.bme {\n                    return Err(Reason::BusMasterAlreadySet);",
    "            Phase::Programmed | Phase::Programming => {\n                if self.bme {\n                    return Err(Reason::BusMasterAlreadySet);")
mut('run-without-status-check', 'R/S accepted without the post-ERSTBA status read',
    "                if !self.status_clean {\n                    return Err(Reason::StatusNotChecked);\n                }\n", "")
mut('reset-with-bme', 'HCRST accepted while BME is set',
    "                if self.bme {\n                    return Err(Reason::ResetWithBusMaster);\n                }\n", "")
mut('reset-without-bme-verification', 'HCRST accepted before the BME clear was read back',
    "                if !self.bme_verified {\n                    return Err(Reason::BusMasterUnverified);\n                }\n                let id = if self.exposed() { \"N-18\" }", "                let id = if self.exposed() { \"N-18\" }")
mut('bme-clear-before-halt', 'BME clear accepted while the controller may still run',
    "            Phase::Halted => {\n                if !self.bme {\n                    return Err(Reason::BusMasterAlreadyClear);",
    "            Phase::Halted | Phase::Halting | Phase::Running => {\n                if !self.bme {\n                    return Err(Reason::BusMasterAlreadyClear);")
mut('ack-without-ehb', 'ERDP acknowledgment accepted without EHB', "        if !ehb {\n            return Err(Reason::EhbNotSet);\n        }\n", "")
mut('ack-repeat-allowed', 'the same ERDP value may be written twice', "        if addr == self.last_erdp {\n            return Err(Reason::ErdpRepeat);\n        }\n", "")
mut('ack-address-unchecked', 'ERDP need not match the next TRB', "        if addr != self.expected_erdp() {\n            return Err(Reason::ErdpMismatch);\n        }\n", "")
mut('doorbell-ignores-unacked', 'doorbell accepted with unacknowledged events', "        if self.unacked > 0 {\n            return Err(Reason::AckMissing);\n        }\n        self.staged = None;", "        self.staged = None;")
mut('doorbell-ignores-missing-link', 'doorbell accepted before the Link TRB', "        if self.link_required {\n            return Err(Reason::LinkMissing);\n        }\n", "")
mut('link-toggle-unchecked', 'Link TRB without Toggle Cycle accepted', "        if !toggle {\n            return Err(Reason::LinkToggleMissing);\n        }\n", "")
mut('link-cycle-unchecked', 'Link TRB with the wrong cycle accepted',
    "        if cycle != self.pcs {\n            return Err(Reason::CommandCycleMismatch);\n        }\n        if !toggle {", "        if !toggle {")
mut('noop-cycle-unchecked', 'No-Op with the wrong cycle accepted',
    "        if cycle != self.pcs {\n            return Err(Reason::CommandCycleMismatch);\n        }\n        self.staged = Some(index);", "        self.staged = Some(index);")
mut('noop-index-unchecked', 'No-Op index need not be the enqueue position', "        if index != self.enq || index > RING_TRBS - 2 {", "        if index > RING_TRBS - 2 {")
mut('producer-cycle-not-toggled', 'command producer cycle not toggled at the Link', "        self.enq = 0;\n        self.pcs = !self.pcs;\n", "        self.enq = 0;\n")
mut('consumer-cycle-not-toggled', 'event consumer cycle not toggled at the wrap', "            self.deq = 0;\n            self.ccs = !self.ccs;\n", "            self.deq = 0;\n")
mut('completion-pointer-unchecked', 'completion for another TRB accepted',
    "                if t.pointer != self.pages[PageKind::Command as usize].addr + 16 * u64::from(index) {\n                    return self.fail(Reason::CompletionPointerMismatch, Rule::FaultEvent);\n                }\n", "")
mut('completion-code-unchecked', 'failed completion code accepted', "                if t.code != 1 {\n                    return self.fail(Reason::CommandFailed, Rule::FaultEvent);\n                }\n", "")
mut('completion-fields-unchecked', 'completion with slot/VF/parameter accepted',
    "                if t.slot != 0 || t.vf != 0 || t.param != 0 {\n                    return self.fail(Reason::CompletionFieldInvalid, Rule::FaultEvent);\n                }\n", "")
mut('poll-budget-removed', 'unbounded events per poll step', "        if self.step_events >= POLL_BUDGET {\n            return Err(Reason::PollBudgetExceeded);\n        }\n", "")
mut('unacked-limit-removed', 'unbounded unacknowledged events', "        if self.unacked >= UNACKED_LIMIT {\n            return Err(Reason::AckMissing);\n        }\n", "")
mut('scratchpads-allowed', 'controller needing scratchpads qualifies', "        } else if f.scratchpads > 0 {\n            Some(Reason::ScratchpadsRequired)\n", "        } else if false {\n            Some(Reason::ScratchpadsRequired)\n")
mut('dcbaa-entry0-allowed', 'nonzero DCBAA entry 0 accepted', "                if entry0 != 0 { Err(Reason::ScratchpadPointerUnsupported) } else { Ok(()) }", "                if false { Err(Reason::ScratchpadPointerUnsupported) } else { Ok(()) }")
mut('unknown-iommu-accepted', 'unresolved IOMMU treated as absent', "        } else if f.iommu == Tri::Unknown {\n            Some(Reason::IommuUnresolved)", "        } else if false {\n            Some(Reason::IommuUnresolved)")
mut('present-iommu-accepted', 'present IOMMU accepted', "        } else if f.iommu == Tri::Yes {\n            Some(Reason::IommuPresent)", "        } else if false {\n            Some(Reason::IommuPresent)")
mut('unknown-identity-dma-accepted', 'unresolved identity DMA accepted', "        } else if f.identity_dma == Tri::Unknown {\n            Some(Reason::IdentityDmaUnresolved)", "        } else if false {\n            Some(Reason::IdentityDmaUnresolved)")
mut('denied-identity-dma-accepted', 'denied identity DMA accepted', "        } else if f.identity_dma == Tri::No {\n            Some(Reason::IdentityDmaDenied)", "        } else if false {\n            Some(Reason::IdentityDmaDenied)")
mut('unknown-no-snoop-accepted', 'unresolved No Snoop accepted', "        } else if f.no_snoop == Tri::Unknown {\n            Some(Reason::NoSnoopUnresolved)", "        } else if false {\n            Some(Reason::NoSnoopUnresolved)")
mut('unknown-dbc-accepted', 'unresolved Debug Capability accepted', "        } else if f.dbc == Dbc::Unknown {\n            Some(Reason::DbcUnresolved)", "        } else if false {\n            Some(Reason::DbcUnresolved)")
mut('enabled-dbc-accepted', 'enabled Debug Capability accepted', "        } else if f.dbc == Dbc::Enabled {\n            Some(Reason::DbcEnabled)", "        } else if false {\n            Some(Reason::DbcEnabled)")
mut('msi-accepted-at-qualification', 'MSI/MSI-X enabled controller qualifies', "        } else if f.msi {\n            Some(Reason::MsiEnabled)", "        } else if false {\n            Some(Reason::MsiEnabled)")
mut('attached-device-accepted', 'attached device ignored at qualification', "        } else if f.connected != 0 {\n            Some(Reason::DeviceAttached)", "        } else if false {\n            Some(Reason::DeviceAttached)")
mut('ownership-conflict-accepted', 'both semaphores set accepted', "        } else if matches!(f.legacy, Legacy::Present { bios: true, os: true }) {\n            Some(Reason::OwnershipConflict)", "        } else if false {\n            Some(Reason::OwnershipConflict)")
mut('qualification-fault-accepted', 'HSE/HCE at qualification ignored', "        } else if f.hse || f.hce {\n            Some(Reason::ControllerFault)", "        } else if false {\n            Some(Reason::ControllerFault)")
mut('wrong-profile-accepted', 'unqualified platform accepted', "        let refusal = if f.profile != Profile::Qemu1021 {", "        let refusal = if false {")
mut('handoff-deadline-missing', 'no deadline on the BIOS release', "        self.arm(Reason::HandoffTimeout, HANDOFF_TICKS);\n", "")
mut('halt-deadline-missing', 'no deadline on the initial halt', "                self.arm(Reason::HaltTimeout, HALT_TICKS);\n", "")
mut('reset-deadline-missing', 'no deadline on HCRST completion', "                self.arm(Reason::ResetTimeout, RESET_TICKS);\n", "")
mut('start-deadline-missing', 'no deadline on Run', "                self.arm(Reason::StartTimeout, START_TICKS);\n", "")
mut('command-deadline-missing', 'no deadline on a command', "        self.arm(Reason::CommandTimeout, COMMAND_TICKS);\n", "")
mut('deadline-off-by-one', 'deadline expires one tick late', "            && t - start >= ticks", "            && t - start > ticks")
mut('clock-backwards-ignored', 'a clock that goes backwards is accepted', "        if t < self.now {\n            return self.fail(Reason::ClockBackwards, Rule::FaultClock);\n        }\n", "")
mut('exposure-ignores-pages', 'published pages do not count as exposure', "        self.armed || self.pages.iter().any(|p| p.state == PageState::Published)", "        self.armed")
mut('failure-never-quarantines', 'exposed failures end as clean failures', "let to = if self.exposed() { Phase::Quarantined } else { Phase::Failed };", "let to = Phase::Failed;")
mut('tail-trusts-stale-halt', 'quarantine tail clears BME because of an old halted read', "            if self.rs {\n                Tail::NeedHalt\n            } else if self.hch {\n                Tail::NeedBmeClear\n            } else {\n                Tail::AwaitHalt\n            }", "            if self.hch {\n                Tail::NeedBmeClear\n            } else if self.rs {\n                Tail::NeedHalt\n            } else {\n                Tail::AwaitHalt\n            }")
mut('run-start-keeps-halted-belief', 'halted belief survives setting R/S', "                self.hch = false; // the last \"halted\" observation no longer holds once R/S is set\n", "")
mut('tail-skips-halt', 'quarantine tail may clear BME without halting', "            (Tail::NeedBmeClear, Operation::ClearBusMaster) => {", "            (Tail::NeedHalt | Tail::NeedBmeClear, Operation::ClearBusMaster) => {")
mut('quarantine-releases-pages', 'pages can be released from a quarantined controller', "            (Phase::Failed | Phase::Quiesced, Operation::DmaRelease(k)) => self.release(k),", "            (Phase::Failed | Phase::Quiesced | Phase::Quarantined, Operation::DmaRelease(k)) => self.release(k),")
mut('published-release-before-quiescence', 'published page released before quiescence', "                if self.phase != Phase::Quiesced {\n                    Err(Reason::PageStillPublished)", "                if false {\n                    Err(Reason::PageStillPublished)")
mut('release-policy-ignored', 'physical never-release policy ignored', "                } else if self.policy == ReleasePolicy::Never {\n                    Err(Reason::ReleaseForbiddenByPolicy)\n                } else {", "                } else if false {\n                    Err(Reason::ReleaseForbiddenByPolicy)\n                } else {")
mut('allocation-open-after-exposure', 'allocation allowed while stopping an exposed device', "            Phase::Halting | Phase::Halted | Phase::Resetting if !self.exposed() => Ok(()),", "            Phase::Halting | Phase::Halted | Phase::Resetting => Ok(()),")
mut('misaligned-page-accepted', 'unaligned DMA page accepted', "        if addr % PAGE != 0 {\n            return Err(Reason::Misaligned);\n        }\n", "")
mut('low-address-accepted', 'page below 1 MiB accepted', "            Some(end) if addr >= MIN_ADDR && end <= limit => {}", "            Some(end) if end <= limit => {}")
mut('high-address-accepted', 'page beyond the DMA limit accepted', "            Some(end) if addr >= MIN_ADDR && end <= limit => {}", "            Some(_) if addr >= MIN_ADDR => {}")
mut('overlap-accepted', 'overlapping DMA pages accepted', "        if self.pages.iter().any(|p| p.state != PageState::Free && p.addr.abs_diff(addr) < PAGE) {\n            return Err(Reason::PageOverlap);\n        }\n", "")
mut('double-allocation-accepted', 'a page can be allocated twice', "        if self.pages[kind as usize].state != PageState::Free {\n            return Err(Reason::PageAlreadyOwned);\n        }\n", "")
mut('page-state-unchecked', 'any page state may be zeroed/prepared', "        } else if s != want {\n            Err(Reason::PageState)\n        } else {", "        } else if false {\n            Err(Reason::PageState)\n        } else {")
mut('erst-size-unchecked', 'ERST segment size not validated', "                } else if size != RING_TRBS {\n                    Err(Reason::ErstSegmentSizeUnsupported)\n                } else {", "                } else if false {\n                    Err(Reason::ErstSegmentSizeUnsupported)\n                } else {")
mut('erst-segment-unchecked', 'ERST segment need not be the event ring', "                } else if segment != event.addr {\n                    Err(Reason::ErstSegmentMismatch)", "                } else if false {\n                    Err(Reason::ErstSegmentMismatch)")
mut('erstsz-unchecked', 'ERSTSZ other than 1 accepted', "        if n != 1 {\n            return Err(Reason::ErstSizeInvalid);\n        }\n", "")
mut('slots-unchecked', 'any MaxSlotsEn accepted', "        if n != 1 {\n            return Err(Reason::SlotsInvalid);\n        }\n", "")
mut('crcr-allowed-while-running', 'CRCR rewrite accepted while the controller runs', "        if matches!(self.phase, Phase::Starting | Phase::Running) {\n            return Err(Reason::CrcrWhileRunning);\n        }\n", "")
mut('crcr-rcs-unchecked', 'CRCR with RCS=0 accepted', "        if !rcs {\n            return Err(Reason::CycleStateInvalid);\n        }\n", "")
mut('static-register-repeat-allowed', 'MaxSlotsEn written twice', "        if self.written & R_SLOTS != 0 {\n            return Err(Reason::RegisterRepeated);\n        }\n", "")
mut('interrupt-enable-allowed', 'interrupt enable not refused by name', "                Forbidden::InterruptEnable => Reason::ForbiddenInterruptEnable,", "                Forbidden::InterruptEnable => Reason::WrongPhase,")
mut('port-mask-unchecked', 'port mask beyond the port count accepted', "        if connected & !valid != 0 {\n            return Err(Reason::PortMaskInvalid);\n        }\n", "")
mut('late-attachment-fails', 'attachment after Run treated as a failure', "        if self.phase == Phase::Running {\n            self.devices |= connected;", "        if false {\n            self.devices |= connected;")
mut('port-event-range-unchecked', 'port change event for a nonexistent port accepted', "                if t.port == 0 || t.port > ports {\n                    return self.fail(Reason::PortOutOfRange, Rule::FaultEvent);\n                }\n", "")
mut('msi-observation-ignored', 'MSI observed after qualification ignored', "        if msi {\n            return self.fail(Reason::MsiEnabled, Rule::FaultMsi);\n        }\n", "")
mut('bme-mismatch-ignored', 'wrong BME read-back ignored', "        if bme != self.bme {\n            return self.fail(Reason::BusMasterMismatch, Rule::FaultBmeMismatch);\n        }\n", "")
mut('cancel-in-running', 'Cancel accepted while running (skips the R/S halt)', "            Phase::Starting | Phase::Running => Err(Reason::UseClearRunStop),", "            Phase::Starting | Phase::Running => {\n                self.deadline = None;\n                self.acc(Phase::Halted, Rule::CancelHalt, None)\n            }")
mut('reset-ports-unverified', 'reset accepted with a port still reporting a device', "r.hch && r.usbcmd_zero && r.pointers_zero && !r.bme && r.ports_quiet", "r.hch && r.usbcmd_zero && r.pointers_zero && !r.bme")
mut('reset-pointers-unverified', 'reset accepted with controller pointers not cleared', "r.hch && r.usbcmd_zero && r.pointers_zero && !r.bme && r.ports_quiet", "r.hch && r.usbcmd_zero && !r.bme && r.ports_quiet")
mut('reset-ignores-cnr', 'reset verification while CNR is still set', "        if r.cnr {\n            return self.stay(Rule::ResetPending);\n        }\n", "")
mut('bios-regain-ignored', 'BIOS taking the controller back is ignored after the handoff', "        _ => {\n                if bios {\n                    self.fail(Reason::OwnershipConflict, Rule::FaultOwnership)\n                } else if !os {", "        _ => {\n                if false {\n                    self.fail(Reason::OwnershipConflict, Rule::FaultOwnership)\n                } else if !os {")
mut('refusal-mutates-state', 'a refused status observation still updates state', "    fn observe_status(&mut self, s: Status) -> Result<Accepted, Reason> {\n", "    fn observe_status(&mut self, s: Status) -> Result<Accepted, Reason> {\n        self.hch = s.hch;\n")
mut('smi-before-handoff', 'SMI disable accepted during the handoff', "            Phase::Handoff => return Err(Reason::SmiBeforeOwnership),", "            Phase::Handoff => true,")
mut('halt-when-halted', 'ClearRunStop accepted when R/S is already clear', "                if !self.rs {\n                    return Err(Reason::AlreadyHalted);\n                }\n                self.rs = false;\n                self.hch = false; // halted", "                self.rs = false;\n                self.hch = false; // halted")
mut('controller-error-ignored', 'HSE/HCE observations ignored', "        let fault = s.hse || s.hce;", "        let fault = false;")
mut('unexpected-halt-ignored', 'controller halting itself while running ignored', "            Phase::Running => {\n                if s.hch {\n                    self.fail(Reason::UnexpectedHalt, Rule::FaultUnexpectedHalt)", "            Phase::Running => {\n                if false {\n                    self.fail(Reason::UnexpectedHalt, Rule::FaultUnexpectedHalt)")
mut('command-deadline-not-cleared', 'completion leaves the command deadline armed', "                self.pending = None;\n                self.deadline = None;\n                self.noops += 1;", "                self.pending = None;\n                self.noops += 1;")
mut('poll-step-does-not-reset', 'BeginPollStep does not reset the per-step budget', "                self.need_running()?;\n                self.step_events = 0;\n", "                self.need_running()?;\n")
mut('erdp-stride-wrong', 'expected ERDP uses an 8-byte stride', "self.pages[PageKind::Event as usize].addr + 16 * u64::from(self.deq)", "self.pages[PageKind::Event as usize].addr + 8 * u64::from(self.deq)")
mut('wrong-inventory-id', 'doorbell event carries the wrong inventory id', 'ev("N-14", Target::Doorbell0, 32, 0)', 'ev("N-41", Target::Doorbell0, 32, 0)')
mut('wrong-hcrst-value', 'HCRST event writes the wrong value', 'ev(id, Target::Hcrst, 32, 2)', 'ev(id, Target::Hcrst, 32, 1)')
mut('wrong-bme-event-id', 'exit-path BME clear carries the init-path id', 'let id = if self.exposed() { "N-17" } else { "N-02" };', 'let id = "N-02";')
mut('erdp-ack-clears-ehb-bit', 'ERDP acknowledgment does not set EHB', 'ev("N-15", Target::Erdp, 64, addr | 8)', 'ev("N-15", Target::Erdp, 64, addr)')
mut('crcr-event-drops-rcs', 'CRCR event loses RCS', 'ev("N-08", Target::Crcr, 64, addr | 1)', 'ev("N-08", Target::Crcr, 64, addr)')
mut('qualified-twice', 'a second qualification is accepted', "            Operation::Qualify(_) => Err(Reason::AlreadyQualified),", "            Operation::Qualify(f) => self.qualify(f),")
mut('terminal-accepts-ticks', 'time still advances after a terminal failure', "            _ => Err(Reason::AfterTerminal),\n        }\n    }\n\n    /// The fixed containment tail", "            (_, Operation::Tick(t)) => self.tick(t),\n            _ => Err(Reason::AfterTerminal),\n        }\n    }\n\n    /// The fixed containment tail")
mut('forbidden-checked-after-phase', 'forbidden writes refused only after the idle gate', "        if let Operation::Forbidden(f) = op {\n            return Err(match f {", "        if let (Operation::Forbidden(f), false) = (op, self.phase == Phase::Idle) {\n            return Err(match f {")
mut('legacy-request-without-belief-update', 'ownership request does not record the OS semaphore', "        if let Legacy::Present { bios, .. } = self.legacy {\n            self.legacy = Legacy::Present { bios, os: true };\n        }\n", "")

CF = {
    'old-broad-program-action-is-gone': ('use vos_xhci_init::*;\nfn main() { let _ = Operation::Program { command: 0, event: 0, dcbaa: 0, erst: 0 }; }\n', 'E0599'),
    'state-cannot-be-forged': ('use vos_xhci_init::*;\nfn main() { let mut m = Machine::new(ReleasePolicy::Never); m.phase = Phase::Running; }\n', 'E0616'),
    'registers-have-no-raw-write': ('use vos_xhci_init::*;\nfn main() { let _ = Operation::WriteRegister { offset: 0x38, value: 1 }; }\n', 'E0599'),
}
BANNED = re.compile(r'\bunsafe\b|\bextern\b|\basm!|\bglobal_asm!|\balloc::|\bstd::|\bstatic\s+mut\b|\bconst\s+fn\s+\w+\s*\([^)]*\*(const|mut)')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--seeds', type=int, default=600, help='seeds exported for the main oracle run')
    ap.add_argument('--mutant-seeds', type=int, default=120)
    ap.add_argument('--only', help='comma-separated mutant names (debugging)')
    ap.add_argument('--workers-dir', type=Path, help='reuse these mutant build directories in place (overwritten; they hold build caches, not evidence)')
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    report = {'hosted_only': True, 'device_access': False, 'pci_mmio_physical_memory_access': False,
              'source_sha256': {}, 'commands': []}
    for p in sorted(MODEL.glob('*.rs')) + [SUPPORT, SUITE, ORACLE, Path(__file__)]:
        report['source_sha256'][str(p.relative_to(ROOT))] = sha(p)

    def run(argv, name, expected=0, env=None, cwd=ROOT, timeout=900):
        argv = list(map(str, argv))
        report['commands'].append(argv)
        with (out / name).open('x') as f:
            p = subprocess.run(argv, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout,
                               env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', **(env or {})))
        if expected is not None and p.returncode != expected:
            raise RuntimeError(f'{name}: exit {p.returncode}, expected {expected}')
        return (out / name).read_text()

    rust = [args.rustc, '--edition=2024']
    started = time.time()
    try:
        # 1. static isolation -------------------------------------------------------------------
        for p in sorted(MODEL.glob('*.rs')):
            hit = BANNED.search(p.read_text())
            # 'unsafe_code' appears only inside the forbid attribute
            text = p.read_text().replace('forbid(unsafe_code)', '')
            hit = BANNED.search(text)
            if hit:
                raise RuntimeError(f'{p.name}: banned construct {hit.group(0)!r}')
        lib = (MODEL / 'lib.rs').read_text()
        if '#![no_std]' not in lib or '#![forbid(unsafe_code)]' not in lib:
            raise RuntimeError('model crate must be #![no_std] and #![forbid(unsafe_code)]')
        mentions = []
        tracked = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
        allowed = re.compile(r'^(kernel/rust/usb/xhci_init/|tests/rust/xhci_init_|scripts/(xhci_init_oracle|test-usb2c41|test-usb2c42a|measure-usb2c41|measure-usb2c42a)|docs/|README\.md|\.github/workflows/build-smoke\.yml|\.gitleaksignore)')
        for f in tracked:
            if allowed.match(f) or not (ROOT / f).is_file() or (ROOT / f).stat().st_size > 2_000_000:
                continue
            try:
                if re.search(r'xhci_init|vos_xhci_init', (ROOT / f).read_text(errors='ignore')):
                    mentions.append(f)
            except OSError:
                pass
        if mentions:
            raise RuntimeError('kernel, build or ISO inputs mention the model: ' + ', '.join(mentions))
        report['static_isolation'] = {'model_files': [p.name for p in sorted(MODEL.glob('*.rs'))], 'unsafe_free_no_std': True,
                                      'files_outside_allowed_set_mentioning_model': mentions,
                                      'kernel_crate_root_mentions_model': 'xhci_init' in (ROOT / 'arch/x86_64/rust/kernel_probe.rs').read_text()}
        if report['static_isolation']['kernel_crate_root_mentions_model']:
            raise RuntimeError('kernel crate root includes the model')

        # build -------------------------------------------------------------------------------
        rlib = out / 'libvos_xhci_init.rlib'
        run(rust + ['-Dwarnings', '--crate-name', 'vos_xhci_init', '--crate-type=rlib', '-Coverflow-checks=yes',
                    '-Cdebug-assertions=yes', MODEL / 'lib.rs', '-o', rlib], 'model-build.txt')
        run(rust + ['-Dwarnings', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '--extern', f'vos_xhci_init={rlib}',
                    SUITE, '-o', out / 'init-tests'], 'tests-build.txt')

        # 2. compile-fail controls ---------------------------------------------------------------
        report['compile_fail_controls'] = {}
        for name, (body, code) in CF.items():
            src = out / f'cf-{name}.rs'
            src.write_text(body)
            log = run(rust + ['--extern', f'vos_xhci_init={rlib}', src, '-o', out / f'cf-{name}'], f'cf-{name}.txt', expected=1)
            if code not in log:
                raise RuntimeError(f'compile-fail control {name}: expected {code}')
            report['compile_fail_controls'][name] = code
        src = out / 'cf-unsafe-rejected.rs'
        shutil.copytree(MODEL, out / 'cf-unsafe-model')
        mp = out / 'cf-unsafe-model/machine.rs'
        mp.write_text(mp.read_text().replace('pub fn phase(&self) -> Phase {\n        self.phase\n    }',
                                             'pub fn phase(&self) -> Phase {\n        let x = 1u8; let _ = unsafe { core::ptr::read(&x) };\n        self.phase\n    }'))
        log = run(rust + ['--crate-name', 'cf_unsafe', '--crate-type=rlib', out / 'cf-unsafe-model/lib.rs', '-o', out / 'cf-unsafe.rlib'],
                  'cf-unsafe-rejected.txt', expected=1)
        if 'unsafe' not in log or 'forbid' not in log:
            raise RuntimeError('forbid(unsafe_code) did not reject unsafe code')
        report['compile_fail_controls']['unsafe-rejected'] = 'forbid(unsafe_code)'

        # 3. Rust tests -----------------------------------------------------------------------------
        text = run([out / 'init-tests', '--test-threads=1'], 'tests.txt')
        report['rust_tests'] = re.search(r'test result: .*', text)[0]
        report['rust_test_names'] = re.findall(r'^test (\S+) \.\.\. ok$', text, re.M)

        # 4. oracle -------------------------------------------------------------------------------------
        trace = out / 'trace.txt'
        run([out / 'init-tests', '--test-threads=1', 'export_traces_for_the_oracle'], 'export.txt',
            env={'XHCI_INIT_TRACE': str(trace), 'XHCI_INIT_SEEDS': str(args.seeds)})
        otext = run([sys.executable, '-I', '-B', ORACLE, trace], 'oracle.txt')
        report['oracle'] = json.loads(otext)
        lines = trace.read_text().split('\n')
        report['oracle_trace_sha256'] = sha(trace)
        controls = {}
        picks = {'refusal-reason': ('=> err wrong-phase', lambda l: l.replace('err wrong-phase', 'err not-running')),
                 'rule-name': ('program.dcbaap', lambda l: l.replace('program.dcbaap', 'program.crcr')),
                 'write-value': ('N-11:pci.command.bme:16:1', lambda l: l.replace('N-11:pci.command.bme:16:1', 'N-11:pci.command.bme:16:0')),
                 'terminal-cause': (' @ handoff-timeout', lambda l: l.replace('@ handoff-timeout', '@ halt-timeout')),
                 'transition-target': ('ok contained>programming', lambda l: l.replace('ok contained>programming', 'ok contained>programmed'))}
        for k, (needle, f) in picks.items():
            i = next(i for i, l in enumerate(lines) if l.startswith('S ') and needle in l)
            bad = lines[:]
            bad[i] = f(bad[i])
            tp = out / f'trace-corrupt-{k}.txt'
            tp.write_text('\n'.join(bad))
            r = subprocess.run([sys.executable, '-I', '-B', ORACLE, tp], capture_output=True, text=True)
            controls[k] = json.loads(r.stdout)['mismatches']
            tp.unlink()
            if controls[k] != 1:
                raise RuntimeError(f'oracle failed to detect corrupted trace ({k})')
        report['oracle_corruption_controls'] = controls

        # 5. mutation controls ----------------------------------------------------------------------------
        base = (MODEL / 'machine.rs').read_text()
        names = [m[0] for m in M]
        if len(set(names)) != len(names):
            raise RuntimeError('duplicate mutant names')
        todo = [m for m in M if not args.only or m[0] in args.only.split(',')]
        for name, _why, old, new, fname in todo:
            src = (MODEL / fname).read_text()
            if src.count(old) != 1:
                raise RuntimeError(f'mutant {name}: anchor occurs {src.count(old)} times in {fname}')
            if old == new:
                raise RuntimeError(f'mutant {name}: no change')
        workers = args.workers_dir.resolve() if args.workers_dir else out / 'mutant-workers'
        if args.workers_dir:
            if not workers.is_relative_to(ROOT / 'build'):
                raise RuntimeError('--workers-dir must be below build/')
            report['workers_dir_reused'] = str(workers.relative_to(ROOT))
        workers.mkdir(exist_ok=True)
        free = list(range(args.jobs))
        import threading
        lock = threading.Lock()

        def one(m):
            name, why, old, new, fname = m
            with lock:
                w = free.pop()
            try:
                d = workers / f'w{w}'
                d.mkdir(exist_ok=True)
                md = d / 'model'
                if md.exists():
                    for f in md.glob('*.rs'):
                        f.write_text((MODEL / f.name).read_text())
                else:
                    shutil.copytree(MODEL, md)
                (md / fname).write_text((MODEL / fname).read_text().replace(old, new))
                res = {'name': name, 'breaks': why, 'killed_by': [], 'failing_tests': []}

                def sh(argv, env=None, timeout=300):
                    return subprocess.run(list(map(str, argv)), cwd=ROOT, capture_output=True, text=True, timeout=timeout,
                                          env=dict(os.environ, **(env or {})))
                b = sh(rust + ['--cap-lints=allow', '--crate-name', 'vos_xhci_init', '--crate-type=rlib', '-Coverflow-checks=yes',
                               '-Cdebug-assertions=yes', md / 'lib.rs', '-o', d / 'lib.rlib'])
                if b.returncode:
                    res['killed_by'] = ['INVALID: mutant does not compile']
                    res['detail'] = b.stderr[-300:]
                    return res
                b = sh(rust + ['--cap-lints=allow', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '--extern',
                               f'vos_xhci_init={d / "lib.rlib"}', SUITE, '-o', d / 'tests'])
                if b.returncode:
                    res['killed_by'] = ['INVALID: tests do not compile against the mutant']
                    res['detail'] = b.stderr[-300:]
                    return res
                try:
                    t = sh([d / 'tests', '--test-threads=1'], timeout=240)
                    if t.returncode:
                        res['killed_by'].append('rust-tests')
                        res['failing_tests'] = re.findall(r'^test (\S+) \.\.\. FAILED$', t.stdout, re.M)[:6]
                except subprocess.TimeoutExpired:
                    res['killed_by'].append('rust-tests (timeout)')
                tr = d / 'trace.txt'
                try:
                    sh([d / 'tests', '--test-threads=1', 'export_traces_for_the_oracle'],
                       env={'XHCI_INIT_TRACE': str(tr), 'XHCI_INIT_SEEDS': str(args.mutant_seeds)}, timeout=240)
                    if tr.exists() and tr.stat().st_size:
                        o = sh([sys.executable, '-I', '-B', ORACLE, tr], timeout=240)
                        if o.returncode:
                            res['killed_by'].append('oracle')
                except subprocess.TimeoutExpired:
                    pass
                return res
            finally:
                with lock:
                    free.append(w)

        results = []
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            for r in ex.map(one, todo):
                results.append(r)
        report['mutants'] = results
        invalid = [r['name'] for r in results if any(k.startswith('INVALID') for k in r['killed_by'])]
        survived = [r['name'] for r in results if not r['killed_by']]
        report['mutation_summary'] = {
            'total': len(results), 'killed': len(results) - len(survived) - len(invalid), 'survived': survived, 'invalid': invalid,
            'killed_by_rust_tests': sum('rust-tests' in ' '.join(r['killed_by']) for r in results),
            'killed_by_oracle': sum('oracle' in r['killed_by'] for r in results),
            'killed_by_oracle_only': [r['name'] for r in results if r['killed_by'] == ['oracle']],
            'killed_by_rust_tests_only': sum(r['killed_by'] == ['rust-tests'] for r in results)}
        if survived or invalid:
            raise RuntimeError(f'mutation controls: survived={survived} invalid={invalid}')
        report['pass'] = True
    except Exception as e:
        report['pass'] = False
        report['failure'] = str(e)
        raise
    finally:
        report['seconds'] = round(time.time() - started, 1)
        (out / 'result.json').write_text(json.dumps(report, indent=1))
        shutil.rmtree(out / 'cf-unsafe-model', ignore_errors=True) if False else None
    print(json.dumps({k: report[k] for k in ('rust_tests', 'oracle', 'oracle_corruption_controls', 'mutation_summary', 'pass', 'seconds') if k in report}, indent=1))


if __name__ == '__main__':
    main()
