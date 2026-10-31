//! The EHCI qualification and firmware-handoff state machine (hosted model).
//!
//! `Machine::apply` consumes one typed `Operation` and either accepts it (returning the transition, the single `Rule`
//! that justifies it and, when the step needs a register write, the abstract `Event` a future backend would perform) or
//! refuses it with a named `Reason`, leaving the state untouched. Deadlines and hardware faults are accepted
//! transitions into the terminal phase `Failed`. A controller that fails is never retried by this model.
//!
//! Order (EHCI 1.0 section 2.3, EHCI extended capabilities section 5): qualify, take the OS-owned semaphore and wait for
//! the BIOS to release, disable the legacy SMI enables, switch the periodic and asynchronous schedules off and wait for
//! their status bits, clear Run/Stop and wait for HCHalted, set HCRESET and wait for it to clear. A reset also clears
//! CONFIGFLAG, so the EHCI no longer owns any port afterwards; setting CONFIGFLAG again, scheduling and everything after
//! it are outside this model.
//!
//! Lifecycle invariants (each is checked by the tests):
//! 1. An event is emitted only by an accepted `Request` from exactly its predecessor phase, never by an observation,
//!    a tick or a refusal.
//! 2. Every event is computed from a live observation made after the last event that touches the same register. Static
//!    survey facts alone never produce a write (`StaleObservation`).
//! 3. Only `Observed` facts satisfy a safety requirement. `Unknown`, `Inferred` and `Contradictory` facts block exactly the
//!    steps that depend on them and nothing else.
//! 4. The model never clears the BIOS-owned semaphore or forces ownership. Firmware that does not release, or takes the
//!    semaphore back, ends in `Failed`.
//! 5. The schedules are observed off before Run/Stop is cleared, and the controller is observed halted before reset.
//! 6. Terminal phases accept nothing and emit nothing.
//! 7. The crate has no I/O: an `Event` is a value and nothing in the crate can perform it.

use crate::facts::{EhciFacts, Fact};
use crate::types::*;

/// Abstract time units (one unit is one millisecond of the backend's clock). The OS-owned handoff gets one second.
pub const HANDOFF_TICKS: u64 = 1000;
pub const SMI_TICKS: u64 = 100;
pub const SCHEDULE_TICKS: u64 = 20;
/// EHCI requires a halt within 16 micro-frames of clearing Run/Stop; 20 ms is generous.
pub const HALT_TICKS: u64 = 20;
pub const RESET_TICKS: u64 = 250;

pub const INTEL_VENDOR: u16 = 0x8086;
/// PCI class code of an EHCI: base class 0x0c, sub-class 0x03, programming interface 0x20.
pub const CLASS_EHCI: u32 = 0x000c_0320;
pub const HCI_VERSION: u16 = 0x0100;
pub const MAX_PORTS: u8 = 15;

pub const CMD_RUN: u32 = 1 << 0;
pub const CMD_HCRESET: u32 = 1 << 1;
pub const CMD_PERIODIC_ENABLE: u32 = 1 << 4;
pub const CMD_ASYNC_ENABLE: u32 = 1 << 5;
pub const STS_HOST_ERROR: u32 = 1 << 4;
pub const STS_HALTED: u32 = 1 << 12;
pub const STS_PERIODIC_ACTIVE: u32 = 1 << 14;
pub const STS_ASYNC_ACTIVE: u32 = 1 << 15;
/// USBLEGCTLSTS SMI enables: bits 0-5 and 13-15. Bits 16-21 and 29-31 are write-one-to-clear status.
pub const LEGCTL_ENABLES: u32 = 0x0000_e03f;

const F_SUP: u8 = 1;
const F_CTL: u8 = 2;
const F_CMD: u8 = 4;
const F_STS: u8 = 8;

fn need<T: Copy + PartialEq>(fact: Fact<T>, field: Field) -> Result<T, Reason> {
    match fact {
        Fact::Observed(v) => Ok(v),
        Fact::Unknown => Err(Reason::MissingFact(field)),
        Fact::Inferred(_) => Err(Reason::InferredOnly(field)),
        Fact::Contradictory => Err(Reason::ContradictoryFact(field)),
    }
}

/// The legacy support capability: `None` when the controller has none, else its configuration-space offset.
fn legacy_cap(f: &EhciFacts) -> Result<Option<u8>, Reason> {
    match need(f.legacy, Field::Legacy)? {
        None => Ok(None),
        Some(off) if (0x40..=0xf8).contains(&off) && off % 4 == 0 => Ok(Some(off)),
        Some(_) => Err(Reason::LegacyOffsetInvalid),
    }
}

fn operational_base(f: &EhciFacts) -> Result<u8, Reason> {
    let c = need(f.caplength, Field::Caplength)?;
    if (0x10..=0x40).contains(&c) && c % 4 == 0 {
        Ok(c)
    } else {
        Err(Reason::CaplengthInvalid)
    }
}

/// Whether the facts identify an EHCI the model covers. This says nothing about any particular step: a controller can
/// qualify and still be blocked from a step for lack of one register's value.
pub fn qualify(f: &EhciFacts) -> Result<Qualification, Reason> {
    for (contradictory, field) in [
        (f.vendor == Fact::Contradictory, Field::Vendor),
        (f.device == Fact::Contradictory, Field::Device),
        (f.class_code == Fact::Contradictory, Field::ClassCode),
        (f.hci_version == Fact::Contradictory, Field::HciVersion),
        (f.n_ports == Fact::Contradictory, Field::Ports),
    ] {
        if contradictory {
            return Err(Reason::ContradictoryFact(field));
        }
    }
    if need(f.vendor, Field::Vendor)? != INTEL_VENDOR {
        return Err(Reason::NotIntel);
    }
    need(f.device, Field::Device)?;
    if need(f.class_code, Field::ClassCode)? != CLASS_EHCI {
        return Err(Reason::NotEhciClass);
    }
    if need(f.hci_version, Field::HciVersion)? != HCI_VERSION {
        return Err(Reason::HciVersionUnsupported);
    }
    let ports = need(f.n_ports, Field::Ports)?;
    if ports == 0 || ports > MAX_PORTS {
        return Err(Reason::PortsInvalid);
    }
    // N_PORTS cannot exceed N_CC x N_PCC when there are companions; observed values that say otherwise disagree.
    if let (Fact::Observed(cc), Fact::Observed(pcc)) = (f.n_companions, f.ports_per_companion) {
        if cc > 0 && u16::from(ports) > u16::from(cc) * u16::from(pcc) {
            return Err(Reason::ContradictoryFact(Field::Ports));
        }
    }
    Ok(Qualification { ports })
}

/// Whether `step` could be planned from the facts as currently known. Executing it also needs a fresh live observation.
pub fn readiness(f: &EhciFacts, step: Step) -> Readiness {
    fn go(r: Result<Readiness, Reason>) -> Readiness {
        match r {
            Ok(x) => x,
            Err(e) => Readiness::Blocked(e),
        }
    }
    match step {
        Step::Handoff => go((|| -> Result<Readiness, Reason> {
            match legacy_cap(f)? {
                None => return Ok(Readiness::NotNeeded),
                Some(_) => {}
            }
            need(f.legacy_bios_owned, Field::LegacyBios)?;
            need(f.legacy_os_owned, Field::LegacyOs)?;
            Ok(Readiness::Ready)
        })()),
        Step::MaskSmi => go((|| -> Result<Readiness, Reason> {
            match legacy_cap(f)? {
                None => return Ok(Readiness::NotNeeded),
                Some(_) => {}
            }
            need(f.legacy_ctl, Field::LegacyCtl)?;
            Ok(Readiness::Ready)
        })()),
        Step::StopSchedules | Step::Halt => go((|| -> Result<Readiness, Reason> {
            operational_base(f)?;
            need(f.usbcmd, Field::Usbcmd)?;
            need(f.usbsts, Field::Usbsts)?;
            Ok(Readiness::Ready)
        })()),
        Step::Reset => go((|| -> Result<Readiness, Reason> {
            operational_base(f)?;
            need(f.usbcmd, Field::Usbcmd)?;
            need(f.usbsts, Field::Usbsts)?;
            // A reset clears CONFIGFLAG and so hands every port to the companion controllers; none are modelled.
            if need(f.n_companions, Field::Companions)? != 0 {
                return Err(Reason::CompanionsPresent);
            }
            Ok(Readiness::Ready)
        })()),
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Machine {
    phase: Phase,
    facts: EhciFacts,
    qualification: Option<Qualification>,
    now: u64,
    deadline: Option<(u64, Reason)>,
    fresh: u8,
    stable_since: Option<u64>,
    /// Reset verification: bit 0 HCRESET read back clear, bit 1 HCHalted set, bit 2 CONFIGFLAG read back clear.
    reset_seen: u8,
    writes: u8,
    failure: Option<Reason>,
}

impl Default for Machine {
    fn default() -> Self {
        Self::new()
    }
}

impl Machine {
    pub const fn new() -> Self {
        Self {
            phase: Phase::Idle,
            facts: EhciFacts::UNKNOWN,
            qualification: None,
            now: 0,
            deadline: None,
            fresh: 0,
            stable_since: None,
            reset_seen: 0,
            writes: 0,
            failure: None,
        }
    }

    pub fn phase(&self) -> Phase {
        self.phase
    }
    pub fn facts(&self) -> &EhciFacts {
        &self.facts
    }
    pub fn qualification(&self) -> Option<Qualification> {
        self.qualification
    }
    pub fn now(&self) -> u64 {
        self.now
    }
    /// Number of abstract write events emitted so far.
    pub fn writes(&self) -> u8 {
        self.writes
    }
    /// Why the machine is `Failed` or `Refused`.
    pub fn failure(&self) -> Option<Reason> {
        self.failure
    }
    /// The time at which the current wait fails, if one is running.
    pub fn deadline(&self) -> Option<u64> {
        self.deadline.map(|d| d.0)
    }

    fn fail(&mut self, why: Reason, rule: Rule) -> Accepted {
        let from = self.phase;
        self.phase = Phase::Failed;
        self.deadline = None;
        self.failure = Some(why);
        Accepted { from, to: Phase::Failed, why: rule, event: None, failure: Some(why) }
    }

    fn done(&mut self, to: Phase, why: Rule, event: Option<Event>) -> Accepted {
        let from = self.phase;
        self.phase = to;
        if event.is_some() {
            self.writes = self.writes.saturating_add(1);
        }
        Accepted { from, to, why, event, failure: None }
    }

    fn arm(&mut self, ticks: u64, why: Reason) {
        self.deadline = Some((self.now.saturating_add(ticks), why));
    }

    pub fn apply(&mut self, op: Operation) -> Result<Accepted, Reason> {
        if self.phase.terminal() {
            return Err(Reason::Terminal);
        }
        match op {
            Operation::Qualify(f) => self.qualify_op(f),
            _ if self.phase == Phase::Idle => Err(Reason::WrongPhase),
            Operation::Observe(o) => Ok(self.observe(o)),
            Operation::Advance(n) => self.advance(n),
            Operation::Request(s) => self.request(s),
        }
    }

    fn qualify_op(&mut self, f: EhciFacts) -> Result<Accepted, Reason> {
        if self.phase != Phase::Idle {
            return Err(Reason::WrongPhase);
        }
        match qualify(&f) {
            Ok(q) => {
                self.facts = f;
                self.qualification = Some(q);
                Ok(self.done(Phase::Qualified, Rule::Qualified, None))
            }
            Err(r) => {
                self.phase = Phase::Refused;
                self.failure = Some(r);
                Err(r)
            }
        }
    }

    fn advance(&mut self, n: u64) -> Result<Accepted, Reason> {
        if n == 0 {
            return Err(Reason::BadTicks);
        }
        self.now = self.now.checked_add(n).ok_or(Reason::BadTicks)?;
        if let Some((at, why)) = self.deadline {
            if self.now >= at {
                return Ok(self.fail(why, Rule::FaultTimeout));
            }
        }
        let p = self.phase;
        Ok(self.done(p, Rule::TickAdvance, None))
    }

    fn observe(&mut self, o: Obs) -> Accepted {
        let from = self.phase;
        let recorded = |m: &mut Machine| Accepted { from, to: m.phase, why: Rule::Recorded, event: None, failure: None };
        match o {
            Obs::ReadFailed => self.fail(Reason::ReadFailed, Rule::FaultRead),
            Obs::Caplength(c) => {
                let merged = self.facts.caplength.reconcile(Fact::Observed(c));
                let newly = merged == Fact::Contradictory && self.facts.caplength != Fact::Contradictory;
                self.facts.caplength = merged;
                let mut a = recorded(self);
                if newly {
                    a.why = Rule::Contradicted;
                }
                a
            }
            Obs::LegacySup { bios, os } => {
                self.facts.legacy_bios_owned = Fact::Observed(bios);
                self.facts.legacy_os_owned = Fact::Observed(os);
                self.fresh |= F_SUP;
                if self.phase != Phase::Handoff {
                    return recorded(self);
                }
                if !os {
                    return self.fail(Reason::FirmwareRevoked, Rule::FaultRevoked);
                }
                if bios {
                    self.stable_since = None;
                    return Accepted { from, to: from, why: Rule::HandoffPending, event: None, failure: None };
                }
                match self.stable_since {
                    Some(t) if self.now > t => {
                        self.deadline = None;
                        self.done(Phase::HandedOff, Rule::HandoffStable, None)
                    }
                    Some(_) => Accepted { from, to: from, why: Rule::HandoffComplete, event: None, failure: None },
                    None => {
                        self.stable_since = Some(self.now);
                        Accepted { from, to: from, why: Rule::HandoffComplete, event: None, failure: None }
                    }
                }
            }
            Obs::LegacyCtl(v) => {
                self.facts.legacy_ctl = Fact::Observed(v);
                self.fresh |= F_CTL;
                if self.phase != Phase::MaskingSmi {
                    return recorded(self);
                }
                if v & LEGCTL_ENABLES == 0 {
                    self.deadline = None;
                    self.done(Phase::SmiMasked, Rule::SmiMasked, None)
                } else {
                    Accepted { from, to: from, why: Rule::SmiPending, event: None, failure: None }
                }
            }
            Obs::Usbcmd(v) => {
                self.facts.usbcmd = Fact::Observed(v);
                self.fresh |= F_CMD;
                if self.phase != Phase::Resetting {
                    return recorded(self);
                }
                if v & CMD_HCRESET == 0 {
                    self.reset_seen |= 1;
                } else {
                    self.reset_seen &= !1;
                }
                self.reset_progress(from)
            }
            Obs::Usbsts(v) => {
                if v & STS_HOST_ERROR != 0 {
                    return self.fail(Reason::HostSystemError, Rule::FaultHostError);
                }
                self.facts.usbsts = Fact::Observed(v);
                self.fresh |= F_STS;
                match self.phase {
                    Phase::Stopping => {
                        if v & (STS_PERIODIC_ACTIVE | STS_ASYNC_ACTIVE) == 0 {
                            self.deadline = None;
                            self.done(Phase::Quiet, Rule::SchedulesStopped, None)
                        } else {
                            Accepted { from, to: from, why: Rule::SchedulesPending, event: None, failure: None }
                        }
                    }
                    Phase::Halting => {
                        if v & STS_HALTED != 0 {
                            self.deadline = None;
                            self.done(Phase::Halted, Rule::HaltObserved, None)
                        } else {
                            Accepted { from, to: from, why: Rule::HaltPending, event: None, failure: None }
                        }
                    }
                    Phase::Resetting => {
                        if v & STS_HALTED != 0 {
                            self.reset_seen |= 2;
                        } else {
                            self.reset_seen &= !2;
                        }
                        self.reset_progress(from)
                    }
                    _ => recorded(self),
                }
            }
            Obs::Configflag(b) => {
                self.facts.configflag = Fact::Observed(b);
                if self.phase != Phase::Resetting {
                    return recorded(self);
                }
                if b {
                    self.reset_seen &= !4;
                    if self.reset_seen & 1 != 0 {
                        // HCRESET already read back clear but CONFIGFLAG is still set: the reset did not take effect.
                        return self.fail(Reason::ResetUnverified, Rule::FaultResetUnverified);
                    }
                } else {
                    self.reset_seen |= 4;
                }
                self.reset_progress(from)
            }
        }
    }

    fn reset_progress(&mut self, from: Phase) -> Accepted {
        if self.reset_seen == 7 {
            self.deadline = None;
            self.done(Phase::Reset, Rule::ResetVerified, None)
        } else {
            Accepted { from, to: from, why: Rule::ResetPending, event: None, failure: None }
        }
    }

    fn fresh_or(&self, bits: &[(u8, Field)]) -> Result<(), Reason> {
        for (bit, field) in bits {
            if self.fresh & bit == 0 {
                return Err(Reason::StaleObservation(*field));
            }
        }
        Ok(())
    }

    fn request(&mut self, step: Step) -> Result<Accepted, Reason> {
        let from = match step {
            Step::Handoff => Phase::Qualified,
            Step::MaskSmi => Phase::HandedOff,
            Step::StopSchedules => Phase::SmiMasked,
            Step::Halt => Phase::Quiet,
            Step::Reset => Phase::Halted,
        };
        if self.phase != from {
            return Err(Reason::WrongPhase);
        }
        let ready = readiness(&self.facts, step);
        let cmd = self.facts.usbcmd.observed().unwrap_or(0);
        let sts = self.facts.usbsts.observed().unwrap_or(0);
        match (step, ready) {
            (_, Readiness::Blocked(r)) => Err(r),
            (Step::Handoff, Readiness::NotNeeded) => Ok(self.done(Phase::HandedOff, Rule::HandoffNotNeeded, None)),
            (Step::MaskSmi, Readiness::NotNeeded) => Ok(self.done(Phase::SmiMasked, Rule::SmiNotNeeded, None)),
            (_, Readiness::NotNeeded) => Err(Reason::WrongPhase),
            (Step::Handoff, Readiness::Ready) => {
                self.fresh_or(&[(F_SUP, Field::LegacyBios)])?;
                let cap = legacy_cap(&self.facts)?.ok_or(Reason::WrongPhase)?;
                let bios = self.facts.legacy_bios_owned.observed().unwrap_or(true);
                let os = self.facts.legacy_os_owned.observed().unwrap_or(false);
                if !bios && os {
                    return Ok(self.done(Phase::HandedOff, Rule::HandoffAlreadyOwned, None));
                }
                self.stable_since = None;
                self.arm(HANDOFF_TICKS, Reason::FirmwareNoRelease);
                if os {
                    // The semaphore is already set and the BIOS still holds its own: only wait.
                    Ok(self.done(Phase::Handoff, Rule::HandoffWait, None))
                } else {
                    self.fresh &= !F_SUP;
                    Ok(self.done(Phase::Handoff, Rule::HandoffRequest, Some(Event::LegacyOsSemaphore { cap })))
                }
            }
            (Step::MaskSmi, Readiness::Ready) => {
                self.fresh_or(&[(F_CTL, Field::LegacyCtl)])?;
                let cap = legacy_cap(&self.facts)?.ok_or(Reason::WrongPhase)?;
                let ctl = self.facts.legacy_ctl.observed().unwrap_or(LEGCTL_ENABLES);
                if ctl & LEGCTL_ENABLES == 0 {
                    return Ok(self.done(Phase::SmiMasked, Rule::SmiAlreadyMasked, None));
                }
                self.fresh &= !F_CTL;
                self.arm(SMI_TICKS, Reason::SmiNotDisabled);
                let value = ctl & 0x0000_ffff & !LEGCTL_ENABLES;
                Ok(self.done(Phase::MaskingSmi, Rule::SmiRequest, Some(Event::LegacyCtlWrite { cap, value })))
            }
            (Step::StopSchedules, Readiness::Ready) => {
                self.fresh_or(&[(F_CMD, Field::Usbcmd), (F_STS, Field::Usbsts)])?;
                let enabled = cmd & (CMD_PERIODIC_ENABLE | CMD_ASYNC_ENABLE) != 0;
                let active = sts & (STS_PERIODIC_ACTIVE | STS_ASYNC_ACTIVE) != 0;
                if !enabled && !active {
                    return Ok(self.done(Phase::Quiet, Rule::SchedulesAlreadyOff, None));
                }
                self.arm(SCHEDULE_TICKS, Reason::ScheduleStopTimeout);
                if !enabled {
                    return Ok(self.done(Phase::Stopping, Rule::SchedulesWait, None));
                }
                self.fresh &= !(F_CMD | F_STS);
                let value = cmd & !(CMD_PERIODIC_ENABLE | CMD_ASYNC_ENABLE);
                Ok(self.done(Phase::Stopping, Rule::SchedulesRequest, Some(Event::UsbcmdWrite { value })))
            }
            (Step::Halt, Readiness::Ready) => {
                self.fresh_or(&[(F_CMD, Field::Usbcmd), (F_STS, Field::Usbsts)])?;
                if cmd & (CMD_PERIODIC_ENABLE | CMD_ASYNC_ENABLE) != 0 || sts & (STS_PERIODIC_ACTIVE | STS_ASYNC_ACTIVE) != 0 {
                    return Err(Reason::SchedulesStillActive);
                }
                if cmd & CMD_RUN == 0 && sts & STS_HALTED != 0 {
                    return Ok(self.done(Phase::Halted, Rule::HaltAlreadyHalted, None));
                }
                self.arm(HALT_TICKS, Reason::HaltTimeout);
                if cmd & CMD_RUN == 0 {
                    return Ok(self.done(Phase::Halting, Rule::HaltWait, None));
                }
                self.fresh &= !(F_CMD | F_STS);
                let value = cmd & !CMD_RUN;
                Ok(self.done(Phase::Halting, Rule::HaltRequest, Some(Event::UsbcmdWrite { value })))
            }
            (Step::Reset, Readiness::Ready) => {
                self.fresh_or(&[(F_CMD, Field::Usbcmd), (F_STS, Field::Usbsts)])?;
                if cmd & CMD_RUN != 0 || sts & STS_HALTED == 0 {
                    return Err(Reason::NotHalted);
                }
                self.fresh &= !(F_CMD | F_STS);
                self.reset_seen = 0;
                self.facts.configflag = Fact::Unknown;
                self.arm(RESET_TICKS, Reason::ResetTimeout);
                let value = cmd | CMD_HCRESET;
                Ok(self.done(Phase::Resetting, Rule::ResetRequest, Some(Event::UsbcmdWrite { value })))
            }
        }
    }
}
