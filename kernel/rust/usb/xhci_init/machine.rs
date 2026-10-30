//! The C4.1 initialization protocol state machine (hosted model).
//!
//! `Machine::apply` consumes one typed `Operation` and either accepts it (returning the
//! transition, the single `Rule` that justifies it and, for a register write, the
//! `ModelEvent` that a future backend would perform) or refuses it with a named `Reason`,
//! leaving the state untouched. Faults reported by observations and expired deadlines are
//! accepted transitions into a terminal phase: `Failed` when nothing was ever exposed to the
//! device, `Quarantined` otherwise. Exposure means a published DMA page or a bus master that
//! this protocol enabled. A quarantined controller gets one fixed containment tail
//! (halt, observe halt, clear BME, observe BME) and nothing else.
//!
//! Ordering encoded here (xHCI 1.2 section 4.2, 4.9.3, 4.9.4, 4.21.2, 4.22.1):
//! ownership -> halt -> BME clear -> HCRST -> CNR clear and reset values -> static registers
//! (any order) -> BME set -> ERSTBA -> status check -> R/S -> No-Op commands. ERSTBA follows
//! BME because QEMU 10.2.1 fetches the ERST inside that write (see the hosted device model);
//! the specification does not say when the fetch happens, so the protocol is conservative.

use crate::dma_model::{SCRATCHPAD_BUDGET, SPEC_MAX_SCRATCHPADS};
use crate::types::*;

pub const HANDOFF_TICKS: u64 = 100;
pub const HALT_TICKS: u64 = 10;
pub const STOP_TICKS: u64 = 100;
pub const RESET_TICKS: u64 = 100;
pub const START_TICKS: u64 = 100;
pub const COMMAND_TICKS: u64 = 100;
pub const RING_TRBS: u16 = 256;
pub const POLL_BUDGET: u16 = 32;
pub const UNACKED_LIMIT: u16 = 32;
pub const PAGE: u64 = 4096;
pub const MIN_ADDR: u64 = 0x10_0000;
pub const MAX_PORTS: u8 = 16;

const R_SLOTS: u8 = 1;
const R_DCBAAP: u8 = 2;
const R_CRCR: u8 = 4;
const R_ERSTSZ: u8 = 8;
const R_ERDP: u8 = 16;
const R_ALL: u8 = 31;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Page {
    pub state: PageState,
    pub addr: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Tail {
    Done,
    NeedHalt,
    AwaitHalt,
    NeedBmeClear,
    AwaitVerify,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Machine {
    phase: Phase,
    facts: Option<Facts>,
    policy: ReleasePolicy,
    now: u64,
    deadline: Option<(u64, u64, Reason)>,
    terminal: Option<Reason>,
    tail: Tail,
    legacy: Legacy,
    bme: bool,
    bme_verified: bool,
    armed: bool,
    rs: bool,
    hch: bool,
    written: u8,
    pages: [Page; 4],
    status_clean: bool,
    enq: u16,
    pcs: bool,
    staged: Option<u16>,
    link_required: bool,
    pending: Option<u16>,
    deq: u16,
    ccs: bool,
    unacked: u16,
    step_events: u16,
    last_erdp: u64,
    noops: u32,
    port_events: u32,
    empty_polls: u32,
    devices: u32,
    acks: u32,
    bme_stuck: bool,
}

fn ev(id: &'static str, target: Target, width: u8, value: u64) -> Option<ModelEvent> {
    Some(ModelEvent { id, target, width, value })
}

impl Machine {
    pub const fn new(policy: ReleasePolicy) -> Self {
        Self {
            phase: Phase::Idle,
            facts: None,
            policy,
            now: 0,
            deadline: None,
            terminal: None,
            tail: Tail::Done,
            legacy: Legacy::Absent,
            bme: false,
            bme_verified: false,
            armed: false,
            rs: false,
            hch: false,
            written: 0,
            pages: [Page { state: PageState::Free, addr: 0 }; 4],
            status_clean: false,
            enq: 0,
            pcs: true,
            staged: None,
            link_required: false,
            pending: None,
            deq: 0,
            ccs: true,
            unacked: 0,
            step_events: 0,
            last_erdp: 0,
            noops: 0,
            port_events: 0,
            empty_polls: 0,
            devices: 0,
            acks: 0,
            bme_stuck: false,
        }
    }

    pub fn phase(&self) -> Phase {
        self.phase
    }
    pub fn policy(&self) -> ReleasePolicy {
        self.policy
    }
    /// The cause of the terminal phase (`None` while running and in `Quiesced`).
    pub fn terminal_reason(&self) -> Option<Reason> {
        self.terminal
    }
    pub fn page(&self, kind: PageKind) -> Page {
        self.pages[kind as usize]
    }
    /// Qualification facts, once accepted.
    pub fn facts(&self) -> Option<Facts> {
        self.facts
    }
    /// May host DMA pages be allocated, zeroed and prepared in the current phase?
    pub fn allocation_open(&self) -> bool {
        self.alloc_open().is_ok()
    }
    pub fn exposed(&self) -> bool {
        self.armed || self.pages.iter().any(|p| p.state == PageState::Published)
    }
    pub fn bus_master(&self) -> bool {
        self.bme
    }
    pub fn run_stop(&self) -> bool {
        self.rs
    }
    pub fn noops_completed(&self) -> u32 {
        self.noops
    }
    pub fn port_events(&self) -> u32 {
        self.port_events
    }
    pub fn devices_seen(&self) -> u32 {
        self.devices
    }
    pub fn acks(&self) -> u32 {
        self.acks
    }
    pub fn empty_polls(&self) -> u32 {
        self.empty_polls
    }
    pub fn unacked(&self) -> u16 {
        self.unacked
    }
    pub fn pending_command(&self) -> bool {
        self.pending.is_some()
    }
    pub fn command_cursor(&self) -> (u16, bool) {
        (self.enq, self.pcs)
    }
    pub fn event_cursor(&self) -> (u16, bool) {
        (self.deq, self.ccs)
    }
    /// ERDP value that acknowledges everything consumed so far.
    pub fn expected_erdp(&self) -> u64 {
        self.pages[PageKind::Event as usize].addr + 16 * u64::from(self.deq)
    }
    pub fn statics_written(&self) -> u8 {
        self.written
    }
    pub fn bme_stuck(&self) -> bool {
        self.bme_stuck
    }
    /// Polling mode is the only mode; interrupts are never enabled by this protocol.
    pub fn polling(&self) -> bool {
        true
    }
    /// The last PCI command read-back matched the believed Bus Master Enable state.
    pub fn bus_master_verified(&self) -> bool {
        self.bme_verified
    }
    /// USBSTS was read clean after ERSTBA (the ERST fetch raised no error).
    pub fn status_clean(&self) -> bool {
        self.status_clean
    }
    /// A fresh USBSTS read showed the controller halted and nothing since has invalidated it
    /// (writing R/S makes it unknown again).
    pub fn halt_observed(&self) -> bool {
        self.hch
    }
    /// Events consumed since the last `BeginPollStep`.
    pub fn poll_events(&self) -> u16 {
        self.step_events
    }
    pub fn staged_command(&self) -> Option<u16> {
        self.staged
    }
    pub fn link_required(&self) -> bool {
        self.link_required
    }
    /// Progress of the quarantine containment tail.
    pub fn tail(&self) -> &'static str {
        match self.tail {
            Tail::Done => "done",
            Tail::NeedHalt => "need-halt",
            Tail::AwaitHalt => "await-halt",
            Tail::NeedBmeClear => "need-bme-clear",
            Tail::AwaitVerify => "await-verify",
        }
    }

    // ---- transition helpers ---------------------------------------------------------------

    fn acc(&mut self, to: Phase, why: Rule, event: Option<ModelEvent>) -> Result<Accepted, Reason> {
        let from = self.phase;
        self.phase = to;
        Ok(Accepted { from, to, why, event })
    }
    fn stay(&mut self, why: Rule) -> Result<Accepted, Reason> {
        self.acc(self.phase, why, None)
    }
    fn arm(&mut self, reason: Reason, ticks: u64) {
        self.deadline = Some((self.now, ticks, reason));
    }
    /// Terminal failure: quarantine when anything was exposed, otherwise a clean failure.
    fn fail(&mut self, reason: Reason, why: Rule) -> Result<Accepted, Reason> {
        self.terminal = Some(reason);
        self.deadline = None;
        self.tail = if self.armed && self.bme {
            // R/S written by this protocol outranks any earlier "halted" read: the controller may
            // still be about to start, so it is halted again (and observed) before BME is cleared.
            if self.rs {
                Tail::NeedHalt
            } else if self.hch {
                Tail::NeedBmeClear
            } else {
                Tail::AwaitHalt
            }
        } else {
            Tail::Done
        };
        let to = if self.exposed() { Phase::Quarantined } else { Phase::Failed };
        self.acc(to, why, None)
    }

    pub fn apply(&mut self, op: Operation) -> Result<Accepted, Reason> {
        if self.phase.terminal() {
            return self.after_terminal(op);
        }
        if let Operation::Forbidden(f) = op {
            return Err(match f {
                Forbidden::InterruptEnable => Reason::ForbiddenInterruptEnable,
                Forbidden::PortWrite => Reason::ForbiddenPortWrite,
                Forbidden::SlotDoorbell => Reason::ForbiddenSlotDoorbell,
                Forbidden::Command => Reason::ForbiddenCommand,
            });
        }
        if self.phase == Phase::Idle {
            return match op {
                Operation::Qualify(f) => self.qualify(f),
                Operation::Tick(t) => self.tick(t),
                _ => Err(Reason::NotQualified),
            };
        }
        match op {
            Operation::Qualify(_) => Err(Reason::AlreadyQualified),
            Operation::Tick(t) => self.tick(t),
            Operation::ObserveHandoff { bios, os } => self.observe_handoff(bios, os),
            Operation::ObserveStatus(s) => self.observe_status(s),
            Operation::ObserveReset(r) => self.observe_reset(r),
            Operation::ObservePci { bme, msi } => self.observe_pci(bme, msi),
            Operation::ObservePorts { connected } => self.observe_ports(connected),
            Operation::ConsumeEvent(e) => self.consume(e),
            Operation::DmaAllocate { kind, addr } => self.allocate(kind, addr),
            Operation::DmaZero(kind) => self.zero(kind),
            Operation::PrepareCommandRing => self.prepare(PageKind::Command, |_| Ok(())),
            Operation::PrepareEventRing => self.prepare(PageKind::Event, |_| Ok(())),
            Operation::PrepareDcbaa { entry0 } => self.prepare(PageKind::Dcbaa, move |m| {
                let scratchpads = m.facts.map_or(0, |f| f.scratchpads);
                let limit = m.facts.map_or(0, |f| f.dma_limit);
                if scratchpads == 0 {
                    if entry0 != 0 { Err(Reason::ScratchpadPointerUnsupported) } else { Ok(()) }
                } else if entry0 == 0 {
                    Err(Reason::ScratchpadArrayRequired)
                } else if entry0 % PAGE != 0 {
                    Err(Reason::Misaligned)
                } else if !matches!(entry0.checked_add(PAGE), Some(end) if entry0 >= MIN_ADDR && end <= limit) {
                    Err(Reason::AddressRange)
                } else if m.pages.iter().any(|p| p.state != PageState::Free && p.addr.abs_diff(entry0) < PAGE) {
                    Err(Reason::PageOverlap)
                } else {
                    Ok(())
                }
            }),
            Operation::PrepareErst { segment, size } => self.prepare(PageKind::Erst, move |m| {
                let event = m.pages[PageKind::Event as usize];
                if !matches!(event.state, PageState::Prepared | PageState::Published) {
                    Err(Reason::DependencyNotReady)
                } else if segment != event.addr {
                    Err(Reason::ErstSegmentMismatch)
                } else if size < 16 {
                    Err(Reason::ErstSegmentTooSmall)
                } else if size != RING_TRBS {
                    Err(Reason::ErstSegmentSizeUnsupported)
                } else {
                    Ok(())
                }
            }),
            Operation::DmaRelease(kind) => self.release(kind),
            Operation::RequestOsOwnership => self.request_ownership(),
            Operation::DisableLegacySmi => self.disable_smi(),
            Operation::ClearRunStop => self.clear_run_stop(),
            Operation::ClearBusMaster => self.clear_bus_master(),
            Operation::AssertReset => self.assert_reset(),
            Operation::SetMaxSlots(n) => self.set_slots(n),
            Operation::SetDcbaap(a) => self.set_dcbaap(a),
            Operation::SetCommandRing { addr, rcs } => self.set_crcr(addr, rcs),
            Operation::SetErstSize(n) => self.set_erstsz(n),
            Operation::SetEventDequeue { addr, ehb } => self.set_erdp(addr, ehb),
            Operation::SetBusMaster => self.set_bus_master(),
            Operation::SetErstBase(a) => self.set_erstba(a),
            Operation::SetRunStop => self.set_run_stop(),
            Operation::PublishNoop { index, cycle } => self.publish_noop(index, cycle),
            Operation::PublishLink { index, cycle, toggle, target } => self.publish_link(index, cycle, toggle, target),
            Operation::RingCommandDoorbell => self.doorbell(),
            Operation::BeginPollStep => {
                self.need_running()?;
                self.step_events = 0;
                self.stay(Rule::PollStep)
            }
            Operation::Cancel => self.cancel(),
            Operation::BackendFault => self.fail(Reason::BackendFault, Rule::FaultBackend),
            Operation::Forbidden(_) => Err(Reason::WrongPhase),
        }
    }

    fn after_terminal(&mut self, op: Operation) -> Result<Accepted, Reason> {
        match (self.phase, op) {
            (Phase::Failed | Phase::Quiesced, Operation::DmaRelease(k)) => self.release(k),
            (Phase::Quarantined, op) => self.tail_op(op),
            _ => Err(Reason::AfterTerminal),
        }
    }

    /// The fixed containment tail of a quarantined controller (halt before BME clear, 4.21.2).
    fn tail_op(&mut self, op: Operation) -> Result<Accepted, Reason> {
        match (self.tail, op) {
            (Tail::NeedHalt, Operation::ClearRunStop) => {
                self.rs = false;
                self.hch = false;
                self.tail = Tail::AwaitHalt;
                Ok(Accepted {
                    from: Phase::Quarantined,
                    to: Phase::Quarantined,
                    why: Rule::QuarantineHalt,
                    event: ev("N-16", Target::RunStop, 32, 0),
                })
            }
            (Tail::AwaitHalt, Operation::ObserveStatus(s)) => {
                self.hch = s.hch;
                let why = if s.hch { Rule::QuarantineHalted } else { Rule::HaltPending };
                if s.hch {
                    self.tail = Tail::NeedBmeClear;
                }
                self.stay(why)
            }
            (Tail::NeedBmeClear, Operation::ClearBusMaster) => {
                self.bme = false;
                self.bme_verified = false;
                self.tail = Tail::AwaitVerify;
                Ok(Accepted {
                    from: Phase::Quarantined,
                    to: Phase::Quarantined,
                    why: Rule::QuarantineBmeClear,
                    event: ev("N-17", Target::PciBme, 16, 0),
                })
            }
            (Tail::AwaitVerify, Operation::ObservePci { bme, .. }) => {
                self.tail = Tail::Done;
                if bme {
                    self.bme_stuck = true;
                    self.stay(Rule::QuarantineBmeStuck)
                } else {
                    self.bme_verified = true;
                    self.stay(Rule::QuarantineVerified)
                }
            }
            _ => Err(Reason::AfterTerminal),
        }
    }

    // ---- qualification and time -----------------------------------------------------------

    fn qualify(&mut self, f: Facts) -> Result<Accepted, Reason> {
        let refusal = if f.profile != Profile::Qemu1021 {
            Some(Reason::UnqualifiedPlatform)
        } else if f.version != 0x0100 {
            Some(Reason::UnsupportedVersion)
        } else if !f.page_4k {
            Some(Reason::PageSizeUnsupported)
        } else if f.slots == 0 || f.ports == 0 || f.ports > MAX_PORTS || f.interrupters == 0 {
            Some(Reason::BadCapabilities)
        } else if f.dma_limit < 0x20_0000 || f.dma_limit > 1 << 30 {
            Some(Reason::BadDmaLimit)
        } else if f.hse || f.hce {
            Some(Reason::ControllerFault)
        } else if f.halted == f.running {
            Some(Reason::InconsistentRunState)
        } else if f.iommu == Tri::Yes {
            Some(Reason::IommuPresent)
        } else if f.iommu == Tri::Unknown {
            Some(Reason::IommuUnresolved)
        } else if f.identity_dma == Tri::No {
            Some(Reason::IdentityDmaDenied)
        } else if f.identity_dma == Tri::Unknown {
            Some(Reason::IdentityDmaUnresolved)
        } else if f.no_snoop == Tri::Yes {
            Some(Reason::NoSnoopEnabled)
        } else if f.no_snoop == Tri::Unknown {
            Some(Reason::NoSnoopUnresolved)
        } else if f.msi {
            Some(Reason::MsiEnabled)
        } else if f.dbc == Dbc::Enabled {
            Some(Reason::DbcEnabled)
        } else if f.dbc == Dbc::Unknown {
            Some(Reason::DbcUnresolved)
        } else if f.scratchpads > SPEC_MAX_SCRATCHPADS {
            Some(Reason::ScratchpadsInvalid)
        } else if f.scratchpads > SCRATCHPAD_BUDGET {
            Some(Reason::ScratchpadsExceedBudget)
        } else if matches!(f.legacy, Legacy::Present { bios: true, os: true }) {
            Some(Reason::OwnershipConflict)
        } else if f.connected != 0 {
            Some(Reason::DeviceAttached)
        } else {
            None
        };
        if let Some(reason) = refusal {
            self.terminal = Some(reason);
            return self.acc(Phase::Refused, Rule::FaultRefused, None);
        }
        self.facts = Some(f);
        self.legacy = f.legacy;
        self.bme = f.bme;
        self.bme_verified = true;
        self.rs = f.running;
        self.hch = f.halted;
        match f.legacy {
            Legacy::Absent => self.acc(Phase::Owned, Rule::QualifyNoLegacy, None),
            Legacy::Present { .. } => self.acc(Phase::Qualified, Rule::QualifyLegacy, None),
        }
    }

    fn tick(&mut self, t: u64) -> Result<Accepted, Reason> {
        if t < self.now {
            return self.fail(Reason::ClockBackwards, Rule::FaultClock);
        }
        self.now = t;
        if let Some((start, ticks, reason)) = self.deadline
            && t - start >= ticks
        {
            return self.fail(reason, Rule::FaultTimeout);
        }
        self.stay(Rule::TickAdvance)
    }

    // ---- ownership (xHCI 1.2 sections 4.22.1 and 7.1) -------------------------------------

    fn request_ownership(&mut self) -> Result<Accepted, Reason> {
        let Legacy::Present { os, .. } = self.legacy else {
            return Err(Reason::NoLegacyCapability);
        };
        if os {
            return Err(Reason::AlreadyOsOwned);
        }
        if self.phase != Phase::Qualified {
            return Err(Reason::WrongPhase);
        }
        // The semaphore write is part of the request: from here the OS-owned bit is ours.
        if let Legacy::Present { bios, .. } = self.legacy {
            self.legacy = Legacy::Present { bios, os: true };
        }
        self.arm(Reason::HandoffTimeout, HANDOFF_TICKS);
        self.acc(Phase::Handoff, Rule::HandoffRequest, ev("N-04", Target::LegacyOsOwned, 8, 1))
    }

    fn disable_smi(&mut self) -> Result<Accepted, Reason> {
        let Legacy::Present { bios, os } = self.legacy else {
            return Err(Reason::NoLegacyCapability);
        };
        let ok = match self.phase {
            Phase::HandedOff => true,
            Phase::Qualified => {
                if !(os && !bios) {
                    return Err(Reason::SmiBeforeOwnership);
                }
                true
            }
            Phase::Handoff => return Err(Reason::SmiBeforeOwnership),
            _ => false,
        };
        if !ok {
            return Err(Reason::WrongPhase);
        }
        self.acc(Phase::Owned, Rule::SmiDisabled, ev("N-05", Target::LegacySmi, 32, 0))
    }

    fn observe_handoff(&mut self, bios: bool, os: bool) -> Result<Accepted, Reason> {
        if self.legacy == Legacy::Absent {
            return Err(Reason::NoLegacyCapability);
        }
        match self.phase {
            Phase::Handoff => {
                if !os {
                    self.fail(Reason::OwnershipLost, Rule::FaultOwnership)
                } else if bios {
                    self.stay(Rule::HandoffPending)
                } else {
                    self.legacy = Legacy::Present { bios: false, os: true };
                    self.deadline = None;
                    self.acc(Phase::HandedOff, Rule::HandoffComplete, None)
                }
            }
            Phase::Qualified => {
                if bios && os {
                    self.fail(Reason::OwnershipConflict, Rule::FaultOwnership)
                } else {
                    self.legacy = Legacy::Present { bios, os };
                    self.stay(Rule::HandoffStable)
                }
            }
            _ => {
                if bios {
                    self.fail(Reason::OwnershipConflict, Rule::FaultOwnership)
                } else if !os {
                    self.fail(Reason::OwnershipLost, Rule::FaultOwnership)
                } else {
                    self.stay(Rule::HandoffStable)
                }
            }
        }
    }

    // ---- observations ---------------------------------------------------------------------

    fn observe_status(&mut self, s: Status) -> Result<Accepted, Reason> {
        let fault = s.hse || s.hce;
        if !fault {
            // Refusals are decided before any state is touched.
            match self.phase {
                Phase::Qualified | Phase::Handoff | Phase::HandedOff | Phase::Resetting => {
                    return Err(Reason::ObservationNotExpected);
                }
                Phase::Owned if self.rs => return Err(Reason::HaltNotRequested),
                _ => {}
            }
        }
        self.hch = s.hch;
        if fault {
            return self.fail(Reason::ControllerFault, Rule::FaultStatus);
        }
        match self.phase {
            Phase::Owned => {
                if s.hch {
                    self.acc(Phase::Halted, Rule::HaltIdleObserved, None)
                } else {
                    self.fail(Reason::InconsistentRunState, Rule::FaultInconsistent)
                }
            }
            Phase::Halting => {
                if s.hch {
                    self.deadline = None;
                    self.acc(Phase::Halted, Rule::HaltObserved, None)
                } else {
                    self.stay(Rule::HaltPending)
                }
            }
            Phase::Halted | Phase::Contained | Phase::Programming | Phase::Programmed | Phase::BusMastering => {
                if s.hch {
                    self.stay(Rule::StatusStable)
                } else {
                    self.fail(Reason::InconsistentRunState, Rule::FaultInconsistent)
                }
            }
            Phase::EventRingArmed => {
                if s.hch {
                    self.status_clean = true;
                    self.stay(Rule::ArmedStatusClean)
                } else {
                    self.fail(Reason::InconsistentRunState, Rule::FaultInconsistent)
                }
            }
            Phase::Starting => {
                if s.hch {
                    self.stay(Rule::RunPending)
                } else {
                    self.deadline = None;
                    self.acc(Phase::Running, Rule::RunStarted, None)
                }
            }
            Phase::Running => {
                if s.hch {
                    self.fail(Reason::UnexpectedHalt, Rule::FaultUnexpectedHalt)
                } else {
                    self.stay(Rule::StatusStable)
                }
            }
            _ => Err(Reason::WrongPhase),
        }
    }

    fn observe_reset(&mut self, r: ResetState) -> Result<Accepted, Reason> {
        if self.phase != Phase::Resetting {
            return Err(Reason::ObservationNotExpected);
        }
        if r.cnr {
            return self.stay(Rule::ResetPending);
        }
        if !(r.hch && r.usbcmd_zero && r.pointers_zero && !r.bme && r.ports_quiet) {
            return self.fail(Reason::ResetUnverified, Rule::FaultResetUnverified);
        }
        self.deadline = None;
        self.written = 0;
        self.rs = false;
        self.hch = true;
        self.bme = false;
        self.bme_verified = true;
        if self.exposed() {
            self.terminal = None;
            self.acc(Phase::Quiesced, Rule::ResetQuiesced, None)
        } else {
            self.acc(Phase::Contained, Rule::ResetVerified, None)
        }
    }

    fn observe_pci(&mut self, bme: bool, msi: bool) -> Result<Accepted, Reason> {
        if msi {
            return self.fail(Reason::MsiEnabled, Rule::FaultMsi);
        }
        if bme != self.bme {
            return self.fail(Reason::BusMasterMismatch, Rule::FaultBmeMismatch);
        }
        self.bme_verified = true;
        self.stay(Rule::PciStable)
    }

    fn observe_ports(&mut self, connected: u32) -> Result<Accepted, Reason> {
        let ports = self.facts.map_or(0, |f| f.ports);
        let valid = if ports >= 32 { u32::MAX } else { (1u32 << ports) - 1 };
        if connected & !valid != 0 {
            return Err(Reason::PortMaskInvalid);
        }
        if connected == 0 {
            return self.stay(Rule::PortsQuiet);
        }
        if self.phase == Phase::Running {
            self.devices |= connected;
            self.stay(Rule::PortsDeviceNoted)
        } else {
            self.fail(Reason::DeviceAttached, Rule::FaultDevice)
        }
    }

    // ---- halt, reset, bus master ----------------------------------------------------------

    fn clear_run_stop(&mut self) -> Result<Accepted, Reason> {
        match self.phase {
            Phase::Owned => {
                if !self.rs {
                    return Err(Reason::AlreadyHalted);
                }
                self.rs = false;
                self.hch = false; // halted is unknown again until USBSTS is read
                self.arm(Reason::HaltTimeout, HALT_TICKS);
                self.acc(Phase::Halting, Rule::HaltRequest, ev("N-01", Target::RunStop, 32, 0))
            }
            Phase::Starting | Phase::Running => {
                self.rs = false;
                self.hch = false;
                self.staged = None;
                self.pending = None;
                self.link_required = false;
                self.arm(Reason::HaltTimeout, STOP_TICKS);
                self.acc(Phase::Halting, Rule::HaltRequest, ev("N-16", Target::RunStop, 32, 0))
            }
            Phase::Qualified | Phase::Handoff | Phase::HandedOff => Err(Reason::OwnershipUnresolved),
            _ => Err(Reason::AlreadyHalted),
        }
    }

    fn clear_bus_master(&mut self) -> Result<Accepted, Reason> {
        match self.phase {
            Phase::Halted => {
                if !self.bme {
                    return Err(Reason::BusMasterAlreadyClear);
                }
                let id = if self.exposed() { "N-17" } else { "N-02" };
                self.bme = false;
                self.bme_verified = false;
                self.acc(Phase::Halted, Rule::BmeClear, ev(id, Target::PciBme, 16, 0))
            }
            Phase::Contained | Phase::Programming | Phase::Programmed | Phase::BusMastering | Phase::EventRingArmed => {
                Err(Reason::UseCancel)
            }
            Phase::Resetting => Err(Reason::WrongPhase),
            _ => Err(Reason::BmeClearBeforeHalt),
        }
    }

    fn assert_reset(&mut self) -> Result<Accepted, Reason> {
        match self.phase {
            Phase::Halted => {
                if self.bme {
                    return Err(Reason::ResetWithBusMaster);
                }
                if !self.bme_verified {
                    return Err(Reason::BusMasterUnverified);
                }
                let id = if self.exposed() { "N-18" } else { "N-03" };
                self.arm(Reason::ResetTimeout, RESET_TICKS);
                self.acc(Phase::Resetting, Rule::ResetAssert, ev(id, Target::Hcrst, 32, 2))
            }
            Phase::Contained | Phase::Programming | Phase::Programmed | Phase::BusMastering | Phase::EventRingArmed => {
                Err(Reason::UseCancel)
            }
            Phase::Resetting => Err(Reason::WrongPhase),
            _ => Err(Reason::ResetBeforeHalt),
        }
    }

    fn cancel(&mut self) -> Result<Accepted, Reason> {
        match self.phase {
            Phase::Contained | Phase::Programming | Phase::Programmed | Phase::BusMastering | Phase::EventRingArmed => {
                self.deadline = None;
                self.acc(Phase::Halted, Rule::CancelHalt, None)
            }
            Phase::Starting | Phase::Running => Err(Reason::UseClearRunStop),
            Phase::Halting | Phase::Halted | Phase::Resetting => Err(Reason::WrongPhase),
            _ => self.fail(Reason::Cancelled, Rule::FaultCancelled),
        }
    }

    // ---- static registers (xHCI 1.2 section 4.2) ------------------------------------------

    fn program_phase(&self) -> Result<(), Reason> {
        match self.phase {
            Phase::Contained | Phase::Programming => Ok(()),
            Phase::Qualified | Phase::Handoff | Phase::HandedOff | Phase::Owned => Err(Reason::ProgramBeforeContainment),
            Phase::Halting | Phase::Halted | Phase::Resetting => Err(if self.exposed() {
                Reason::ProgrammingClosed
            } else {
                Reason::ProgramBeforeContainment
            }),
            _ => Err(Reason::ProgrammingClosed),
        }
    }

    fn registered(&mut self, bit: u8, why: Rule, event: Option<ModelEvent>) -> Result<Accepted, Reason> {
        self.written |= bit;
        let to = if self.written == R_ALL { Phase::Programmed } else { Phase::Programming };
        self.acc(to, why, event)
    }

    fn set_slots(&mut self, n: u8) -> Result<Accepted, Reason> {
        self.program_phase()?;
        if self.written & R_SLOTS != 0 {
            return Err(Reason::RegisterRepeated);
        }
        if n != 1 {
            return Err(Reason::SlotsInvalid);
        }
        self.registered(R_SLOTS, Rule::ProgramSlots, ev("N-06", Target::Config, 32, 1))
    }

    fn set_dcbaap(&mut self, addr: u64) -> Result<Accepted, Reason> {
        self.program_phase()?;
        if self.written & R_DCBAAP != 0 {
            return Err(Reason::RegisterRepeated);
        }
        let p = self.pages[PageKind::Dcbaa as usize];
        if p.state != PageState::Prepared {
            return Err(Reason::PageNotPrepared);
        }
        if addr != p.addr {
            return Err(Reason::PointerMismatch);
        }
        self.pages[PageKind::Dcbaa as usize].state = PageState::Published;
        self.registered(R_DCBAAP, Rule::ProgramDcbaap, ev("N-07", Target::Dcbaap, 64, addr))
    }

    fn set_crcr(&mut self, addr: u64, rcs: bool) -> Result<Accepted, Reason> {
        if matches!(self.phase, Phase::Starting | Phase::Running) {
            return Err(Reason::CrcrWhileRunning);
        }
        self.program_phase()?;
        if self.written & R_CRCR != 0 {
            return Err(Reason::RegisterRepeated);
        }
        let p = self.pages[PageKind::Command as usize];
        if p.state != PageState::Prepared {
            return Err(Reason::PageNotPrepared);
        }
        if !rcs {
            return Err(Reason::CycleStateInvalid);
        }
        if addr != p.addr {
            return Err(Reason::PointerMismatch);
        }
        self.pages[PageKind::Command as usize].state = PageState::Published;
        self.registered(R_CRCR, Rule::ProgramCrcr, ev("N-08", Target::Crcr, 64, addr | 1))
    }

    fn set_erstsz(&mut self, n: u16) -> Result<Accepted, Reason> {
        self.program_phase()?;
        if self.written & R_ERSTSZ != 0 {
            return Err(Reason::RegisterRepeated);
        }
        if self.pages[PageKind::Erst as usize].state != PageState::Prepared {
            return Err(Reason::PageNotPrepared);
        }
        if n != 1 {
            return Err(Reason::ErstSizeInvalid);
        }
        self.registered(R_ERSTSZ, Rule::ProgramErstsz, ev("N-09", Target::Erstsz, 32, 1))
    }

    fn set_erdp(&mut self, addr: u64, ehb: bool) -> Result<Accepted, Reason> {
        match self.phase {
            Phase::Running => return self.ack(addr, ehb),
            Phase::Starting => return Err(Reason::NotRunning),
            _ => {}
        }
        self.program_phase()?;
        if self.written & R_ERDP != 0 {
            return Err(Reason::RegisterRepeated);
        }
        let p = self.pages[PageKind::Event as usize];
        if p.state != PageState::Prepared {
            return Err(Reason::PageNotPrepared);
        }
        if ehb {
            return Err(Reason::EhbNotExpected);
        }
        if addr != p.addr {
            return Err(Reason::PointerMismatch);
        }
        self.pages[PageKind::Event as usize].state = PageState::Published;
        self.last_erdp = addr;
        self.registered(R_ERDP, Rule::ProgramErdp, ev("N-10", Target::Erdp, 64, addr))
    }

    // ---- bus master, event ring, run ------------------------------------------------------

    fn set_bus_master(&mut self) -> Result<Accepted, Reason> {
        match self.phase {
            Phase::Programmed => {
                if self.bme {
                    return Err(Reason::BusMasterAlreadySet);
                }
                self.bme = true;
                self.armed = true;
                self.bme_verified = false;
                self.acc(Phase::BusMastering, Rule::BmeSet, ev("N-11", Target::PciBme, 16, 1))
            }
            Phase::BusMastering | Phase::EventRingArmed | Phase::Starting | Phase::Running => {
                Err(Reason::BusMasterAlreadySet)
            }
            Phase::Halting | Phase::Halted | Phase::Resetting => Err(Reason::WrongPhase),
            _ => Err(Reason::BusMasterBeforeProgramming),
        }
    }

    fn set_erstba(&mut self, addr: u64) -> Result<Accepted, Reason> {
        match self.phase {
            Phase::BusMastering => {
                if !self.bme_verified {
                    return Err(Reason::BusMasterUnverified);
                }
                let p = self.pages[PageKind::Erst as usize];
                if p.state != PageState::Prepared {
                    return Err(Reason::PageNotPrepared);
                }
                if addr != p.addr {
                    return Err(Reason::PointerMismatch);
                }
                self.pages[PageKind::Erst as usize].state = PageState::Published;
                self.status_clean = false;
                self.acc(Phase::EventRingArmed, Rule::ErstbaArmed, ev("N-12", Target::Erstba, 64, addr))
            }
            Phase::Contained | Phase::Programming => Err(Reason::ErstbaBeforeStatic),
            Phase::Programmed => Err(Reason::ErstbaWithoutBusMaster),
            Phase::EventRingArmed | Phase::Starting | Phase::Running => Err(Reason::RegisterRepeated),
            Phase::Qualified | Phase::Handoff | Phase::HandedOff | Phase::Owned => Err(Reason::ProgramBeforeContainment),
            _ => Err(Reason::WrongPhase),
        }
    }

    fn set_run_stop(&mut self) -> Result<Accepted, Reason> {
        match self.phase {
            Phase::EventRingArmed => {
                if !self.status_clean {
                    return Err(Reason::StatusNotChecked);
                }
                self.rs = true;
                self.hch = false; // the last "halted" observation no longer holds once R/S is set
                self.arm(Reason::StartTimeout, START_TICKS);
                self.acc(Phase::Starting, Rule::RunStart, ev("N-13", Target::RunStop, 32, 1))
            }
            Phase::Contained | Phase::Programming | Phase::Programmed | Phase::BusMastering => {
                Err(Reason::RunBeforeEventRing)
            }
            Phase::Starting | Phase::Running => Err(Reason::AlreadyRunning),
            Phase::Qualified | Phase::Handoff | Phase::HandedOff | Phase::Owned => Err(Reason::ProgramBeforeContainment),
            _ => Err(Reason::WrongPhase),
        }
    }

    fn need_running(&self) -> Result<(), Reason> {
        if self.phase == Phase::Running { Ok(()) } else { Err(Reason::NotRunning) }
    }

    fn ack(&mut self, addr: u64, ehb: bool) -> Result<Accepted, Reason> {
        if addr != self.expected_erdp() {
            return Err(Reason::ErdpMismatch);
        }
        if !ehb {
            return Err(Reason::EhbNotSet);
        }
        if addr == self.last_erdp {
            return Err(Reason::ErdpRepeat);
        }
        self.last_erdp = addr;
        self.unacked = 0;
        self.acks += 1;
        // EHB set: the write clears Event Handler Busy (xHCI 1.2 Table 5-42).
        self.acc(Phase::Running, Rule::AckErdp, ev("N-15", Target::Erdp, 64, addr | 8))
    }

    fn publish_noop(&mut self, index: u16, cycle: bool) -> Result<Accepted, Reason> {
        self.need_running()?;
        if self.pending.is_some() || self.staged.is_some() {
            return Err(Reason::CommandOutstanding);
        }
        if index != self.enq || index > RING_TRBS - 2 {
            return Err(Reason::CommandIndexMismatch);
        }
        if cycle != self.pcs {
            return Err(Reason::CommandCycleMismatch);
        }
        self.staged = Some(index);
        if index == RING_TRBS - 2 {
            self.link_required = true;
            self.enq = RING_TRBS - 1;
        } else {
            self.enq += 1;
        }
        self.stay(Rule::CmdNoopStaged)
    }

    fn publish_link(&mut self, index: u16, cycle: bool, toggle: bool, target: u64) -> Result<Accepted, Reason> {
        self.need_running()?;
        if !self.link_required {
            return Err(Reason::LinkNotExpected);
        }
        if index != self.enq {
            return Err(Reason::CommandIndexMismatch);
        }
        if cycle != self.pcs {
            return Err(Reason::CommandCycleMismatch);
        }
        if !toggle {
            return Err(Reason::LinkToggleMissing);
        }
        if target != self.pages[PageKind::Command as usize].addr {
            return Err(Reason::LinkTargetMismatch);
        }
        self.link_required = false;
        self.enq = 0;
        self.pcs = !self.pcs;
        self.stay(Rule::CmdLinkPublished)
    }

    fn doorbell(&mut self) -> Result<Accepted, Reason> {
        self.need_running()?;
        if self.pending.is_some() {
            return Err(Reason::CommandOutstanding);
        }
        let Some(index) = self.staged else {
            return Err(Reason::NothingStaged);
        };
        if self.link_required {
            return Err(Reason::LinkMissing);
        }
        if self.unacked > 0 {
            return Err(Reason::AckMissing);
        }
        self.staged = None;
        self.pending = Some(index);
        self.arm(Reason::CommandTimeout, COMMAND_TICKS);
        self.acc(Phase::Running, Rule::DoorbellNoop, ev("N-14", Target::Doorbell0, 32, 0))
    }

    fn consume(&mut self, e: Option<EventTrb>) -> Result<Accepted, Reason> {
        self.need_running()?;
        let Some(t) = e else {
            self.empty_polls += 1;
            return self.stay(Rule::EventEmpty);
        };
        if self.step_events >= POLL_BUDGET {
            return Err(Reason::PollBudgetExceeded);
        }
        if self.unacked >= UNACKED_LIMIT {
            return Err(Reason::AckMissing);
        }
        if t.index != self.deq {
            return Err(Reason::EventIndexMismatch);
        }
        if t.cycle != self.ccs {
            return Err(Reason::EventCycleMismatch);
        }
        let ports = self.facts.map_or(0, |f| f.ports);
        match t.kind {
            33 => {
                let Some(index) = self.pending else {
                    return self.fail(Reason::UnexpectedCompletion, Rule::FaultEvent);
                };
                if t.pointer != self.pages[PageKind::Command as usize].addr + 16 * u64::from(index) {
                    return self.fail(Reason::CompletionPointerMismatch, Rule::FaultEvent);
                }
                if t.code != 1 {
                    return self.fail(Reason::CommandFailed, Rule::FaultEvent);
                }
                if t.slot != 0 || t.vf != 0 || t.param != 0 {
                    return self.fail(Reason::CompletionFieldInvalid, Rule::FaultEvent);
                }
                self.pending = None;
                self.deadline = None;
                self.noops += 1;
                self.advance_event();
                self.stay(Rule::EventCompletion)
            }
            34 => {
                if t.port == 0 || t.port > ports {
                    return self.fail(Reason::PortOutOfRange, Rule::FaultEvent);
                }
                self.port_events += 1;
                self.advance_event();
                self.stay(Rule::EventPortChange)
            }
            _ => self.fail(Reason::UnexpectedEventType, Rule::FaultEvent),
        }
    }

    fn advance_event(&mut self) {
        if self.deq == RING_TRBS - 1 {
            self.deq = 0;
            self.ccs = !self.ccs;
        } else {
            self.deq += 1;
        }
        self.unacked += 1;
        self.step_events += 1;
    }

    // ---- DMA page lifecycle ---------------------------------------------------------------

    fn alloc_open(&self) -> Result<(), Reason> {
        match self.phase {
            Phase::Qualified | Phase::Handoff | Phase::HandedOff | Phase::Owned | Phase::Contained | Phase::Programming
                if !self.exposed() =>
            {
                Ok(())
            }
            Phase::Halting | Phase::Halted | Phase::Resetting if !self.exposed() => Ok(()),
            _ => Err(Reason::AllocationClosed),
        }
    }

    fn allocate(&mut self, kind: PageKind, addr: u64) -> Result<Accepted, Reason> {
        self.alloc_open()?;
        if self.pages[kind as usize].state != PageState::Free {
            return Err(Reason::PageAlreadyOwned);
        }
        if addr % PAGE != 0 {
            return Err(Reason::Misaligned);
        }
        let limit = self.facts.map_or(0, |f| f.dma_limit);
        match addr.checked_add(PAGE) {
            Some(end) if addr >= MIN_ADDR && end <= limit => {}
            _ => return Err(Reason::AddressRange),
        }
        if self.pages.iter().any(|p| p.state != PageState::Free && p.addr.abs_diff(addr) < PAGE) {
            return Err(Reason::PageOverlap);
        }
        self.pages[kind as usize] = Page { state: PageState::Allocated, addr };
        self.stay(Rule::DmaAllocate)
    }

    fn owned_state(&self, kind: PageKind, want: PageState) -> Result<(), Reason> {
        let s = self.pages[kind as usize].state;
        if s == PageState::Free {
            Err(Reason::PageNotOwned)
        } else if s != want {
            Err(Reason::PageState)
        } else {
            Ok(())
        }
    }

    fn zero(&mut self, kind: PageKind) -> Result<Accepted, Reason> {
        self.alloc_open()?;
        self.owned_state(kind, PageState::Allocated)?;
        self.pages[kind as usize].state = PageState::Zeroed;
        self.stay(Rule::DmaZero)
    }

    fn prepare(&mut self, kind: PageKind, check: impl FnOnce(&Self) -> Result<(), Reason>) -> Result<Accepted, Reason> {
        self.alloc_open()?;
        self.owned_state(kind, PageState::Zeroed)?;
        check(self)?;
        self.pages[kind as usize].state = PageState::Prepared;
        self.stay(Rule::DmaPrepare)
    }

    fn release(&mut self, kind: PageKind) -> Result<Accepted, Reason> {
        let terminal = self.phase.terminal();
        match self.pages[kind as usize].state {
            PageState::Free => Err(Reason::PageNotOwned),
            PageState::Published => {
                if self.phase != Phase::Quiesced {
                    Err(Reason::PageStillPublished)
                } else if self.policy == ReleasePolicy::Never {
                    Err(Reason::ReleaseForbiddenByPolicy)
                } else {
                    self.pages[kind as usize] = Page { state: PageState::Free, addr: 0 };
                    self.stay(Rule::CleanupRelease)
                }
            }
            _ => {
                self.pages[kind as usize] = Page { state: PageState::Free, addr: 0 };
                self.stay(if terminal { Rule::CleanupRelease } else { Rule::DmaRelease })
            }
        }
    }
}
