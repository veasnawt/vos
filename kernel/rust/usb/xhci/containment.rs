//! USB.2-C3b.1 controller containment policy and dry-run state machine.
//!
//! A rehearsal of the proposed C3b.2 sequence: eligibility, revalidation and the
//! auditable list of the writes that *would* be made. **Nothing here, and
//! nothing in the C3b.1 kernel, writes anything.** The proposed effects are
//! plain values in a trace; no function consumes one, and the platform exports
//! no PCI-write or register-write adapter. C3b.2 would add exactly two
//! single-purpose adapters and flip `ISSUED`; until then, a rehearsal can only
//! observe.
//!
//! The proposed sequence (QEMU xHCI profile only), after full revalidation:
//!   W1  PCI configuration, 16-bit write at offset 0x04: clear bit 2 (bus master)
//!   W2  xHCI USBCMD (BAR0 + CAPLENGTH), 32-bit write: HCRST only
//! with a bounded poll for HCRST = 0 and CNR = 0, then a pointer-zero check.
//! Interrupts are not masked: eligibility requires them to be disabled already.
use super::lifecycle::{self, Facts, Step};
use super::residue::{DEVICES, SCRATCHPADS, fold};

/// PCI command register the eligible profile must show (I/O, memory, bus master).
pub const COMMAND: u16 = 0x0007;
pub const STATUS: u16 = 0x0010;
pub const BUS_MASTER: u16 = 0x0004;
/// W1: configuration offset and width (a 32-bit write would touch RW1C status).
pub const W1_OFFSET: u32 = 0x04;
pub const W1_WIDTH: u8 = 16;
/// W2: USBCMD relative to the operational base, width, and the only bit written.
pub const W2_WIDTH: u8 = 32;
pub const HCRST: u32 = 0x2;
pub const IDLE_USBSTS: u32 = 0x1;
/// Reset poll bound: iterations of (USBCMD, USBSTS) reads. Time is not modelled.
pub const POLL_BOUND: u32 = 1024;
const CNR: u32 = 1 << 11;
const PORT_BUSY: u32 = 0x00fe_0001; // CCS and every change bit
pub const ID: u32 = 0x000d_1b36;
pub const CLASS_REVISION: u32 = 0x0c03_3001;
/// CAPLENGTH/HCIVERSION, HCSPARAMS1-3, HCCPARAMS1, DBOFF, RTSOFF.
pub const HEADER: [u32; 7] = [0x0100_0040, 0x0800_1040, 0x0000_000f, 0, 0x0008_7001, 0x2000, 0x1000];
pub const PCIE_ABSENT: u8 = 0;
pub const PCIE_IDLE: u8 = 1;
pub const PCIE_PENDING: u8 = 2;

// Refusal bits: every one is evaluated, none short-circuits.
pub const NOT_UEFI: u32 = 1 << 0;
pub const UNQUALIFIED: u32 = 1 << 1;
pub const PROFILE: u32 = 1 << 2;
pub const PCI_COMMAND: u32 = 1 << 3;
pub const PCI_STATUS: u32 = 1 << 4;
pub const PCIE_UNKNOWN: u32 = 1 << 5;
pub const PCIE_BUSY: u32 = 1 << 6;
pub const INTERRUPTS: u32 = 1 << 7;
pub const RUNNING: u32 = 1 << 8;
pub const USBCMD: u32 = 1 << 9;
pub const USBSTS: u32 = 1 << 10;
pub const PORTS: u32 = 1 << 11;
pub const SLOTS: u32 = 1 << 12;
pub const SCRATCH: u32 = 1 << 13;
pub const RESIDUE: u32 = 1 << 14;
pub const MEMORY: u32 = 1 << 15;
pub const CHANGED: u32 = 1 << 16;
pub const PLAN: u32 = 1 << 17;
pub const UNREADABLE: u32 = 1 << 18;

/// Everything read at one point in time. Constructed by the platform; `rest` of
/// the fields must be equal at every revalidation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Observed {
    pub bar: u64,
    pub id: u32,
    pub class_revision: u32,
    pub command: u16,
    pub status: u16,
    pub msix_control: u16,
    pub msi_control: u16,
    pub pcie: u8,
    pub legacy: bool,
    pub ports_busy: bool,
    pub extended: u8,
    pub header: [u32; 7],
    pub protocol_ports: u32,
    pub usbcmd: u32,
    pub usbsts: u32,
    pub config: u32,
    pub crcr: u64,
    pub dcbaap: u64,
    pub interrupter_enables: u16,
    pub ip_pending: u16,
    pub findings: u16,
    pub reserved: u8,
    pub owned_again: u8,
    pub protected: u8,
    pub hazards: u8,
    pub excluded: u8,
    /// Digest of every memory word and interrupter register the walk read.
    pub memory: u64,
}

impl Observed {
    /// No interrupt can be raised: INTE, EINT and every IP clear, MSI and MSI-X
    /// disabled, INTx not asserted. Revalidated with everything else.
    pub fn interrupts_disabled(&self) -> bool {
        self.usbcmd & 4 == 0
            && self.usbsts & 8 == 0
            && self.ip_pending == 0
            && self.msix_control & 0x8000 == 0
            && self.msi_control & 1 == 0
            && self.status & 8 == 0
    }

    /// Audit value for the trace; equality is decided on the fields, not on this.
    #[inline(never)]
    pub fn fingerprint(&self) -> u32 {
        let mut d = self.memory;
        let words = [self.bar, u64::from(self.id), u64::from(self.class_revision), u64::from(self.command),
                     u64::from(self.status), u64::from(self.usbcmd), u64::from(self.usbsts), u64::from(self.config),
                     self.crcr, self.dcbaap, u64::from(self.header[1]) << 32 | u64::from(self.header[2])];
        for w in words {
            d = fold(d, w);
        }
        (d ^ d >> 32) as u32
    }
}

/// Platform facts that are not register values.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Context {
    pub uefi: bool,
    pub qualified: bool,
    /// Operational register base (CAPLENGTH), for the proposed USBCMD offset.
    pub operational: u32,
}

/// Every reason the controller is not eligible; 0 means eligible.
#[inline(never)]
pub fn refusals(o: &Observed, c: &Context) -> u32 {
    let mut m = 0u32;
    let mut set = |bit: u32, condition: bool| {
        if condition {
            m |= bit;
        }
    };
    set(NOT_UEFI, !c.uefi);
    set(UNQUALIFIED, !c.qualified);
    set(PROFILE, o.id != ID || o.class_revision != CLASS_REVISION || o.header != HEADER || o.extended != 2
        || o.protocol_ports != 0xff || o.legacy);
    set(PCI_COMMAND, o.command != COMMAND);
    set(PCI_STATUS, o.status != STATUS);
    set(PCIE_UNKNOWN, o.pcie == PCIE_ABSENT);
    set(PCIE_BUSY, o.pcie == PCIE_PENDING);
    set(INTERRUPTS, !o.interrupts_disabled());
    set(RUNNING, o.usbcmd & 1 != 0 || o.usbsts & 1 == 0);
    set(USBCMD, o.usbcmd != 0);
    set(USBSTS, o.usbsts != IDLE_USBSTS);
    set(PORTS, o.ports_busy);
    set(SLOTS, o.findings & DEVICES != 0);
    set(SCRATCH, o.findings & SCRATCHPADS != 0 || (o.header[2] >> 21) & 0x1f != 0 || o.header[2] >> 27 != 0);
    set(RESIDUE, o.findings != 0);
    set(MEMORY, o.reserved | o.owned_again | o.protected | o.hazards != 0 || o.excluded == 0);
    m
}

/// The verdict of the lifecycle model for these observations.
fn facts(o: &Observed, c: &Context) -> Facts {
    Facts {
        qualified: c.qualified,
        residue_complete: o.findings == 0,
        halted: o.usbsts & 1 != 0,
        run_stop: o.usbcmd & 1 != 0,
        bus_master: o.command & BUS_MASTER != 0,
        controller_ready: o.usbsts & CNR == 0,
        interrupter_enables: o.interrupter_enables,
        interrupts_disabled: o.interrupts_disabled(),
        legacy_present: o.legacy,
        legacy_os_owned: false,
    }
}

/// The only plan an eligible controller may produce: no mask (interrupts are
/// verified disabled), no halt (it is halted); bus master off, reset, verify.
const EXPECTED: [Step; 5] =
    [Step::AwaitHalted, Step::DisableMaster, Step::Reset, Step::AwaitResetReady, Step::VerifyCleared];

fn plan_matches(f: &Facts) -> bool {
    matches!(lifecycle::plan(f), Ok(p) if p.steps() == EXPECTED && lifecycle::validate(p.steps(), f).is_ok())
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum State {
    Admitted = 1,
    Revalidated0 = 2,
    BusMasterProposed = 3,
    Revalidated1 = 4,
    ResetProposed = 5,
    DryRunComplete = 6,
    Refused = 7,
    // The states below need a real write; the dry-run never reaches them. They
    // define the C3b.2 failure behaviour and are exercised only by hosted tests.
    BusMasterOff = 8,
    BusMasterStuck = 9,
    ContainedBmeOnly = 10,
    ResetStuck = 11,
    ResetUnverified = 12,
    Contained = 13,
    IntegrityFault = 14,
}

pub const K_REVALIDATE: u8 = 1;
pub const K_PCI_WRITE: u8 = 2;
pub const K_MMIO_WRITE: u8 = 3;
pub const K_RESET_POLL: u8 = 4;
pub const K_VERIFY: u8 = 5;
pub const R_OK: u8 = 0;
pub const R_CHANGED: u8 = 1;
pub const R_NOT_ISSUED: u8 = 2;
pub const R_NOT_RUN: u8 = 3;
pub const TRACE_LIMIT: usize = 6;

/// One line of the proposed-action trace.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Trace {
    pub kind: u8,
    pub result: u8,
    pub width: u8,
    pub step: u8,
    pub offset: u32,
    pub old: u32,
    pub new: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rehearsal {
    pub state: State,
    pub refusals: u32,
    pub len: u8,
    pub plan_len: u8,
    pub trace: [Trace; TRACE_LIMIT],
}

impl Rehearsal {
    #[inline(never)]
    fn push(&mut self, kind: u8, result: u8, width: u8, offset: u32, old: u32, new: u32) {
        let step = self.len + 1;
        if let Some(slot) = self.trace.get_mut(usize::from(self.len)) {
            *slot = Trace { kind, result, width, step, offset, old, new };
            self.len += 1;
        }
    }
    fn refuse(mut self, bits: u32) -> Self {
        self.refusals |= bits;
        self.state = State::Refused;
        self
    }
    /// A rehearsal that never started: refused with `bits`.
    pub fn refused(bits: u32) -> Self {
        Rehearsal { state: State::Refused, refusals: bits, len: 0, plan_len: 0, trace: [Trace::default(); TRACE_LIMIT] }
    }
    pub fn entries(&self) -> &[Trace] {
        self.trace.get(..usize::from(self.len)).unwrap_or(&[])
    }
}

/// A fresh observation of everything above, or false if any read failed.
pub trait Source {
    fn observe(&mut self, out: &mut Observed) -> bool;
}

/// C3b.1 issues nothing. C3b.2 would set this together with its adapters; the
/// expectation after the proposed bus-master clear depends on it.
pub const ISSUED: bool = false;

/// What the command register must read after W1 (or, in a dry-run, still).
pub fn expected_command(before: u16, issued: bool) -> u16 {
    if issued { before & !BUS_MASTER } else { before }
}

/// Rehearse the sequence against live observations: eligibility, revalidation
/// before each simulated write, and the auditable trace. `first` receives the
/// initial observation.
pub fn rehearse<S: Source>(s: &mut S, c: &Context, first: &mut Observed) -> Rehearsal {
    let mut r = Rehearsal { state: State::Refused, refusals: 0, len: 0, plan_len: 0, trace: [Trace::default(); TRACE_LIMIT] };
    if !s.observe(first) {
        return r.refuse(UNREADABLE);
    }
    let bits = refusals(first, c);
    if bits != 0 {
        return r.refuse(bits);
    }
    let f = facts(first, c);
    if !plan_matches(&f) {
        return r.refuse(PLAN);
    }
    r.plan_len = EXPECTED.len() as u8;
    r.state = State::Admitted;
    let mut now = Observed::default();
    // Revalidate immediately before W1: identity, configuration and ownership.
    if !s.observe(&mut now) {
        return r.refuse(UNREADABLE);
    }
    if now != *first {
        r.push(K_REVALIDATE, R_CHANGED, 0, 0, first.fingerprint(), now.fingerprint());
        return r.refuse(CHANGED);
    }
    r.push(K_REVALIDATE, R_OK, 0, 0, first.fingerprint(), now.fingerprint());
    r.state = State::Revalidated0;
    let new = first.command & !BUS_MASTER;
    r.push(K_PCI_WRITE, R_NOT_ISSUED, W1_WIDTH, W1_OFFSET, u32::from(first.command), u32::from(new));
    r.state = State::BusMasterProposed;
    // Revalidate immediately before W2. Nothing was written, so the command
    // register must still hold its original value (`expected_command`).
    let expected = expected_command(first.command, ISSUED);
    if !s.observe(&mut now) {
        return r.refuse(UNREADABLE);
    }
    let command_ok = now.command == expected;
    now.command = first.command;
    if !command_ok || now != *first {
        r.push(K_REVALIDATE, R_CHANGED, 0, 0, first.fingerprint(), now.fingerprint());
        return r.refuse(CHANGED);
    }
    r.push(K_REVALIDATE, R_OK, 0, 0, first.fingerprint(), now.fingerprint());
    r.state = State::Revalidated1;
    r.push(K_MMIO_WRITE, R_NOT_ISSUED, W2_WIDTH, c.operational, first.usbcmd, first.usbcmd | HCRST);
    r.state = State::ResetProposed;
    r.push(K_RESET_POLL, R_NOT_RUN, W2_WIDTH, c.operational + 4, POLL_BOUND, CNR);
    r.push(K_VERIFY, R_NOT_RUN, W2_WIDTH, c.operational + 4, IDLE_USBSTS, 0);
    r.state = State::DryRunComplete;
    r
}

/// Observations after a (future) bus-master clear, for the C3b.2 failure model.
pub fn bus_master_outcome(before: &Observed, after: &Observed) -> State {
    let mut rest = *after;
    rest.command = before.command;
    if after.command & BUS_MASTER != 0 {
        State::BusMasterStuck
    } else if after.command != before.command & !BUS_MASTER || rest != *before {
        State::IntegrityFault
    } else {
        State::BusMasterOff
    }
}

/// Observations after a (future) reset: expected completion and timeout rules.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Reset {
    pub polls: u32,
    pub hcrst_clear: bool,
    pub cnr_clear: bool,
    pub usbcmd: u32,
    pub usbsts: u32,
    pub crcr: u64,
    pub dcbaap: u64,
    /// Every interrupter reads ERSTSZ = ERSTBA = ERDP = 0 and IE = 0.
    pub interrupters_clear: bool,
}

/// Completion needs HCRST = 0 and CNR = 0 within `POLL_BOUND` polls; then the
/// DMA-relevant registers must read as reset values. CONFIG is recorded, not gated.
pub fn reset_outcome(r: &Reset) -> State {
    if r.polls > POLL_BOUND || !r.hcrst_clear || !r.cnr_clear {
        return State::ResetStuck;
    }
    let cleared = r.usbcmd == 0 && r.usbsts == IDLE_USBSTS && r.crcr >> 6 == 0 && r.crcr & 8 == 0
        && r.dcbaap == 0 && r.interrupters_clear;
    if cleared { State::Contained } else { State::ResetUnverified }
}

/// The revalidation after W1 failed: bus mastering is already off and is never restored.
pub fn after_revalidation_failure() -> State {
    State::ContainedBmeOnly
}

/// PORTSC value that makes a port unusable for the eligible profile.
pub fn port_busy(portsc: u32) -> bool {
    portsc & PORT_BUSY != 0
}
