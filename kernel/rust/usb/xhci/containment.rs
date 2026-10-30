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
//!
//! USB.2-C3b.2 adds `execute` (below), compiled only with `--cfg vos_contain_test`,
//! which explicitly named QEMU test images pass and production images never do.
//! It performs exactly W1 then W2 through an `Effects` platform, verifies each,
//! and ends in a terminal state; nothing in it re-enables bus mastering.
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
/// USB.2-C3b.2, evaluated only when executing: the xHCI PCI subsystem is not
/// QEMU's default 1af4:1100, or an independent QEMU test-environment check failed.
pub const SUBSYSTEM: u32 = 1 << 19;
pub const ENVIRONMENT: u32 = 1 << 20;
pub const SUBSYSTEM_ID: u32 = 0x1100_1af4;

/// Everything read at one point in time. Constructed by the platform; `rest` of
/// the fields must be equal at every revalidation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Observed {
    pub bar: u64,
    pub id: u32,
    pub class_revision: u32,
    /// PCI subsystem vendor (low half) and device (high half), offset 0x2C.
    pub subsystem: u32,
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
    /// USB.2-C3b.2: the writes will really be issued (test image, execute option).
    pub execute: bool,
    /// Failed QEMU test-environment checks (platform bit mask); 0 when all pass.
    pub environment: u32,
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
    set(SUBSYSTEM, c.execute && o.subsystem != SUBSYSTEM_ID);
    set(ENVIRONMENT, c.execute && c.environment != 0);
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
    // USB.2-C3b.2 terminal states of `execute`. From W1 on, every one is final
    // for the boot and leaves bus mastering off.
    PreW1Drift = 15,
    W1Failed = 16,
    PciChanged = 17,
    ControllerError = 18,
    PreW2Drift = 19,
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
/// initial observation. One copy serves the dry-run and the C3b.2 execution.
#[inline(never)]
pub fn rehearse(s: &mut dyn Source, c: &Context, first: &mut Observed) -> Rehearsal {
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

// ---- USB.2-C3b.2: controlled execution (QEMU test images only) ----

/// One immediate read of the values every write depends on. `ports_busy` is
/// read only when asked (before each write); the reset poll skips the ports.
#[cfg(vos_contain_test)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Quick {
    pub command: u16,
    pub status: u16,
    pub usbcmd: u32,
    pub usbsts: u32,
    pub ports_busy: bool,
}

/// Registers read once after the reset completed. IMOD is not read: its
/// specification default is 4000h and QEMU resets it to 0. PORTSC change bits
/// are ignored: QEMU sets CSC on every port during reset.
#[cfg(vos_contain_test)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AfterReset {
    pub usbcmd: u32,
    pub usbsts: u32,
    pub dnctrl: u32,
    pub config: u32,
    /// Reads 0 on a compliant controller whatever the pointer (xHCI 1.2
    /// 5.4.5); QEMU returns its internal value. Either way it must read 0.
    pub crcr: u64,
    pub dcbaap: u64,
    /// Some port reports CCS, PED, PR or OCA.
    pub ports_active: bool,
    /// Every interrupter reads IMAN = ERSTSZ = ERSTBA = ERDP = 0.
    pub interrupters_clear: bool,
    pub command: u16,
    pub status: u16,
    pub msix_control: u16,
    pub msi_control: u16,
    pub pcie: u8,
    pub bar: u64,
}

/// The hardware the executor drives. Implemented only by the QEMU test-image
/// platform, whose adapters enforce order and once-only again on their side.
/// A write method returns false when its adapter refused and wrote nothing.
#[cfg(vos_contain_test)]
pub trait Effects: Source {
    /// W1: 16-bit PCI command write at 0x04 of `value` (= `old` & !BUS_MASTER).
    fn write_command(&mut self, old: u16, value: u16) -> bool;
    /// W2: 32-bit USBCMD write of `value` (= `old` | HCRST).
    fn write_usbcmd(&mut self, old: u32, value: u32) -> bool;
    fn quick(&mut self, out: &mut Quick, ports: bool) -> bool;
    /// Start a new read budget of `reads` register reads.
    fn phase(&mut self, reads: u32) -> bool;
    fn after_reset(&mut self, out: &mut AfterReset) -> bool;
}

#[cfg(vos_contain_test)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Execution {
    pub rehearsal: Rehearsal,
    pub state: State,
    /// Writes the adapters confirmed issuing (0, 1 or 2).
    pub writes: u8,
    pub polls: u32,
    /// W1 old and new command, and the command and status read back.
    pub w1: [u16; 4],
    pub usbsts_after_w1: u32,
    /// W2 old and new USBCMD.
    pub w2: [u32; 2],
    pub after: AfterReset,
}

/// Every check of the completed reset (xHCI 1.2 4.23.1: operational and
/// runtime registers at their defaults; 5.4.1: PCI configuration unaffected).
#[cfg(vos_contain_test)]
pub fn reset_verified(first: &Observed, a: &AfterReset) -> bool {
    a.usbcmd == 0 && a.usbsts == IDLE_USBSTS && a.dnctrl == 0 && a.config == 0 && a.crcr == 0 && a.dcbaap == 0
        && !a.ports_active && a.interrupters_clear && a.command == first.command & !BUS_MASTER
        && a.status == first.status && a.msix_control == first.msix_control && a.msi_control == first.msi_control
        && a.pcie == PCIE_IDLE && a.bar == first.bar
}

/// The C3b.2 sequence: a complete C3b.1 rehearsal (eligibility and two
/// revalidations), then a fresh pass and immediate re-read, W1, its read-back
/// and a full re-observation, an immediate re-read, W2, a bounded poll and the
/// post-reset verification. Each failure is a distinct terminal state; after
/// W1 there is no path that writes anything except W2, and W2 only after W1
/// was verified.
#[cfg(vos_contain_test)]
pub fn execute(e: &mut dyn Effects, c: &Context, first: &mut Observed) -> Execution {
    let rehearsal = rehearse(e, c, first);
    let mut x = Execution { rehearsal, state: rehearsal.state, writes: 0, polls: 0, w1: [0; 4], usbsts_after_w1: 0,
                            w2: [0; 2], after: AfterReset::default() };
    if rehearsal.state != State::DryRunComplete || !c.execute {
        x.state = State::Refused;
        return x;
    }
    x.state = stages(e, first, &mut x);
    x
}

#[cfg(vos_contain_test)]
#[inline(never)]
fn stages(e: &mut dyn Effects, first: &Observed, x: &mut Execution) -> State {
    let mut now = Observed::default();
    let mut q = Quick::default();
    let idle = Quick { command: first.command, status: first.status, usbcmd: first.usbcmd, usbsts: first.usbsts,
                       ports_busy: false };
    if !e.observe(&mut now) || now != *first || !e.quick(&mut q, true) || q != idle {
        return State::PreW1Drift;
    }
    let off = first.command & !BUS_MASTER;
    x.w1[0] = first.command;
    x.w1[1] = off;
    if !e.write_command(first.command, off) {
        return State::W1Failed;
    }
    x.writes = 1;
    // From here every return is final and leaves bus mastering off. The read-back is lean; the full
    // pass that follows covers the ports.
    if !e.quick(&mut q, false) {
        return State::W1Failed;
    }
    x.w1[2] = q.command;
    x.w1[3] = q.status;
    x.usbsts_after_w1 = q.usbsts;
    if q.command & BUS_MASTER != 0 {
        return State::BusMasterStuck;
    }
    if q.command != off || q.status != first.status {
        return State::PciChanged;
    }
    // HCE (QEMU: a DMA attempt after the clear), HSE, CNR or a running controller.
    if q.usbsts != IDLE_USBSTS || q.usbcmd != first.usbcmd {
        return State::ControllerError;
    }
    if !e.observe(&mut now) || now.command != off {
        return State::ContainedBmeOnly;
    }
    now.command = first.command;
    if now != *first {
        return State::ContainedBmeOnly;
    }
    if !e.quick(&mut q, true) || q != (Quick { command: off, ..idle }) {
        return State::PreW2Drift;
    }
    x.w2 = [q.usbcmd, q.usbcmd | HCRST];
    if !e.write_usbcmd(q.usbcmd, q.usbcmd | HCRST) {
        return State::PreW2Drift;
    }
    x.writes = 2;
    // Reads only while HCRST is 1 (xHCI 1.2 5.4.1). QEMU completes the reset
    // inside the write, so the first poll suffices; the bound is a backstop.
    if !e.phase(2 * POLL_BOUND) {
        return State::ResetStuck;
    }
    loop {
        if x.polls == POLL_BOUND || !e.quick(&mut q, false) {
            return State::ResetStuck;
        }
        x.polls += 1;
        if q.usbcmd & HCRST == 0 && q.usbsts & CNR == 0 {
            break;
        }
    }
    if !e.phase(256) || !e.after_reset(&mut x.after) || !reset_verified(first, &x.after) {
        return State::ResetUnverified;
    }
    State::Contained
}
