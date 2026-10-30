//! USB.2-C4.2a hosted session: the only sanctioned way for observations to reach the C4.1 `Machine`.
//!
//! A caller cannot hand the machine an observation. It asks the session to observe; the session reads typed
//! registers through the `Backend`, validates every read (register identity, width, session, strictly
//! consecutive sequence numbers, all-ones, reserved bits, impossible transitions), builds the machine
//! operation itself and feeds it. Before a state-changing operation it checks that the observations the write
//! depends on are still valid (a write invalidates the observations it can change), re-reads identity,
//! topology and the write's preconditions, only then lets the machine decide, issues a single-use ticket,
//! performs the write and verifies its read-back. The machine is committed only after the decision and the
//! (uncertain) outcome agree; an uncertain write is assumed to have happened in the more exposed direction.
//! Any fault closes the session fail-closed and drives the machine into its terminal path.
//!
//! Limits (see the C4.2a report): sequence numbers order what the backend did. They do NOT prove that the
//! hardware or firmware did not change state asynchronously, and a controller that reports false values
//! consistently cannot be detected by software. The re-reads narrow the check-then-act window; they do not close it.

use crate::backend::*;
use crate::backend::named;
use crate::dma_model::{DReason, DState, Item, Ledger, PoolSpec, PublishProof, QuiescenceProof};
use crate::machine::{MAX_PORTS, Machine, Page};
use crate::types::*;

named! {
    /// A refusal by the session itself (nothing consumed, state unchanged).
    pub enum SReason {
        NotBound => "not-bound",
        AlreadyBound => "already-bound",
        NotQualified => "not-qualified",
        UseBackendObservation => "use-backend-observation",
        StaleObservation => "stale-observation",
        Closed => "closed",
        CorePageViaOperation => "core-page-via-operation",
        ScratchpadArrayNotReady => "scratchpad-array-not-ready",
        NotClosed => "not-closed",
    }
}

named! {
    /// Why a session closed. A closed session accepts only the quarantine tail.
    pub enum Cause {
        ReadAllOnes => "read-all-ones",
        ReadWidth => "read-width",
        ReadMisaligned => "read-misaligned",
        ReadOutOfExtent => "read-out-of-extent",
        ReadTorn => "read-torn",
        ReadPartial => "read-partial",
        ReadBudget => "read-budget",
        LeaseLost => "lease-lost",
        ReadOther => "read-other",
        WrongRegister => "wrong-register",
        WidthMismatch => "width-mismatch",
        SequenceDuplicate => "sequence-duplicate",
        SequenceReordered => "sequence-reordered",
        SequenceGap => "sequence-gap",
        SessionMismatch => "session-mismatch",
        ClockBackwards => "clock-backwards",
        ReservedBitsSet => "reserved-bits-set",
        ImpossibleTransition => "impossible-transition",
        UnexpectedChange => "unexpected-change",
        UncorroboratedHalt => "uncorroborated-halt",
        IdentityChanged => "identity-changed",
        TopologyChanged => "topology-changed",
        AmbiguousTopology => "ambiguous-topology",
        PreconditionChanged => "precondition-changed",
        EventMismatch => "event-mismatch",
        WriteRefused => "write-refused",
        WriteUnverified => "write-unverified",
        WriteMismatch => "write-mismatch",
        WriteBadTicket => "write-bad-ticket",
        WriteFailed => "write-failed",
        LedgerDisagrees => "ledger-disagrees",
    }
}

/// A hardware fact the session may not know. `Unknown` is never treated as favourable.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fact<T: Copy> {
    Unknown,
    Known(T),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    Machine(Reason),
    Session(SReason),
    Ledger(DReason),
}

/// What the session did for one request.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Done {
    pub accepted: Option<Accepted>,
    /// Set when this request closed the session (or a tail fault ended the tail).
    pub closed: Option<Cause>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Req {
    Bind,
    Qualify,
    Tick,
    Status,
    Pci,
    Reset,
    Handoff,
    Ports,
    Event,
    Op(Operation),
    /// A scratchpad array or buffer page (the four core pages go through `Op`).
    Scratch(Scratch),
}

/// Scratchpad page lifecycle. Allocation, zeroing and preparation follow the machine's allocation window.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Scratch {
    Allocate { item: Item, addr: u64 },
    Zero(Item),
    Prepare(Item),
    Release(Item),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Config {
    /// The controller reads CRCR pointer bits as zero (xHCI 1.2 section 5.4.5). QEMU does not.
    pub crcr_reads_zero: bool,
}

// Observation kinds whose freshness a write can depend on.
const ST: usize = 0;
const PCI: usize = 1;
const HND: usize = 2;
const PRT: usize = 3;
const EVT: usize = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum W {
    ReqOs,
    Smi,
    ClearRs,
    ClearBme,
    Reset,
    Slots,
    Dcbaap,
    Crcr,
    Erstsz,
    ErdpInit,
    SetBme,
    Erstba,
    SetRs,
    Doorbell,
    Ack,
}

fn classify(op: &Operation, phase: Phase) -> Option<W> {
    Some(match op {
        Operation::RequestOsOwnership => W::ReqOs,
        Operation::DisableLegacySmi => W::Smi,
        Operation::ClearRunStop => W::ClearRs,
        Operation::ClearBusMaster => W::ClearBme,
        Operation::AssertReset => W::Reset,
        Operation::SetMaxSlots(_) => W::Slots,
        Operation::SetDcbaap(_) => W::Dcbaap,
        Operation::SetCommandRing { .. } => W::Crcr,
        Operation::SetErstSize(_) => W::Erstsz,
        Operation::SetEventDequeue { .. } => {
            if phase == Phase::Running { W::Ack } else { W::ErdpInit }
        }
        Operation::SetBusMaster => W::SetBme,
        Operation::SetErstBase(_) => W::Erstba,
        Operation::SetRunStop => W::SetRs,
        Operation::RingCommandDoorbell => W::Doorbell,
        _ => return None,
    })
}

/// Observations that must be valid (read after the last write that can change them) before the write.
fn requires(w: W) -> &'static [usize] {
    match w {
        W::ReqOs | W::Smi => &[HND],
        W::ClearRs | W::Doorbell => &[ST],
        W::ClearBme | W::Reset | W::SetBme | W::Erstba | W::SetRs => &[ST, PCI],
        W::Slots | W::Dcbaap | W::Crcr | W::Erstsz | W::ErdpInit => &[ST],
        W::Ack => &[EVT],
    }
}

/// Observations a write invalidates.
fn invalidates(w: W) -> &'static [usize] {
    match w {
        W::ReqOs | W::Smi => &[HND],
        W::ClearRs | W::SetRs | W::Erstba => &[ST],
        W::ClearBme | W::SetBme => &[PCI],
        W::Reset => &[ST, PRT, EVT],
        W::Doorbell => &[ST, EVT],
        W::Ack => &[EVT],
        W::Slots | W::Dcbaap | W::Crcr | W::Erstsz | W::ErdpInit => &[],
    }
}

const HCH: u64 = 1;
const HSE: u64 = 1 << 2;
const CNR: u64 = 1 << 11;
const HCE: u64 = 1 << 12;

/// Register re-read immediately before the write: (register, mask, expected masked value).
fn preconditions(w: W) -> &'static [(Reg, u64, u64)] {
    match w {
        W::ReqOs => &[(Reg::Usblegsup, 1 << 24, 0)],
        W::Smi => &[(Reg::Usblegsup, (1 << 24) | (1 << 16), 1 << 24)],
        W::ClearRs => &[(Reg::Usbcmd, 1, 1)],
        W::ClearBme => &[(Reg::Usbsts, HCH, HCH), (Reg::Usbcmd, 1, 0), (Reg::PciCommand, 4, 4)],
        W::Reset => &[(Reg::PciCommand, 4, 0), (Reg::Usbsts, HCH, HCH)],
        W::Slots | W::Dcbaap | W::Crcr | W::Erstsz | W::ErdpInit => &[(Reg::Usbsts, CNR | HCH, HCH)],
        W::SetBme => &[(Reg::Usbsts, HCH, HCH), (Reg::PciCommand, 4, 0)],
        W::Erstba => &[(Reg::PciCommand, 4, 4), (Reg::Usbsts, HCH | HCE, HCH)],
        W::SetRs => &[(Reg::Usbsts, HCE | HSE, 0), (Reg::PciCommand, 4, 4)],
        W::Doorbell => &[(Reg::Usbsts, HCH | HCE | HSE, 0)],
        W::Ack => &[(Reg::Usbsts, HCE, 0)],
    }
}

/// Read-back required after the write: (register, mask, expected).
fn readback_expect(w: W, ev: &ModelEvent) -> Option<(Reg, u64, u64)> {
    Some(match w {
        W::ReqOs => (Reg::Usblegsup, 1 << 24, 1 << 24),
        W::ClearRs => (Reg::Usbcmd, 1, 0),
        W::SetRs => (Reg::Usbcmd, 1, 1),
        W::ClearBme => (Reg::PciCommand, 4, 0),
        W::SetBme => (Reg::PciCommand, 4, 4),
        W::Slots => (Reg::Config, 0xff, 1),
        W::Dcbaap => (Reg::Dcbaap, !0x3f, ev.value),
        W::Erstsz => (Reg::Erstsz, 0xffff, 1),
        W::ErdpInit => (Reg::Erdp, !0xf, ev.value),
        W::Erstba => (Reg::Erstba, !0x3f, ev.value),
        W::Smi | W::Reset | W::Crcr | W::Doorbell | W::Ack => return None,
    })
}

/// Writes that reduce exposure keep the pre-write belief when their outcome is uncertain; every other
/// write is assumed to have happened. In both cases the more exposed world is the one the machine keeps.
fn reduces_exposure(w: W) -> bool {
    matches!(w, W::ClearRs | W::ClearBme | W::Reset)
}

pub struct Session<B: Backend> {
    b: B,
    m: Machine,
    cfg: Config,
    bound: Option<(Identity, Topology, u32)>,
    ports: u8,
    last_seq: Option<u64>,
    last_tick: u64,
    last_now: u64,
    last_write_seq: u64,
    fresh: [bool; 5],
    know_bme: Fact<bool>,
    know_rs: Fact<bool>,
    know_hch: Fact<bool>,
    last_cnr: Option<bool>,
    nonce: u64,
    closed: Option<Cause>,
    latched: bool,
    tail_dead: bool,
    ledger: Option<Ledger>,
    mirrored: [Page; 4],
    quiesced: bool,
}

const KINDS: [PageKind; 4] = [PageKind::Command, PageKind::Event, PageKind::Dcbaa, PageKind::Erst];
/// Enough for every page the plan can hold (4 core, the array and up to the scratchpad budget), so a single failed
/// session can always be fully quarantined; the cap only bounds a pool that is reused.
const QUARANTINE_CAP: u8 = 80;

fn bit(v: u64, n: u32) -> bool {
    v >> n & 1 != 0
}

impl<B: Backend> Session<B> {
    pub fn new(b: B, m: Machine, cfg: Config) -> Self {
        Self {
            b,
            m,
            cfg,
            bound: None,
            ports: 0,
            last_seq: None,
            last_tick: 0,
            last_now: 0,
            last_write_seq: 0,
            fresh: [false; 5],
            know_bme: Fact::Unknown,
            know_rs: Fact::Unknown,
            know_hch: Fact::Unknown,
            last_cnr: None,
            nonce: 1,
            closed: None,
            latched: false,
            tail_dead: false,
            ledger: None,
            mirrored: [Page { state: PageState::Free, addr: 0 }; 4],
            quiesced: false,
        }
    }

    pub fn machine(&self) -> &Machine {
        &self.m
    }
    pub fn backend(&self) -> &B {
        &self.b
    }
    pub fn backend_mut(&mut self) -> &mut B {
        &mut self.b
    }
    pub fn closed(&self) -> Option<Cause> {
        self.closed
    }
    pub fn is_bound(&self) -> bool {
        self.bound.is_some()
    }
    pub fn last_seq(&self) -> Option<u64> {
        self.last_seq
    }
    pub fn last_write_seq(&self) -> u64 {
        self.last_write_seq
    }
    /// Is the observation kind (0 status, 1 pci, 2 handoff, 3 ports, 4 event) valid right now?
    pub fn fresh(&self, kind: usize) -> bool {
        self.fresh.get(kind).copied().unwrap_or(false)
    }
    pub fn knowledge(&self) -> (Fact<bool>, Fact<bool>, Fact<bool>) {
        (self.know_bme, self.know_rs, self.know_hch)
    }
    pub fn ports(&self) -> u8 {
        self.ports
    }
    /// The model-only DMA ownership ledger, once the machine has accepted qualification.
    pub fn ledger(&self) -> Option<&Ledger> {
        self.ledger.as_ref()
    }
    fn sealed(&self) -> bool {
        self.closed.is_some() || self.m.phase().terminal()
    }

    // ---- request dispatch -----------------------------------------------------------------

    /// One request. After it, the DMA ledger is brought in line with whatever the machine accepted; a ledger that
    /// cannot follow the machine is a model inconsistency and closes the session.
    pub fn request(&mut self, r: Req) -> Result<Done, Refusal> {
        let out = self.dispatch(r);
        match self.sync() {
            Ok(()) => out,
            Err(c) => {
                let closed = self.close(c);
                match out {
                    Ok(d) if d.closed.is_none() => Ok(Done { accepted: d.accepted, closed: closed.closed }),
                    Ok(d) => Ok(d),
                    Err(_) => Ok(closed),
                }
            }
        }
    }

    #[inline(never)]
    fn dispatch(&mut self, r: Req) -> Result<Done, Refusal> {
        match r {
            Req::Bind => return self.bind(),
            Req::Op(Operation::Qualify(_) | Operation::Tick(_) | Operation::ObserveHandoff { .. } | Operation::ObserveStatus(_)
                | Operation::ObserveReset(_) | Operation::ObservePci { .. } | Operation::ObservePorts { .. } | Operation::ConsumeEvent(_)) => {
                return Err(Refusal::Session(SReason::UseBackendObservation));
            }
            _ => {}
        }
        if self.bound.is_none() {
            return Err(Refusal::Session(if self.closed.is_some() { SReason::Closed } else { SReason::NotBound }));
        }
        let tail_ok = matches!(
            r,
            Req::Status | Req::Pci | Req::Scratch(Scratch::Release(_))
                | Req::Op(Operation::ClearRunStop | Operation::ClearBusMaster | Operation::DmaRelease(_))
        );
        if self.sealed() && !(tail_ok && self.m.phase() != Phase::Idle && !self.tail_dead) {
            return Err(Refusal::Session(SReason::Closed));
        }
        match r {
            Req::Qualify => self.qualify(),
            Req::Tick => self.tick(),
            Req::Status | Req::Pci | Req::Reset | Req::Handoff | Req::Ports | Req::Event => {
                if self.m.phase() == Phase::Idle {
                    return Err(Refusal::Session(SReason::NotQualified));
                }
                match r {
                    Req::Status => self.run(|s| s.obs_status()),
                    Req::Pci => self.run(|s| s.obs_pci()),
                    Req::Reset => self.run(|s| s.obs_reset()),
                    Req::Handoff => self.run(|s| s.obs_handoff()),
                    Req::Ports => self.run(|s| s.obs_ports()),
                    _ => self.run(|s| s.obs_event()),
                }
            }
            Req::Scratch(s) => self.scratch(s),
            Req::Op(op) => {
                if let Operation::PrepareDcbaa { entry0 } = op
                    && !self.array_ready(entry0)
                {
                    return Err(Refusal::Session(SReason::ScratchpadArrayNotReady));
                }
                let classified = if self.m.phase() == Phase::Idle { None } else { classify(&op, self.m.phase()) };
                if let Some(w) = classified {
                    self.write(op, w)
                } else {
                    let mut trial = self.m.clone();
                    let a = trial.apply(op).map_err(Refusal::Machine)?;
                    // The machine cannot see scratchpad pages: a core page may not land on one.
                    if let Operation::DmaAllocate { addr, .. } = op
                        && self.ledger.as_ref().is_some_and(|l| l.scratch_overlaps(addr))
                    {
                        return Err(Refusal::Ledger(DReason::Overlap));
                    }
                    self.m = trial;
                    self.settle(&a);
                    Ok(Done { accepted: Some(a), closed: None })
                }
            }
            Req::Bind => unreachable!(),
        }
    }

    fn settle(&mut self, a: &Accepted) {
        if a.to.terminal() && !self.latched {
            self.latched = true;
            self.b.latch_terminal();
        }
    }

    fn run(&mut self, f: impl FnOnce(&mut Self) -> Result<Result<Accepted, Reason>, Cause>) -> Result<Done, Refusal> {
        match f(self) {
            Ok(Ok(a)) => {
                self.settle(&a);
                Ok(Done { accepted: Some(a), closed: None })
            }
            Ok(Err(r)) => Err(Refusal::Machine(r)),
            Err(c) => Ok(self.close(c)),
        }
    }

    /// Fail closed: record the cause, latch the backend and drive the machine into its terminal path.
    #[inline(never)]
    fn close(&mut self, c: Cause) -> Done {
        if self.sealed() {
            // A fault inside the quarantine tail ends the tail: nothing can still quiesce the controller, so
            // every page it may be using is kept forever.
            self.tail_dead = true;
            if let Some(l) = self.ledger.as_mut() {
                l.quarantine_all();
            }
            return Done { accepted: None, closed: Some(c) };
        }
        self.closed = Some(c);
        let mut accepted = None;
        if self.m.phase() != Phase::Idle {
            accepted = self.m.apply(Operation::BackendFault).ok();
        }
        if !self.latched {
            self.latched = true;
            self.b.latch_terminal();
        }
        Done { accepted, closed: Some(c) }
    }

    // ---- DMA ledger ---------------------------------------------------------------------------------------

    /// DCBAA entry 0 must be the prepared scratchpad array when the controller needs scratchpads, and 0 otherwise.
    fn array_ready(&self, entry0: u64) -> bool {
        let Some(l) = &self.ledger else { return true };
        if l.spec().scratchpads == 0 {
            return true;
        }
        l.state(Item::Array) == Some(DState::Prepared) && l.address(Item::Array) == Some(entry0)
    }

    fn scratch(&mut self, s: Scratch) -> Result<Done, Refusal> {
        let item = match s {
            Scratch::Allocate { item, .. } | Scratch::Zero(item) | Scratch::Prepare(item) | Scratch::Release(item) => item,
        };
        if matches!(item, Item::Core(_)) {
            return Err(Refusal::Session(SReason::CorePageViaOperation));
        }
        let Some(l) = self.ledger.as_mut() else { return Err(Refusal::Session(SReason::NotQualified)) };
        if !matches!(s, Scratch::Release(_)) && !self.m.allocation_open() {
            return Err(Refusal::Machine(Reason::AllocationClosed));
        }
        let r = match s {
            Scratch::Allocate { item, addr } => l.allocate(item, addr),
            Scratch::Zero(item) => l.zero(item),
            Scratch::Prepare(item) => l.prepare(item),
            Scratch::Release(item) => l.release(item),
        };
        r.map_err(Refusal::Ledger)?;
        Ok(Done { accepted: None, closed: None })
    }

    /// Replay what the machine accepted onto the ledger. Every legal machine step has exactly one ledger step; any
    /// other change, or a ledger that refuses one, is a disagreement.
    #[inline(never)]
    fn sync(&mut self) -> Result<(), Cause> {
        let Some(l) = self.ledger.as_mut() else { return Ok(()) };
        for (i, &k) in KINDS.iter().enumerate() {
            let (a, b) = (self.mirrored[i], self.m.page(k));
            if a == b {
                continue;
            }
            let item = Item::Core(k);
            let r = match (a.state, b.state) {
                (PageState::Free, PageState::Allocated) => l.allocate(item, b.addr),
                (PageState::Allocated, PageState::Zeroed) => l.zero(item),
                (PageState::Zeroed, PageState::Prepared) => l.prepare(item),
                (PageState::Prepared, PageState::Published) if k == PageKind::Dcbaa => l.publish_dcbaa(&PublishProof::new()),
                (PageState::Prepared, PageState::Published) => l.publish(k, &PublishProof::new()),
                (_, PageState::Free) => l.release(item),
                _ => Err(DReason::BadState),
            };
            if r.is_err() {
                return Err(Cause::LedgerDisagrees);
            }
            self.mirrored[i] = b;
        }
        if self.m.phase() == Phase::Quiesced && !self.quiesced {
            self.quiesced = true;
            // `NothingToQuiesce` (nothing was ever published) is the only way this can fail, and it is not a fault.
            let _ = l.quiesce(&QuiescenceProof::new());
        }
        Ok(())
    }

    /// Give up on the DMA pages of a closed session: every live page is kept forever (the device may still own
    /// them). Returns how many were newly quarantined; the pool closes if the quarantine bound is exceeded.
    pub fn abandon(&mut self) -> Result<usize, SReason> {
        if !self.sealed() {
            return Err(SReason::NotClosed);
        }
        Ok(self.ledger.as_mut().map_or(0, Ledger::quarantine_all))
    }

    // ---- stamped, typed reads -----------------------------------------------------------------

    fn stamp(&mut self, st: Stamp, session: u32) -> Result<(), Cause> {
        if let Some((_, _, s)) = self.bound
            && s != session
        {
            return Err(Cause::SessionMismatch);
        }
        match self.last_seq {
            Some(l) if st.seq == l => return Err(Cause::SequenceDuplicate),
            Some(l) if st.seq < l => return Err(Cause::SequenceReordered),
            Some(l) if st.seq > l + 1 => return Err(Cause::SequenceGap),
            _ => {}
        }
        if st.tick < self.last_tick {
            return Err(Cause::ClockBackwards);
        }
        self.last_seq = Some(st.seq);
        self.last_tick = st.tick;
        Ok(())
    }

    fn fault_cause(k: ReadFault) -> Cause {
        match k {
            ReadFault::AllOnes => Cause::ReadAllOnes,
            ReadFault::Width => Cause::ReadWidth,
            ReadFault::Misaligned => Cause::ReadMisaligned,
            ReadFault::OutOfExtent => Cause::ReadOutOfExtent,
            ReadFault::Torn => Cause::ReadTorn,
            ReadFault::Partial => Cause::ReadPartial,
            ReadFault::Budget => Cause::ReadBudget,
            ReadFault::LeaseLost => Cause::LeaseLost,
            ReadFault::NotPresent | ReadFault::Other => Cause::ReadOther,
        }
    }

    /// One typed register read; `Ok(None)` for an optional register that does not exist.
    fn rd(&mut self, reg: Reg, index: u8) -> Result<Option<u64>, Cause> {
        match self.b.read(reg, index) {
            Err(e) => {
                self.stamp(e.stamp, e.session)?;
                if e.kind == ReadFault::NotPresent {
                    return Ok(None);
                }
                Err(Self::fault_cause(e.kind))
            }
            Ok(r) => {
                self.stamp(r.stamp, r.session)?;
                if r.reg != reg || r.index != index {
                    return Err(Cause::WrongRegister);
                }
                if r.width != reg.width() {
                    return Err(Cause::WidthMismatch);
                }
                if r.value == r.width.ones() {
                    return Err(Cause::ReadAllOnes);
                }
                Ok(Some(r.value))
            }
        }
    }
    fn rd_need(&mut self, reg: Reg, index: u8) -> Result<u64, Cause> {
        self.rd(reg, index)?.ok_or(Cause::ReadOther)
    }

    fn rd_stamped<T: Copy>(&mut self, r: ReadResult<T>) -> Result<T, Cause> {
        match r {
            Err(e) => {
                self.stamp(e.stamp, e.session)?;
                Err(Self::fault_cause(e.kind))
            }
            Ok(s) => {
                self.stamp(s.stamp, s.session)?;
                Ok(s.value)
            }
        }
    }

    // ---- bind, qualify, time ------------------------------------------------------------------

    fn bind(&mut self) -> Result<Done, Refusal> {
        if self.bound.is_some() {
            return Err(Refusal::Session(SReason::AlreadyBound));
        }
        if self.sealed() {
            return Err(Refusal::Session(SReason::Closed));
        }
        let r = self.b.read_identity();
        let sess = match &r {
            Ok(s) => s.session,
            Err(e) => e.session,
        };
        self.bound = Some((ZERO_IDENT, ZERO_TOPO, sess));
        let result = (|| {
            let id = self.rd_stamped(r)?;
            let t = self.b.read_topology();
            let topo = self.rd_stamped(t)?;
            if topo.xhci_functions != 1 || !topo.scan_complete {
                return Err(Cause::AmbiguousTopology);
            }
            self.bound = Some((id, topo, sess));
            Ok(())
        })();
        match result {
            Ok(()) => Ok(Done { accepted: None, closed: None }),
            Err(c) => {
                self.bound = None;
                self.closed = Some(c);
                self.latched = true;
                self.b.latch_terminal();
                Ok(Done { accepted: None, closed: Some(c) })
            }
        }
    }

    #[inline(never)]
    fn revalidate(&mut self) -> Result<(), Cause> {
        let (id0, topo0, _) = self.bound.ok_or(Cause::ReadOther)?;
        let r = self.b.read_identity();
        let id = self.rd_stamped(r)?;
        let t = self.b.read_topology();
        let topo = self.rd_stamped(t)?;
        if id != id0 {
            return Err(Cause::IdentityChanged);
        }
        if topo != topo0 {
            return Err(Cause::TopologyChanged);
        }
        Ok(())
    }

    fn qualify(&mut self) -> Result<Done, Refusal> {
        if self.m.phase() != Phase::Idle {
            return Err(Refusal::Machine(Reason::AlreadyQualified));
        }
        self.run(|s| {
            let p = s.b.read_platform();
            let plat = s.rd_stamped(p)?;
            let ver = s.rd_need(Reg::Hciversion, 0)?;
            let hcs1 = s.rd_need(Reg::Hcsparams1, 0)?;
            let hcs2 = s.rd_need(Reg::Hcsparams2, 0)?;
            let hcc1 = s.rd_need(Reg::Hccparams1, 0)?;
            let pagesize = s.rd_need(Reg::Pagesize, 0)?;
            let cmd = s.rd_need(Reg::PciCommand, 0)?;
            let devctl = s.rd(Reg::PcieDevCtl, 0)?;
            let msi = s.rd(Reg::PciMsiControl, 0)?;
            let msix = s.rd(Reg::PciMsixControl, 0)?;
            let usbcmd = s.rd_need(Reg::Usbcmd, 0)?;
            let usbsts = s.rd_need(Reg::Usbsts, 0)?;
            let legacy = s.rd(Reg::Usblegsup, 0)?;
            let dbc = s.rd(Reg::DbcCtrl, 0)?;
            let ports = (hcs1 >> 24) as u8;
            let mut connected = 0u32;
            if ports <= MAX_PORTS {
                for p in 0..ports {
                    if bit(s.rd_need(Reg::Portsc, p)?, 0) {
                        connected |= 1 << p;
                    }
                }
            }
            let facts = Facts {
                profile: plat.profile,
                version: ver as u16,
                page_4k: pagesize & 1 != 0,
                slots: hcs1 as u8,
                ports,
                interrupters: ((hcs1 >> 8) & 0x7ff) as u16,
                scratchpads: scratchpads(hcs2),
                legacy: legacy.map_or(Legacy::Absent, |v| Legacy::Present { bios: bit(v, 16), os: bit(v, 24) }),
                dbc: dbc.map_or(Dbc::Absent, |v| if bit(v, 31) { Dbc::Enabled } else { Dbc::Disabled }),
                iommu: plat.iommu,
                identity_dma: plat.identity_dma,
                no_snoop: devctl.map_or(Tri::Unknown, |v| if bit(v, 11) { Tri::Yes } else { Tri::No }),
                msi: msi.is_some_and(|v| bit(v, 0)) || msix.is_some_and(|v| bit(v, 15)),
                bme: bit(cmd, 2),
                running: bit(usbcmd, 0),
                halted: bit(usbsts, 0),
                hse: usbsts & HSE != 0,
                hce: usbsts & HCE != 0,
                connected,
                dma_limit: plat.dma_limit,
            };
            if usbsts & RSVD_STS != 0 {
                return Err(Cause::ReservedBitsSet);
            }
            let a = s.m.apply(Operation::Qualify(facts));
            if a.is_ok() {
                s.ports = ports.min(MAX_PORTS);
                s.know_bme = Fact::Known(facts.bme);
                s.know_rs = Fact::Known(facts.running);
                s.know_hch = Fact::Known(facts.halted);
                s.last_cnr = Some(usbsts & CNR != 0);
                s.fresh = [true, true, facts.legacy != Legacy::Absent, true, false];
            }
            // A refused qualification plans no DMA at all, so only an accepted one gets a ledger.
            if a.is_ok() && !s.m.phase().terminal() {
                let spec = PoolSpec {
                    device_limit: facts.dma_limit,
                    ac64: bit(hcc1, 0),
                    scratchpads: facts.scratchpads,
                    quarantine_cap: QUARANTINE_CAP,
                    release: s.m.policy(),
                };
                match Ledger::new(spec) {
                    Ok(l) => s.ledger = Some(l),
                    Err(_) => return Err(Cause::LedgerDisagrees),
                }
            }
            Ok(a)
        })
    }

    fn tick(&mut self) -> Result<Done, Refusal> {
        let now = self.b.now();
        if now < self.last_now {
            return Ok(self.close(Cause::ClockBackwards));
        }
        let mut trial = self.m.clone();
        let a = trial.apply(Operation::Tick(now)).map_err(Refusal::Machine)?;
        self.last_now = now;
        self.m = trial;
        self.settle(&a);
        Ok(Done { accepted: Some(a), closed: None })
    }

    // ---- observations -------------------------------------------------------------------------------

    /// USBSTS with contradiction checks. `corroborate`: a halted report is cross-checked against USBCMD.R/S
    /// and a second USBSTS read when the session does not itself believe the controller is running.
    fn read_usbsts(&mut self, corroborate: bool) -> Result<u64, Cause> {
        let v = self.rd_need(Reg::Usbsts, 0)?;
        if v & RSVD_STS != 0 {
            return Err(Cause::ReservedBitsSet);
        }
        let halted = v & HCH != 0;
        // A controller observed halted cannot start unless R/S was set; while we believe R/S is clear it must stay halted.
        if self.know_hch == Fact::Known(true) && !halted && self.know_rs == Fact::Known(false) {
            return Err(Cause::ImpossibleTransition);
        }
        if self.last_cnr == Some(false) && v & CNR != 0 {
            return Err(Cause::ImpossibleTransition);
        }
        if corroborate && halted && self.know_rs != Fact::Known(true) {
            let cmd = self.rd_need(Reg::Usbcmd, 0)?;
            let again = self.rd_need(Reg::Usbsts, 0)?;
            if bit(cmd, 0) || again & HCH == 0 || again & RSVD_STS != 0 {
                return Err(Cause::UncorroboratedHalt);
            }
        }
        self.last_cnr = Some(v & CNR != 0);
        self.know_hch = Fact::Known(halted);
        Ok(v)
    }

    fn obs_status(&mut self) -> Result<Result<Accepted, Reason>, Cause> {
        let v = self.read_usbsts(true)?;
        self.fresh[ST] = true;
        let a = self.m.apply(Operation::ObserveStatus(Status { hch: v & HCH != 0, hse: v & HSE != 0, hce: v & HCE != 0 }));
        Ok(a)
    }

    fn read_pci_bme(&mut self) -> Result<bool, Cause> {
        let cmd = self.rd_need(Reg::PciCommand, 0)?;
        let bme = bit(cmd, 2);
        if let Fact::Known(k) = self.know_bme
            && k != bme
        {
            return Err(Cause::UnexpectedChange);
        }
        self.know_bme = Fact::Known(bme);
        Ok(bme)
    }

    fn obs_pci(&mut self) -> Result<Result<Accepted, Reason>, Cause> {
        let bme = self.read_pci_bme()?;
        let msi = self.rd(Reg::PciMsiControl, 0)?.is_some_and(|v| bit(v, 0));
        let msix = self.rd(Reg::PciMsixControl, 0)?.is_some_and(|v| bit(v, 15));
        self.fresh[PCI] = true;
        let a = self.m.apply(Operation::ObservePci { bme, msi: msi || msix });
        Ok(a)
    }

    fn obs_reset(&mut self) -> Result<Result<Accepted, Reason>, Cause> {
        let cmd = self.rd_need(Reg::Usbcmd, 0)?;
        let sts = self.read_usbsts(false)?;
        let bme = self.read_pci_bme()?;
        let config = self.rd_need(Reg::Config, 0)?;
        let crcr = self.rd_need(Reg::Crcr, 0)?;
        let dcbaap = self.rd_need(Reg::Dcbaap, 0)?;
        let erstsz = self.rd_need(Reg::Erstsz, 0)?;
        let erstba = self.rd_need(Reg::Erstba, 0)?;
        let erdp = self.rd_need(Reg::Erdp, 0)?;
        let mut quiet = true;
        for p in 0..self.ports {
            // connection, enable, over-current and reset; change bits are ignored
            quiet &= self.rd_need(Reg::Portsc, p)? & 0b1_1011 == 0;
        }
        let crcr_ok = if self.cfg.crcr_reads_zero { crcr == 0 } else { true };
        let r = ResetState {
            cnr: sts & CNR != 0,
            hch: sts & HCH != 0,
            usbcmd_zero: cmd == 0,
            pointers_zero: config == 0 && dcbaap == 0 && erstsz == 0 && erstba == 0 && erdp == 0 && crcr_ok,
            bme,
            ports_quiet: quiet,
        };
        let a = self.m.apply(Operation::ObserveReset(r));
        if a.is_ok() {
            self.fresh[ST] = true;
            self.fresh[PCI] = true;
            self.fresh[PRT] = true;
        }
        Ok(a)
    }

    fn obs_handoff(&mut self) -> Result<Result<Accepted, Reason>, Cause> {
        let Some(v) = self.rd(Reg::Usblegsup, 0)? else {
            return Ok(Err(Reason::NoLegacyCapability));
        };
        let a = self.m.apply(Operation::ObserveHandoff { bios: bit(v, 16), os: bit(v, 24) });
        if a.is_ok() {
            self.fresh[HND] = true;
        }
        Ok(a)
    }

    fn obs_ports(&mut self) -> Result<Result<Accepted, Reason>, Cause> {
        let mut connected = 0u32;
        for p in 0..self.ports {
            if bit(self.rd_need(Reg::Portsc, p)?, 0) {
                connected |= 1 << p;
            }
        }
        let a = self.m.apply(Operation::ObservePorts { connected });
        if a.is_ok() {
            self.fresh[PRT] = true;
        }
        Ok(a)
    }

    fn obs_event(&mut self) -> Result<Result<Accepted, Reason>, Cause> {
        let (index, ccs) = self.m.event_cursor();
        let r = self.b.read_event(index, ccs);
        let e = self.rd_stamped(r)?;
        if e.is_some_and(|t| t.index != index) {
            return Err(Cause::EventMismatch);
        }
        let a = self.m.apply(Operation::ConsumeEvent(e));
        if a.is_ok() {
            self.fresh[EVT] = true;
        }
        Ok(a)
    }

    // ---- state-changing operations ----------------------------------------------------------------------

    fn write(&mut self, op: Operation, w: W) -> Result<Done, Refusal> {
        // The quarantine tail can only supply a status read (the machine's tail asks for nothing else).
        let sealed = self.sealed();
        let reqs: &[usize] = if sealed { if w == W::ClearBme { &[ST] } else { &[] } } else { requires(w) };
        for &k in reqs {
            if !self.fresh[k] {
                return Err(Refusal::Session(SReason::StaleObservation));
            }
        }
        // The machine decides first; a refusal consumes nothing from the backend.
        let mut trial = self.m.clone();
        let acc = trial.apply(op).map_err(Refusal::Machine)?;
        let Some(ev) = acc.event else {
            self.m = trial;
            return Ok(Done { accepted: Some(acc), closed: None });
        };
        if let Err(c) = self.precheck(w, sealed) {
            return Ok(self.close(c));
        }
        let outcome = self.perform_checked(&ev, w);
        // Whatever happened, the observations this write can change are no longer valid.
        for &k in invalidates(w) {
            self.fresh[k] = false;
        }
        let verified = outcome.is_ok();
        self.after_write(w, verified);
        match outcome {
            Ok(()) => {
                self.m = trial;
                self.settle(&acc);
                Ok(Done { accepted: Some(acc), closed: None })
            }
            Err(c) => {
                if !reduces_exposure(w) {
                    self.m = trial;
                }
                Ok(self.close(c))
            }
        }
    }

    /// Identity, topology and the write's own preconditions, re-read immediately before the write. Kept out of
    /// `write` so that its temporaries do not add to the frame of the function that also holds the trial machine.
    #[inline(never)]
    fn precheck(&mut self, w: W, sealed: bool) -> Result<(), Cause> {
        if sealed {
            return Ok(());
        }
        self.revalidate()?;
        for &(reg, mask, expect) in preconditions(w) {
            let v = self.rd_need(reg, 0)?;
            if v & mask != expect {
                return Err(Cause::PreconditionChanged);
            }
        }
        Ok(())
    }

    /// Issue the single-use ticket, perform the write and check the receipt and the read-back.
    #[inline(never)]
    fn perform_checked(&mut self, ev: &ModelEvent, w: W) -> Result<(), Cause> {
        let ticket = WriteTicket::new(self.nonce, ev.id);
        self.nonce += 1;
        match self.b.perform(ev, ticket) {
            Err(e) => {
                self.stamp(e.stamp, e.session)?;
                self.last_write_seq = e.stamp.seq;
                Err(match e.kind {
                    WriteFault::Refused => Cause::WriteRefused,
                    WriteFault::BadTicket => Cause::WriteBadTicket,
                    WriteFault::Unverified => Cause::WriteUnverified,
                    WriteFault::Fault => Cause::WriteFailed,
                })
            }
            Ok(r) => {
                self.stamp(r.stamp, r.session)?;
                self.last_write_seq = r.stamp.seq;
                match readback_expect(w, ev) {
                    None => Ok(()),
                    Some((reg, mask, expect)) => match r.readback {
                        None => Err(Cause::WriteUnverified),
                        Some(rb) if rb.reg != reg || rb.width != reg.width() => Err(Cause::WriteMismatch),
                        Some(rb) if rb.value & mask != expect => Err(Cause::WriteMismatch),
                        Some(_) => Ok(()),
                    },
                }
            }
        }
    }

    #[inline(never)]
    fn after_write(&mut self, w: W, verified: bool) {
        let known = |v: bool| if verified { Fact::Known(v) } else { Fact::Unknown };
        match w {
            W::ClearBme => self.know_bme = known(false),
            W::SetBme => self.know_bme = known(true),
            W::ClearRs | W::Reset => {
                self.know_rs = known(false);
                self.know_hch = Fact::Unknown;
            }
            W::SetRs => {
                self.know_rs = known(true);
                self.know_hch = Fact::Unknown;
            }
            _ => {}
        }
        if w == W::Reset {
            // CNR is legitimately set again after HCRST: the previous reading no longer constrains it.
            self.last_cnr = None;
        }
    }

    // ---- proofs for the DMA ledger --------------------------------------------------------------------------

    /// A publication proof exists only while the machine holds the page as published.
    pub fn publish_proof(&self, kind: PageKind) -> Option<crate::dma_model::PublishProof> {
        if self.closed.is_none() && self.m.page(kind).state == PageState::Published {
            Some(crate::dma_model::PublishProof::new())
        } else {
            None
        }
    }
    /// A quiescence proof exists only when the machine reached `Quiesced` (reset verified after exposure).
    pub fn quiescence_proof(&self) -> Option<crate::dma_model::QuiescenceProof> {
        if self.m.phase() == Phase::Quiesced { Some(crate::dma_model::QuiescenceProof::new()) } else { None }
    }
}

/// USBSTS reserved-zero bits (xHCI 1.2 Table 5-21): 1, 5, 6, 7 and 31:13.
const RSVD_STS: u64 = 0xFFFF_E0E2;

const ZERO_IDENT: Identity = Identity {
    bdf: 0,
    vendor: 0,
    device: 0,
    revision: 0,
    class: 0,
    subsystem: 0,
    bar0: 0,
    hciversion: 0,
    hcs1: 0,
    hcs2: 0,
    hcs3: 0,
    hcc1: 0,
    dboff: 0,
    rtsoff: 0,
};
const ZERO_TOPO: Topology = Topology { xhci_functions: 0, scan_complete: false, behind_bridge: false };

/// HCSPARAMS2 Max Scratchpad Buffers: Hi in bits 25:21, Lo in bits 31:27 (xHCI 1.2 Table 5-12).
pub fn scratchpads(hcs2: u64) -> u16 {
    ((((hcs2 >> 21) & 0x1f) << 5) | ((hcs2 >> 27) & 0x1f)) as u16
}
