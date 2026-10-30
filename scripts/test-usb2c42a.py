#!/usr/bin/env python3
"""USB.2-C4.2a hosted verification of the backend contract (session, typed backend, model-only DMA ledger).

Host-only: no QEMU, no kernel build, no device, PCI or MMIO access. The code lives in kernel/rust/usb/xhci_init/ next
to the C4.1 model and is referenced by no kernel, ISO or Makefile input.

Gates, in order (the first failure stops the run and keeps its logs):
  1. static isolation: no unsafe/extern/asm/std/alloc in the model directory, #![no_std] and #![forbid(unsafe_code)],
     and no tracked kernel, build or ISO input mentions the model;
  2. compile-fail controls: proofs and write tickets cannot be forged, session state cannot be set from outside,
     and unsafe code is rejected;
  3. the Rust test-suite (typed observations, sequence/identity/freshness, read and write faults, untrusted reports,
     ownership conflicts, scratchpad policy, DMA ownership, fail-closed tail, seeded randomized runs with injected
     faults, completeness of every named cause and reason);
  4. the independent Python session oracle replays exported traces (predicting every backend call and every outcome)
     and is shown to detect corrupted traces;
  5. mutation controls: every mutant must be killed by the Rust tests or the oracle (the split is recorded).
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
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODEL = ROOT / 'kernel/rust/usb/xhci_init'
SUITE = ROOT / 'tests/rust/xhci_init_backend.rs'
SUPPORT = ROOT / 'tests/rust/xhci_init_backend_support.rs'
ORACLE = ROOT / 'scripts/xhci_init_oracle_session.py'

M = []


def mut(name, why, old, new, file='session.rs'):
    M.append((name, why, old, new, file))


# ---- stamped, typed reads ----
mut('stamp-session-unchecked', 'a foreign session id is accepted', "        if let Some((_, _, s)) = self.bound\n            && s != session\n        {\n            return Err(Cause::SessionMismatch);\n        }\n", "")
mut('stamp-duplicate-allowed', 'a repeated sequence number is accepted', "            Some(l) if st.seq == l => return Err(Cause::SequenceDuplicate),\n", "")
mut('stamp-reorder-allowed', 'a decreasing sequence number is accepted', "            Some(l) if st.seq < l => return Err(Cause::SequenceReordered),\n", "")
mut('stamp-gap-allowed', 'a skipped sequence number is accepted', "            Some(l) if st.seq > l + 1 => return Err(Cause::SequenceGap),\n", "")
mut('stamp-gap-off-by-one', 'a gap of one is tolerated', "Some(l) if st.seq > l + 1 =>", "Some(l) if st.seq > l + 2 =>")
mut('stamp-clock-unchecked', 'a backward tick is accepted', "        if st.tick < self.last_tick {\n            return Err(Cause::ClockBackwards);\n        }\n", "")
mut('stamp-not-recorded', 'the last sequence number is never remembered', "        self.last_seq = Some(st.seq);\n        self.last_tick = st.tick;\n        Ok(())", "        Ok(())")
mut('fault-stamp-unchecked', 'a failed read need not carry a valid stamp', "            Err(e) => {\n                self.stamp(e.stamp, e.session)?;\n                if e.kind == ReadFault::NotPresent {", "            Err(e) => {\n                if e.kind == ReadFault::NotPresent {")
mut('not-present-closes', 'an absent optional register closes the session', "                if e.kind == ReadFault::NotPresent {\n                    return Ok(None);\n                }\n", "")
mut('wrong-register-unchecked', 'a response about another register is accepted', "                if r.reg != reg || r.index != index {\n                    return Err(Cause::WrongRegister);\n                }\n", "")
mut('wrong-index-unchecked', 'a response about another port is accepted', "if r.reg != reg || r.index != index {", "if r.reg != reg {")
mut('width-unchecked', 'a response with the wrong width is accepted', "                if r.width != reg.width() {\n                    return Err(Cause::WidthMismatch);\n                }\n", "")
mut('all-ones-unchecked', 'an all-ones read is accepted', "                if r.value == r.width.ones() {\n                    return Err(Cause::ReadAllOnes);\n                }\n", "")
mut('fault-torn-as-other', 'a torn read is reported as another fault', "ReadFault::Torn => Cause::ReadTorn,", "ReadFault::Torn => Cause::ReadOther,")
mut('fault-lease-as-budget', 'a lost lease is reported as a budget fault', "ReadFault::LeaseLost => Cause::LeaseLost,", "ReadFault::LeaseLost => Cause::ReadBudget,")
mut('stamped-fault-unchecked', 'a failed identity/topology read need not carry a valid stamp', "            Err(e) => {\n                self.stamp(e.stamp, e.session)?;\n                Err(Self::fault_cause(e.kind))", "            Err(e) => {\n                Err(Self::fault_cause(e.kind))")
mut('rd-need-absent-accepted', 'a required register that is absent reads as zero', "self.rd(reg, index)?.ok_or(Cause::ReadOther)", "Ok(self.rd(reg, index)?.unwrap_or(0))")
mut('width-ones-wrong', 'the all-ones value of a width is wrong', "if self == Self::W64 { u64::MAX } else { (1u64 << self.bits()) - 1 }", "if self == Self::W64 { u64::MAX } else { (1u64 << self.bits()) - 2 }", 'backend.rs')
mut('width-of-64bit-registers', '64-bit registers claim 32-bit width', "Self::Crcr | Self::Dcbaap | Self::Erstba | Self::Erdp => Width::W64,", "Self::Crcr | Self::Dcbaap | Self::Erstba | Self::Erdp => Width::W32,", 'backend.rs')
# ---- dispatch and terminal gating ----
mut('caller-observation-accepted', 'a caller can hand the machine an observation', "                return Err(Refusal::Session(SReason::UseBackendObservation));\n", "")
mut('bound-check-removed', 'requests are served before a controller is bound', "        if self.bound.is_none() {\n            return Err(Refusal::Session(if self.closed.is_some() { SReason::Closed } else { SReason::NotBound }));\n        }\n", "")
mut('sealed-allows-everything', 'a closed session still accepts every request', "        if self.sealed() && !(tail_ok && self.m.phase() != Phase::Idle && !self.tail_dead) {", "        if false {")
mut('tail-dead-ignored', 'a dead tail keeps serving requests', "&& self.m.phase() != Phase::Idle && !self.tail_dead) {", "&& self.m.phase() != Phase::Idle) {")
mut('tail-allows-reset-observation', 'the tail may run a reset observation', "Req::Status | Req::Pci | Req::Scratch(Scratch::Release(_))", "Req::Status | Req::Pci | Req::Reset | Req::Scratch(Scratch::Release(_))")
mut('idle-not-qualified-removed', 'observations are served before qualification', "                if self.m.phase() == Phase::Idle {\n                    return Err(Refusal::Session(SReason::NotQualified));\n                }\n", "")
mut('op-in-idle-classified', 'writes in the idle machine are treated as writes', "let classified = if self.m.phase() == Phase::Idle { None } else { classify(&op, self.m.phase()) };", "let classified = classify(&op, self.m.phase());")
mut('settle-never-latches', 'the backend is never told the machine ended', "        if a.to.terminal() && !self.latched {", "        if false && a.to.terminal() && !self.latched {")
mut('close-skips-forced-fault', 'closing does not drive the machine to its terminal path', "            accepted = self.m.apply(Operation::BackendFault).ok();", "            accepted = None;")
mut('close-wrong-fault-reason', 'closing reports the fault as a controller error', "            accepted = self.m.apply(Operation::BackendFault).ok();", "            accepted = self.m.apply(Operation::ObserveStatus(Status { hch: self.m.halt_observed(), hse: true, hce: false })).ok();")
mut('close-no-latch', 'closing does not latch the backend', "        if !self.latched {\n            self.latched = true;\n            self.b.latch_terminal();\n        }\n        Done { accepted, closed: Some(c) }", "        Done { accepted, closed: Some(c) }")
mut('close-sealed-keeps-going', 'a fault inside the tail does not end the tail', "            self.tail_dead = true;\n            if let Some(l) = self.ledger.as_mut() {\n                l.quarantine_all();\n            }\n            return Done", "            if let Some(l) = self.ledger.as_mut() {\n                l.quarantine_all();\n            }\n            return Done")
# ---- bind, qualify, time ----
mut('bind-topology-unchecked', 'ambiguous topology is accepted', "            if topo.xhci_functions != 1 || !topo.scan_complete {\n                return Err(Cause::AmbiguousTopology);\n            }\n", "")
mut('bind-incomplete-scan-accepted', 'an incomplete scan is accepted', "topo.xhci_functions != 1 || !topo.scan_complete", "topo.xhci_functions != 1")
mut('bind-failure-not-latched', 'a failed bind does not latch the backend', "                self.closed = Some(c);\n                self.latched = true;\n                self.b.latch_terminal();", "                self.closed = Some(c);")
mut('bind-twice', 'a second bind is accepted', "        if self.bound.is_some() {\n            return Err(Refusal::Session(SReason::AlreadyBound));\n        }\n", "")
mut('revalidate-identity-unchecked', 'a changed identity is accepted', "        if id != id0 {\n            return Err(Cause::IdentityChanged);\n        }\n", "")
mut('revalidate-topology-unchecked', 'a changed topology is accepted', "        if topo != topo0 {\n            return Err(Cause::TopologyChanged);\n        }\n", "")
mut('revalidate-skipped', 'identity and topology are not re-read before a write', "        self.revalidate()?;\n        for &(reg, mask, expect) in preconditions(w) {", "        for &(reg, mask, expect) in preconditions(w) {")
mut('qualify-twice', 'a second qualification is attempted', "        if self.m.phase() != Phase::Idle {\n            return Err(Refusal::Machine(Reason::AlreadyQualified));\n        }\n", "")
mut('qualify-reserved-unchecked', 'reserved USBSTS bits at qualification are accepted', "            if usbsts & RSVD_STS != 0 {\n                return Err(Cause::ReservedBitsSet);\n            }\n            let a = s.m.apply(Operation::Qualify(facts));", "            let a = s.m.apply(Operation::Qualify(facts));")
mut('qualify-port-bound', 'more ports than the model bound are read', "if ports <= MAX_PORTS {", "if true {")
mut('session-port-clamp-16', 'the session only tracks the first 16 ports', "s.ports = ports.min(MAX_PORTS);", "s.ports = ports.min(16);")
mut('scratchpad-hi-shift', 'the scratchpad high field is decoded from the wrong bits', "((((hcs2 >> 21) & 0x1f) << 5) | ((hcs2 >> 27) & 0x1f)) as u16", "((((hcs2 >> 22) & 0x1f) << 5) | ((hcs2 >> 27) & 0x1f)) as u16")
mut('scratchpad-lo-shift', 'the scratchpad low field is decoded from the wrong bits', "((((hcs2 >> 21) & 0x1f) << 5) | ((hcs2 >> 27) & 0x1f)) as u16", "((((hcs2 >> 21) & 0x1f) << 5) | ((hcs2 >> 26) & 0x1f)) as u16")
mut('no-snoop-bit', 'Enable No Snoop is read from the wrong bit', "if bit(v, 11) { Tri::Yes } else { Tri::No }", "if bit(v, 10) { Tri::Yes } else { Tri::No }")
mut('dbc-enable-bit', 'the Debug Capability enable bit is wrong', "if bit(v, 31) { Dbc::Enabled } else { Dbc::Disabled }", "if bit(v, 30) { Dbc::Enabled } else { Dbc::Disabled }")
mut('legacy-bios-bit', 'the BIOS-owned semaphore bit is wrong', "Legacy::Present { bios: bit(v, 16), os: bit(v, 24) }", "Legacy::Present { bios: bit(v, 17), os: bit(v, 24) }")
mut('msi-enable-bit', 'the MSI enable bit is wrong', "msi.is_some_and(|v| bit(v, 0)) || msix.is_some_and(|v| bit(v, 15))", "msi.is_some_and(|v| bit(v, 1)) || msix.is_some_and(|v| bit(v, 15))")
mut('running-bit', 'the run/stop bit is wrong', "running: bit(usbcmd, 0),", "running: bit(usbcmd, 1),")
mut('bme-bit-qualify', 'the bus-master bit is wrong at qualification', "bme: bit(cmd, 2),", "bme: bit(cmd, 1),")
mut('pagesize-bit', 'the 4 KiB page-size bit is wrong', "page_4k: pagesize & 1 != 0,", "page_4k: pagesize & 2 != 0,")
mut('connected-bit', 'the connection bit is wrong at qualification', "if bit(s.rd_need(Reg::Portsc, p)?, 0) {\n                        connected |= 1 << p;", "if bit(s.rd_need(Reg::Portsc, p)?, 1) {\n                        connected |= 1 << p;")
mut('tick-clock-unchecked', 'a backward clock is accepted by the tick', "        if now < self.last_now {\n            return Ok(self.close(Cause::ClockBackwards));\n        }\n", "")
mut('tick-clock-not-recorded', 'the tick clock is never remembered', "        self.last_now = now;\n        self.m = trial;", "        self.m = trial;")
# ---- observations ----
mut('status-reserved-unchecked', 'reserved USBSTS bits are accepted', "        if v & RSVD_STS != 0 {\n            return Err(Cause::ReservedBitsSet);\n        }\n        let halted", "        let halted")
mut('halt-transition-unchecked', 'halted-to-running without a write is accepted', "        if self.know_hch == Fact::Known(true) && !halted && self.know_rs == Fact::Known(false) {\n            return Err(Cause::ImpossibleTransition);\n        }\n", "")
mut('halt-transition-too-strict', 'halted-to-running is refused even after R/S was set', "if self.know_hch == Fact::Known(true) && !halted && self.know_rs == Fact::Known(false) {", "if self.know_hch == Fact::Known(true) && !halted {")
mut('cnr-transition-unchecked', 'CNR appearing without a reset is accepted', "        if self.last_cnr == Some(false) && v & CNR != 0 {\n            return Err(Cause::ImpossibleTransition);\n        }\n", "")
mut('cnr-baseline-kept-after-reset', 'the CNR baseline survives a reset', "            // CNR is legitimately set again after HCRST: the previous reading no longer constrains it.\n            self.last_cnr = None;\n", "")
mut('corroboration-removed', 'a halted report is not corroborated', "if corroborate && halted && self.know_rs != Fact::Known(true) {", "if false {")
mut('corroboration-ignores-usbcmd', 'corroboration ignores USBCMD.R/S', "if bit(cmd, 0) || again & HCH == 0 || again & RSVD_STS != 0 {", "if again & HCH == 0 || again & RSVD_STS != 0 {")
mut('corroboration-ignores-second-read', 'corroboration ignores the second USBSTS read', "if bit(cmd, 0) || again & HCH == 0 || again & RSVD_STS != 0 {", "if bit(cmd, 0) || again & RSVD_STS != 0 {")
mut('corroboration-always', 'corroboration also runs when the controller is believed running', "if corroborate && halted && self.know_rs != Fact::Known(true) {", "if corroborate && halted {")
mut('status-not-fresh', 'a status read does not make the status observation valid', "        self.fresh[ST] = true;\n        let a = self.m.apply(Operation::ObserveStatus(", "        let a = self.m.apply(Operation::ObserveStatus(")
mut('pci-change-unchecked', 'a bus-master change behind our back is accepted', "        if let Fact::Known(k) = self.know_bme\n            && k != bme\n        {\n            return Err(Cause::UnexpectedChange);\n        }\n", "")
mut('pci-belief-not-recorded', 'the read bus-master state is not remembered', "        self.know_bme = Fact::Known(bme);\n        Ok(bme)", "        Ok(bme)")
mut('pci-msi-ignored', 'an enabled MSI/MSI-X is not passed to the machine', "Operation::ObservePci { bme, msi: msi || msix }", "Operation::ObservePci { bme, msi: false }")
mut('pci-not-fresh', 'a PCI read does not make the PCI observation valid', "        self.fresh[PCI] = true;\n        let a = self.m.apply(Operation::ObservePci", "        let a = self.m.apply(Operation::ObservePci")
mut('reset-ports-skipped', 'ports are not read after a reset', "        for p in 0..self.ports {\n            // connection, enable, over-current and reset; change bits are ignored\n            quiet &= self.rd_need(Reg::Portsc, p)? & 0b1_1011 == 0;\n        }", "")
mut('reset-port-connection-ignored', 'a connected port is not noticed after reset', "0b1_1011", "0b1_1010")
mut('reset-crcr-quirk-ignored', 'the CRCR read quirk configuration is ignored', "let crcr_ok = if self.cfg.crcr_reads_zero { crcr == 0 } else { true };", "let crcr_ok = true;")
mut('handoff-bios-bit', 'the handoff observation reads the wrong bit', "Operation::ObserveHandoff { bios: bit(v, 16), os: bit(v, 24) }", "Operation::ObserveHandoff { bios: bit(v, 17), os: bit(v, 24) }")
mut('ports-bit', 'the ports observation reads the wrong bit', "        for p in 0..self.ports {\n            if bit(self.rd_need(Reg::Portsc, p)?, 0) {", "        for p in 0..self.ports {\n            if bit(self.rd_need(Reg::Portsc, p)?, 2) {")
mut('event-index-unchecked', 'an event read for another slot is accepted', "        if e.is_some_and(|t| t.index != index) {\n            return Err(Cause::EventMismatch);\n        }\n", "")
# ---- writes ----
mut('stale-check-removed', 'a write is allowed on stale observations', "            if !self.fresh[k] {\n                return Err(Refusal::Session(SReason::StaleObservation));\n            }", "            let _ = k;")
mut('requires-ignored-when-live', 'live sessions skip the requirement table', "} else { requires(w) };", "} else { &[] };")
mut('requires-pci-dropped', 'bus-master writes do not need a fresh PCI read', "        W::ClearBme | W::Reset | W::SetBme | W::Erstba | W::SetRs => &[ST, PCI],", "        W::ClearBme | W::Reset | W::SetBme | W::Erstba | W::SetRs => &[ST],")
mut('requires-doorbell-dropped', 'the doorbell does not need a fresh status', "        W::ClearRs | W::Doorbell => &[ST],", "        W::ClearRs => &[ST],\n        W::Doorbell => &[],")
mut('requires-ack-dropped', 'an acknowledgment does not need a fresh event read', "        W::Ack => &[EVT],\n    }\n}\n\n/// Observations a write invalidates.", "        W::Ack => &[],\n    }\n}\n\n/// Observations a write invalidates.")
mut('requires-static-dropped', 'static registers do not need a fresh status', "        W::Slots | W::Dcbaap | W::Crcr | W::Erstsz | W::ErdpInit => &[ST],\n        W::Ack", "        W::Slots | W::Dcbaap | W::Crcr | W::Erstsz | W::ErdpInit => &[],\n        W::Ack")
mut('requires-handoff-dropped', 'ownership writes do not need a fresh handoff read', "        W::ReqOs | W::Smi => &[HND],\n        W::ClearRs | W::Doorbell", "        W::ReqOs | W::Smi => &[],\n        W::ClearRs | W::Doorbell")
mut('invalidates-reset-ports', 'a reset leaves port and event observations valid', "        W::Reset => &[ST, PRT, EVT],", "        W::Reset => &[ST],")
mut('invalidates-doorbell-event', 'a doorbell leaves the event observation valid', "        W::Doorbell => &[ST, EVT],", "        W::Doorbell => &[ST],")
mut('invalidates-bme-pci', 'a bus-master write leaves the PCI observation valid', "        W::ClearBme | W::SetBme => &[PCI],", "        W::ClearBme | W::SetBme => &[],")
mut('invalidates-erstba-status', 'ERSTBA leaves the status observation valid', "        W::ClearRs | W::SetRs | W::Erstba => &[ST],", "        W::ClearRs | W::SetRs => &[ST],\n        W::Erstba => &[],")
mut('invalidates-ack-event', 'an acknowledgment leaves the event observation valid', "        W::Ack => &[EVT],\n        W::Slots", "        W::Ack => &[],\n        W::Slots")
mut('invalidates-handoff', 'ownership writes leave the handoff observation valid', "        W::ReqOs | W::Smi => &[HND],\n        W::ClearRs | W::SetRs", "        W::ReqOs | W::Smi => &[],\n        W::ClearRs | W::SetRs")
mut('invalidation-not-applied', 'writes never invalidate observations', "        for &k in invalidates(w) {\n            self.fresh[k] = false;\n        }", "")
mut('pre-clearrs-wrong', 'the run/stop precondition expects the wrong value', "        W::ClearRs => &[(Reg::Usbcmd, 1, 1)],", "        W::ClearRs => &[(Reg::Usbcmd, 1, 0)],")
mut('pre-clearbme-halt-dropped', 'the bus-master clear does not recheck halt', "[(Reg::Usbsts, HCH, HCH), (Reg::Usbcmd, 1, 0), (Reg::PciCommand, 4, 4)]", "[(Reg::Usbcmd, 1, 0), (Reg::PciCommand, 4, 4)]")
mut('pre-reset-bme-dropped', 'the reset does not recheck bus master', "        W::Reset => &[(Reg::PciCommand, 4, 0), (Reg::Usbsts, HCH, HCH)],", "        W::Reset => &[(Reg::Usbsts, HCH, HCH)],")
mut('pre-static-cnr-dropped', 'static registers do not recheck CNR', "&[(Reg::Usbsts, CNR | HCH, HCH)],\n        W::SetBme", "&[(Reg::Usbsts, HCH, HCH)],\n        W::SetBme")
mut('pre-setbme-bme-dropped', 'the bus-master set does not recheck that it is clear', "        W::SetBme => &[(Reg::Usbsts, HCH, HCH), (Reg::PciCommand, 4, 0)],", "        W::SetBme => &[(Reg::Usbsts, HCH, HCH)],")
mut('pre-erstba-bme-dropped', 'ERSTBA does not recheck bus master', "        W::Erstba => &[(Reg::PciCommand, 4, 4), (Reg::Usbsts, HCH | HCE, HCH)],", "        W::Erstba => &[(Reg::Usbsts, HCH | HCE, HCH)],")
mut('pre-setrs-error-dropped', 'run does not recheck controller errors', "        W::SetRs => &[(Reg::Usbsts, HCE | HSE, 0), (Reg::PciCommand, 4, 4)],", "        W::SetRs => &[(Reg::PciCommand, 4, 4)],")
mut('pre-doorbell-dropped', 'the doorbell does not recheck status', "        W::Doorbell => &[(Reg::Usbsts, HCH | HCE | HSE, 0)],", "        W::Doorbell => &[],")
mut('pre-smi-bios-ignored', 'the SMI write ignores a BIOS that regained ownership', "(Reg::Usblegsup, (1 << 24) | (1 << 16), 1 << 24)", "(Reg::Usblegsup, 1 << 24, 1 << 24)")
mut('pre-compare-inverted', 'a precondition mismatch is accepted', "            if v & mask != expect {\n                return Err(Cause::PreconditionChanged);", "            if v & mask == expect {\n                return Err(Cause::PreconditionChanged);")
mut('readback-clearbme-wrong', 'the bus-master clear verifies the wrong value', "        W::ClearBme => (Reg::PciCommand, 4, 0),", "        W::ClearBme => (Reg::PciCommand, 4, 4),")
mut('readback-missing-accepted', 'a missing read-back is accepted', "                        None => Err(Cause::WriteUnverified),\n                        Some(rb) if rb.reg", "                        None => Ok(()),\n                        Some(rb) if rb.reg")
mut('readback-register-unchecked', 'a read-back of another register is accepted', "                        Some(rb) if rb.reg != reg || rb.width != reg.width() => Err(Cause::WriteMismatch),\n", "")
mut('readback-value-unchecked', 'a wrong read-back value is accepted', "                        Some(rb) if rb.value & mask != expect => Err(Cause::WriteMismatch),\n", "")
mut('ticket-nonce-reused', 'tickets are not single-use', "        let ticket = WriteTicket::new(self.nonce, ev.id);\n        self.nonce += 1;", "        let ticket = WriteTicket::new(self.nonce, ev.id);")
mut('write-stamp-unchecked', 'the stamp of a write receipt is not validated', "            Ok(r) => {\n                self.stamp(r.stamp, r.session)?;\n                self.last_write_seq", "            Ok(r) => {\n                Ok::<(), Cause>(())?;\n                self.last_write_seq")
mut('write-refused-cause', 'a refused write is reported as a failed write', "WriteFault::Refused => Cause::WriteRefused,", "WriteFault::Refused => Cause::WriteFailed,")
mut('write-last-seq-not-recorded', 'a receipt does not update the last write sequence', "                self.stamp(r.stamp, r.session)?;\n                self.last_write_seq = r.stamp.seq;", "                self.stamp(r.stamp, r.session)?;")
mut('write-fail-last-seq-not-recorded', 'a failed write does not update the last write sequence', "                self.stamp(e.stamp, e.session)?;\n                self.last_write_seq = e.stamp.seq;\n", "                self.stamp(e.stamp, e.session)?;\n")
mut('commit-reducing-writes', 'a reducing write that is uncertain is assumed to have happened', "                if !reduces_exposure(w) {\n                    self.m = trial;\n                }", "                self.m = trial;")
mut('commit-increasing-never', 'an increasing write that is uncertain is assumed not to have happened', "                if !reduces_exposure(w) {", "                if false {")
mut('unknown-facts-ignored', 'uncertain writes still leave facts known', "let known = |v: bool| if verified { Fact::Known(v) } else { Fact::Unknown };", "let known = |v: bool| Fact::Known(v);")
mut('halt-belief-kept-after-clear', 'the halted belief survives a run/stop clear', "                self.know_rs = known(false);\n                self.know_hch = Fact::Unknown;", "                self.know_rs = known(false);")
mut('sealed-preconditions', 'the tail repeats the live write checks', "        if sealed {\n            return Ok(());\n        }\n        self.revalidate()?;", "        self.revalidate()?;")
mut('publish-proof-always', 'a publication proof exists for pages that are not published', "if self.closed.is_none() && self.m.page(kind).state == PageState::Published {", "if true {")
mut('quiescence-proof-always', 'a quiescence proof exists before quiescence', "if self.m.phase() == Phase::Quiesced { Some(", "if true { Some(")
# ---- DMA ledger ----
mut('scratch-budget-off-by-one', 'one scratchpad more than the budget is accepted', "if spec.scratchpads > SCRATCHPAD_BUDGET {", "if spec.scratchpads > SCRATCHPAD_BUDGET + 1 {", 'dma_model.rs')
mut('scratch-invalid-unchecked', 'a count beyond the specification maximum is only a budget refusal', "if spec.scratchpads > SPEC_MAX_SCRATCHPADS {", "if false {", 'dma_model.rs')
mut('scratch-limit-31', 'the old 31-entry array limit is applied to the budget', "if spec.scratchpads > SCRATCHPAD_BUDGET {", "if spec.scratchpads > 31 {", 'dma_model.rs')
mut('limit-floor-removed', 'a device limit below the minimum is accepted', "if spec.device_limit < 2 * DMA_MIN_ADDR || spec.device_limit > 1 << 48 {", "if spec.device_limit > 1 << 48 {", 'dma_model.rs')
mut('alloc-closed-ignored', 'a closed pool still allocates', "        if self.closed {\n            return Err(DReason::PoolClosed);\n        }\n        if self.st[i] != DState::Free {", "        if self.st[i] != DState::Free {", 'dma_model.rs')
mut('alloc-owned-unchecked', 'a page can be allocated twice', "        if self.st[i] != DState::Free {\n            return Err(DReason::AlreadyOwned);\n        }\n", "", 'dma_model.rs')
mut('alloc-misaligned-unchecked', 'an unaligned page is accepted', "        if addr % DMA_PAGE != 0 {\n            return Err(DReason::Misaligned);\n        }\n", "", 'dma_model.rs')
mut('alloc-low-accepted', 'a page below 1 MiB is accepted', "Some(end) if addr >= DMA_MIN_ADDR && end <= limit => {}", "Some(end) if end <= limit => {}", 'dma_model.rs')
mut('alloc-high-accepted', 'a page beyond the limit is accepted', "Some(end) if addr >= DMA_MIN_ADDR && end <= limit => {}", "Some(_) if addr >= DMA_MIN_ADDR => {}", 'dma_model.rs')
mut('alloc-overlap-accepted', 'overlapping pages are accepted', "        if (0..MAX_RECORDS).any(|j| self.st[j] != DState::Free && self.ad[j].abs_diff(addr) < DMA_PAGE) {\n            return Err(DReason::Overlap);\n        }\n", "", 'dma_model.rs')
mut('alloc-ac64-ignored', 'the 32-bit address limit is ignored', "let limit = if self.spec.ac64 { self.spec.device_limit } else { self.spec.device_limit.min(1 << 32) };", "let limit = self.spec.device_limit;", 'dma_model.rs')
mut('state-unchecked', 'a page in the wrong state is accepted', "            s if s == want => Ok(i),\n            _ => Err(DReason::BadState),", "            _ => Ok(i),", 'dma_model.rs')
mut('prepare-erst-dependency-dropped', 'the ERST is prepared before the event ring', "Item::Core(PageKind::Erst) => matches!(self.st[PageKind::Event as usize], DState::Prepared | DState::Published),", "Item::Core(PageKind::Erst) => true,", 'dma_model.rs')
mut('prepare-array-dependency-dropped', 'the scratchpad array is prepared before its buffers', "Item::Array => self.all_buffers(DState::Prepared),", "Item::Array => true,", 'dma_model.rs')
mut('prepare-dcbaa-array-dependency-dropped', 'the DCBAA is prepared before the scratchpad array', "Item::Core(PageKind::Dcbaa) if self.spec.scratchpads > 0 => self.st[ARRAY] == DState::Prepared,", "Item::Core(PageKind::Dcbaa) if self.spec.scratchpads > 0 => true,", 'dma_model.rs')
mut('publish-incomplete-scratchpads', 'the DCBAA is published with incomplete scratchpads', "if self.st[ARRAY] != DState::Prepared || !self.all_buffers(DState::Prepared) {", "if false {", 'dma_model.rs')
mut('publish-buffers-cpu-visible', 'published scratchpad buffers stay CPU-accessible', "                self.st[BUF0 + b] = DState::DeviceOwned;", "                self.st[BUF0 + b] = DState::Published;", 'dma_model.rs')
mut('publish-core-state', 'a core page is published from the wrong state', "        let i = self.expect(Item::Core(kind), DState::Prepared)?;\n        self.st[i] = DState::Published;", "        let i = self.expect(Item::Core(kind), DState::Zeroed)?;\n        self.st[i] = DState::Published;", 'dma_model.rs')
mut('publish-dcbaa-via-publish', 'the DCBAA is published without its scratchpads', "        if kind == PageKind::Dcbaa {\n            return Err(DReason::BadState);\n        }\n", "", 'dma_model.rs')
mut('cpu-touch-device-owned', 'the CPU may touch device-owned pages', "            DState::DeviceOwned => Err(DReason::CpuTouchOfDeviceOwned),", "            DState::DeviceOwned => Ok(()),", 'dma_model.rs')
mut('quiesce-nothing-accepted', 'quiescing with nothing published is accepted', "if any { Ok(()) } else { Err(DReason::NothingToQuiesce) }", "Ok(())", 'dma_model.rs')
mut('release-published', 'a published page can be released', "            DState::Published | DState::DeviceOwned => Err(DReason::StillPublished),", "            DState::Published | DState::DeviceOwned => {\n                self.st[i] = DState::Free;\n                self.ad[i] = 0;\n                Ok(())\n            }", 'dma_model.rs')
mut('release-policy-ignored', 'the never-release policy is ignored', "            DState::Quiesced if self.spec.release == ReleasePolicy::Never => Err(DReason::ReleaseForbiddenByPolicy),\n", "", 'dma_model.rs')
mut('quarantine-cap-off-by-one', 'the quarantine holds one more than its capacity', "_ if self.quarantined < self.spec.quarantine_cap => {", "_ if self.quarantined <= self.spec.quarantine_cap => {", 'dma_model.rs')
mut('quarantine-exhaustion-open', 'an exhausted quarantine does not close the pool', "                self.st[i] = DState::Retained;\n                self.closed = true;", "                self.st[i] = DState::Retained;", 'dma_model.rs')
# ---- Increment 1: the ledger follows the machine; scratchpad plans ----
mut('sync-skipped', 'the ledger is never brought in line with the machine', "        match self.sync() {\n            Ok(()) => out,", "        match Ok::<(), Cause>(()) {\n            Ok(()) => out,")
mut('sync-allocate-not-mirrored', 'a machine allocation is not recorded in the ledger', "(PageState::Free, PageState::Allocated) => l.allocate(item, b.addr),", "(PageState::Free, PageState::Allocated) => Ok(()),")
mut('sync-prepare-not-mirrored', 'a machine preparation is not recorded in the ledger', "(PageState::Zeroed, PageState::Prepared) => l.prepare(item),", "(PageState::Zeroed, PageState::Prepared) => Ok(()),")
mut('sync-dcbaa-publish-plain', 'the DCBAA is published without its scratchpads', "(PageState::Prepared, PageState::Published) if k == PageKind::Dcbaa => l.publish_dcbaa(&PublishProof::new()),", "(PageState::Prepared, PageState::Published) if k == PageKind::Dcbaa => l.publish(PageKind::Erst, &PublishProof::new()).or(Ok(())),")
mut('sync-release-not-mirrored', 'a machine release is not recorded in the ledger', "(_, PageState::Free) => l.release(item),", "(_, PageState::Free) => Ok(()),")
mut('sync-quiesce-skipped', 'the ledger is not quiesced when the machine is', "        if self.m.phase() == Phase::Quiesced && !self.quiesced {", "        if false && self.m.phase() == Phase::Quiesced && !self.quiesced {")
mut('array-ready-always', 'the DCBAA may be prepared before the array is', "        l.state(Item::Array) == Some(DState::Prepared) && l.address(Item::Array) == Some(entry0)", "        true")
mut('array-address-unchecked', 'the DCBAA may point anywhere while an array exists', "l.state(Item::Array) == Some(DState::Prepared) && l.address(Item::Array) == Some(entry0)", "l.state(Item::Array) == Some(DState::Prepared)")
mut('array-state-unchecked', 'the DCBAA may point at an unprepared array', "l.state(Item::Array) == Some(DState::Prepared) && l.address(Item::Array) == Some(entry0)", "l.address(Item::Array) == Some(entry0)")
mut('array-needed-without-scratchpads', 'a controller without scratchpads needs an array', "        if l.spec().scratchpads == 0 {\n            return true;\n        }\n", "")
mut('scratch-core-item-accepted', 'a core page can be driven through the scratchpad request', "        if matches!(item, Item::Core(_)) {\n            return Err(Refusal::Session(SReason::CorePageViaOperation));\n        }\n", "")
mut('scratch-window-ignored', 'scratchpad pages can be allocated after exposure', "        if !matches!(s, Scratch::Release(_)) && !self.m.allocation_open() {", "        if false {")
mut('scratch-window-blocks-release', 'scratchpad pages cannot be released when the window is closed', "        if !matches!(s, Scratch::Release(_)) && !self.m.allocation_open() {", "        if !self.m.allocation_open() {")
mut('scratch-overlap-core-unchecked', 'a core page may land on a scratchpad page', "                        && self.ledger.as_ref().is_some_and(|l| l.scratch_overlaps(addr))", "                        && false")
mut('scratch-overlap-skips-array', 'a core page may land on the scratchpad array', "(ARRAY..MAX_RECORDS).any(|j| self.st[j] != DState::Free && self.ad[j].abs_diff(addr) < DMA_PAGE)", "(BUF0..MAX_RECORDS).any(|j| self.st[j] != DState::Free && self.ad[j].abs_diff(addr) < DMA_PAGE)", 'dma_model.rs')
mut('ledger-for-refused-qualification', 'a refused qualification still plans DMA pages', "            if a.is_ok() && !s.m.phase().terminal() {", "            if a.is_ok() {")
mut('ledger-limit-from-nothing', 'the ledger limit is not the platform DMA limit', "                    device_limit: facts.dma_limit,", "                    device_limit: 1 << 20,")
mut('ledger-policy-forced', 'the ledger ignores the machine release policy', "                    release: s.m.policy(),", "                    release: ReleasePolicy::AfterQuiesce,")
mut('abandon-while-live', 'a live session can abandon its pages', "        if !self.sealed() {\n            return Err(SReason::NotClosed);\n        }\n", "")
mut('tail-dead-keeps-pages', 'a dead tail leaves its pages releasable', "            self.tail_dead = true;\n            if let Some(l) = self.ledger.as_mut() {\n                l.quarantine_all();\n            }\n", "            self.tail_dead = true;\n")
mut('quarantine-cap-too-small', 'the quarantine cannot hold a failed plan', "const QUARANTINE_CAP: u8 = 80;", "const QUARANTINE_CAP: u8 = 8;")
mut('quarantine-all-skips-device-owned', 'device-owned pages are not quarantined', "if !matches!(self.st[i], DState::Free | DState::Quarantined | DState::Retained) {", "if !matches!(self.st[i], DState::Free | DState::Quarantined | DState::Retained | DState::DeviceOwned) {", 'dma_model.rs')
mut('quarantine-all-ignores-exhaustion', 'quarantine_all never closes an exhausted pool', "                let _ = self.quarantine_at(i);\n                n += 1;", "                let _ = self.quarantine_at(i).is_ok();\n                n += 1;\n                self.closed = false;", 'dma_model.rs')
mut('array-without-scratchpads', 'a scratchpad array exists with no scratchpads', "Item::Array if self.spec.scratchpads > 0 => Ok(ARRAY),", "Item::Array => Ok(ARRAY),", 'dma_model.rs')
mut('buffer-index-bound', 'one buffer beyond the plan exists', "Item::Buffer(i) if u16::from(i) < self.spec.scratchpads =>", "Item::Buffer(i) if u16::from(i) <= self.spec.scratchpads =>", 'dma_model.rs')


CF = {
    'publish-proof-tuple-cannot-be-forged': ('use vos_xhci_init::*;\nfn main() { let _ = PublishProof(()); }\n', 'E0423'),
    'publish-proof-constructor-is-private': ('use vos_xhci_init::*;\nfn main() { let _ = PublishProof::new(); }\n', 'E0624'),
    'quiescence-proof-constructor-is-private': ('use vos_xhci_init::*;\nfn main() { let _ = QuiescenceProof::new(); }\n', 'E0624'),
    'write-ticket-cannot-be-forged': ('use vos_xhci_init::*;\nfn main() { let _ = WriteTicket::new(1, "N-01"); }\n', 'E0624'),
    'session-state-is-private': ('use vos_xhci_init::*;\nfn f<B: Backend>(mut s: Session<B>) { s.fresh = [true; 5]; }\nfn main() {}\n', 'E0616'),
    'machine-state-is-still-private': ('use vos_xhci_init::*;\nfn main() { let mut m = Machine::new(ReleasePolicy::Never); m.phase = Phase::Running; }\n', 'E0616'),
}
BANNED = re.compile(r'\bunsafe\b|\bextern\b|\basm!|\bglobal_asm!|\balloc::|\bstd::|\bstatic\s+mut\b')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--seeds', type=int, default=200, help='seeds exported for the main oracle run')
    ap.add_argument('--mutant-seeds', type=int, default=60)
    ap.add_argument('--only', help='comma-separated mutant names (debugging)')
    ap.add_argument('--workers-dir', type=Path, help='reuse these mutant build directories in place (they hold build caches, not evidence)')
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    report = {'hosted_only': True, 'device_access': False, 'pci_mmio_physical_memory_access': False, 'source_sha256': {}, 'commands': []}
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
        # 1. static isolation ---------------------------------------------------------------------------------
        for p in sorted(MODEL.glob('*.rs')):
            text = p.read_text().replace('forbid(unsafe_code)', '')
            hit = BANNED.search(text)
            if hit:
                raise RuntimeError(f'{p.name}: banned construct {hit.group(0)!r}')
        lib = (MODEL / 'lib.rs').read_text()
        if '#![no_std]' not in lib or '#![forbid(unsafe_code)]' not in lib:
            raise RuntimeError('model crate must be #![no_std] and #![forbid(unsafe_code)]')
        tracked = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
        allowed = re.compile(r'^(kernel/rust/usb/xhci_init/|tests/rust/xhci_init_|scripts/(xhci_init_oracle|test-usb2c41|test-usb2c42a|measure-usb2c41|measure-usb2c42a)|docs/|README\.md|\.github/workflows/build-smoke\.yml|\.gitleaksignore)')
        mentions = []
        for f in tracked:
            q = ROOT / f
            if allowed.match(f) or not q.is_file() or q.stat().st_size > 2_000_000:
                continue
            if re.search(r'xhci_init|vos_xhci_init', q.read_text(errors='ignore')):
                mentions.append(f)
        if mentions:
            raise RuntimeError('kernel, build or ISO inputs mention the model: ' + ', '.join(mentions))
        if 'xhci_init' in (ROOT / 'arch/x86_64/rust/kernel_probe.rs').read_text():
            raise RuntimeError('kernel crate root includes the model')
        report['static_isolation'] = {'model_files': [p.name for p in sorted(MODEL.glob('*.rs'))], 'unsafe_free_no_std': True,
                                      'files_outside_allowed_set_mentioning_model': mentions, 'kernel_crate_root_mentions_model': False}

        # build -----------------------------------------------------------------------------------------------
        rlib = out / 'libvos_xhci_init.rlib'
        run(rust + ['-Dwarnings', '--crate-name', 'vos_xhci_init', '--crate-type=rlib', '-Coverflow-checks=yes', '-Cdebug-assertions=yes',
                    MODEL / 'lib.rs', '-o', rlib], 'model-build.txt')
        run(rust + ['-Dwarnings', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '--extern', f'vos_xhci_init={rlib}', SUITE,
                    '-o', out / 'backend-tests'], 'tests-build.txt')

        # 2. compile-fail controls -----------------------------------------------------------------------------
        report['compile_fail_controls'] = {}
        for name, (body, code) in CF.items():
            src = out / f'cf-{name}.rs'
            src.write_text(body)
            log = run(rust + ['--extern', f'vos_xhci_init={rlib}', src, '-o', out / f'cf-{name}'], f'cf-{name}.txt', expected=1)
            if code not in log:
                raise RuntimeError(f'compile-fail control {name}: expected {code}')
            report['compile_fail_controls'][name] = code
        shutil.copytree(MODEL, out / 'cf-unsafe-model')
        mp = out / 'cf-unsafe-model/session.rs'
        mp.write_text(mp.read_text().replace('    pub fn ports(&self) -> u8 {\n        self.ports\n    }',
                                             '    pub fn ports(&self) -> u8 {\n        let x = 1u8; let _ = unsafe { core::ptr::read(&x) };\n        self.ports\n    }'))
        log = run(rust + ['--crate-name', 'cf_unsafe', '--crate-type=rlib', out / 'cf-unsafe-model/lib.rs', '-o', out / 'cf-unsafe.rlib'],
                  'cf-unsafe-rejected.txt', expected=1)
        if 'unsafe' not in log or 'forbid' not in log:
            raise RuntimeError('forbid(unsafe_code) did not reject unsafe code')
        report['compile_fail_controls']['unsafe-rejected'] = 'forbid(unsafe_code)'

        # 3. Rust tests -------------------------------------------------------------------------------------------
        text = run([out / 'backend-tests', '--test-threads=1'], 'tests.txt')
        report['rust_tests'] = re.search(r'test result: .*', text)[0]
        report['rust_test_names'] = re.findall(r'^test (\S+) \.\.\. ok$', text, re.M)

        # 4. oracle ---------------------------------------------------------------------------------------------------
        trace = out / 'trace.txt'
        run([out / 'backend-tests', '--test-threads=1', 'export_session_traces_for_the_oracle'], 'export.txt',
            env={'XHCI_BACKEND_TRACE': str(trace), 'XHCI_BACKEND_SEEDS': str(args.seeds)})
        report['oracle'] = json.loads(run([sys.executable, '-I', '-B', ORACLE, trace], 'oracle.txt'))
        report['oracle_trace_sha256'] = sha(trace)
        lines = trace.read_text().split('\n')
        controls = {}

        def first(pred):
            return next(i for i, l in enumerate(lines) if pred(l))

        def corrupt(k, fn):
            bad = fn(lines[:])
            tp = out / f'trace-corrupt-{k}.txt'
            tp.write_text('\n'.join(bad))
            r = subprocess.run([sys.executable, '-I', '-B', ORACLE, tp], capture_output=True, text=True)
            controls[k] = json.loads(r.stdout)['mismatches']
            tp.unlink()
            if controls[k] < 1:
                raise RuntimeError(f'oracle failed to detect corrupted trace ({k})')

        def swap(prefix, old, new):
            def f(b):
                i = first(lambda l: l.startswith(prefix) and old in l)
                b[i] = b[i].replace(old, new, 1)
                return b
            return f
        corrupt('refusal-reason', swap('=> err s:', 'err s:closed', 'err s:not-bound'))
        corrupt('accepted-rule', swap('=> done ok', 'program.dcbaap', 'program.crcr'))
        corrupt('closure-cause', swap('=> done', 'closed=sequence-duplicate', 'closed=sequence-gap'))
        corrupt('write-event', swap('  W ', 'W N-06', 'W N-07'))

        def drop_call(b):
            i = first(lambda l: l.startswith('  R usbsts'))
            del b[i]
            return b
        corrupt('missing-backend-call', drop_call)

        def extra_call(b):
            i = first(lambda l: l.startswith('  R usbsts'))
            b.insert(i, b[i])
            return b
        corrupt('extra-backend-call', extra_call)
        report['oracle_corruption_controls'] = controls

        # 5. mutation controls --------------------------------------------------------------------------------------------
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
                    for f in MODEL.glob('*.rs'):
                        (md / f.name).write_text(f.read_text())
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
                    t = sh([d / 'tests', '--test-threads=1'], timeout=300)
                    if t.returncode:
                        res['killed_by'].append('rust-tests')
                        res['failing_tests'] = re.findall(r'^test (\S+) \.\.\. FAILED$', t.stdout, re.M)[:6]
                except subprocess.TimeoutExpired:
                    res['killed_by'].append('rust-tests (timeout)')
                tr = d / 'trace.txt'
                try:
                    sh([d / 'tests', '--test-threads=1', 'export_session_traces_for_the_oracle'],
                       env={'XHCI_BACKEND_TRACE': str(tr), 'XHCI_BACKEND_SEEDS': str(args.mutant_seeds)}, timeout=300)
                    if tr.exists() and tr.stat().st_size:
                        o = sh([sys.executable, '-I', '-B', ORACLE, tr], timeout=300)
                        if o.returncode:
                            res['killed_by'].append('oracle')
                except subprocess.TimeoutExpired:
                    pass
                return res
            finally:
                with lock:
                    free.append(w)

        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            results = list(ex.map(one, todo))
        report['mutants'] = results
        invalid = [r['name'] for r in results if any(k.startswith('INVALID') for k in r['killed_by'])]
        survived = [r['name'] for r in results if not r['killed_by']]
        report['mutation_summary'] = {
            'total': len(results), 'killed': len(results) - len(survived) - len(invalid), 'survived': survived, 'invalid': invalid,
            'killed_by_rust_tests': sum(any(k.startswith('rust-tests') for k in r['killed_by']) for r in results),
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
    print(json.dumps({k: report[k] for k in ('rust_tests', 'oracle', 'oracle_corruption_controls', 'mutation_summary', 'pass', 'seconds') if k in report}, indent=1))


if __name__ == '__main__':
    main()
