//! Hosted support for the C4.1 tests: a model xHC with QEMU 10.2.1 and alternate ERST-fetch
//! behaviour, a reference driver that derives the canonical next operation from the machine's
//! public state, a harness that forwards only accepted write events to the device, a seeded
//! generator for randomized transitions, and the text encoding used by the Python oracle.
//! Nothing here touches PCI configuration, MMIO or physical memory: all state is host data.
#![allow(dead_code)]
use std::collections::{HashMap, HashSet};
use std::sync::Mutex;
use vos_xhci_init::*;

/// Names of every rule and reason observed by any test (checked for completeness at the end).
pub static COVERAGE: Mutex<Option<(HashSet<&'static str>, HashSet<&'static str>)>> = Mutex::new(None);
pub fn record(r: &Result<Accepted, Reason>) {
    let mut g = COVERAGE.lock().unwrap();
    let (rules, reasons) = g.get_or_insert_with(Default::default);
    match r {
        Ok(a) => {
            rules.insert(a.why.name());
        }
        Err(e) => {
            reasons.insert(e.name());
        }
    }
    if let Ok(a) = r {
        let _ = a;
    }
}

// ---- model device ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErstFetch {
    /// QEMU 10.2.1: the ERST is read inside the ERSTBA-high write (hcd-xhci.c, xhci_er_reset).
    AtErstbaWrite,
    /// Permitted alternative the specification does not exclude: the fetch happens at Run.
    AtRun,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Violation {
    DmaWithoutBme,
    WriteWhileCnr,
    CrcrWhileRunning,
    ErdpRepeat,
    ErdpOutsideRing,
    RunBeforeProgrammed,
    DoorbellWhileHalted,
    ResetWhileRunning,
    ResetWithBme,
    BmeClearWhileRunning,
    ErstSizeInvalid,
    EventRingMissing,
}

#[derive(Clone, Debug)]
pub struct Quirks {
    pub erst_fetch: ErstFetch,
    pub reset_polls: u32,
    pub halt_polls: u32,
    pub start_polls: u32,
    pub completion_polls: u32,
    pub legacy: bool,
    pub bios_release_polls: Option<u32>,
    pub firmware_running: bool,
    pub firmware_bme: bool,
    pub stuck_bme: bool,
    pub never_halts: bool,
    pub reset_leaves_pointers: bool,
    pub scratchpads: u16,
    pub dbc: Dbc,
    pub iommu: Tri,
    pub identity: Tri,
    pub no_snoop: Tri,
    pub msi: bool,
    pub connected: u32,
    pub ports: u8,
}

impl Quirks {
    /// QEMU 10.2.1 `-device qemu-xhci`, idle, UEFI: synchronous reset, no legacy capability.
    pub fn qemu() -> Self {
        Self {
            erst_fetch: ErstFetch::AtErstbaWrite,
            reset_polls: 0,
            halt_polls: 0,
            start_polls: 0,
            completion_polls: 0,
            legacy: false,
            bios_release_polls: Some(0),
            firmware_running: false,
            firmware_bme: false,
            stuck_bme: false,
            never_halts: false,
            reset_leaves_pointers: false,
            scratchpads: 0,
            dbc: Dbc::Absent,
            iommu: Tri::No,
            identity: Tri::Yes,
            no_snoop: Tri::No,
            msi: false,
            connected: 0,
            ports: 8,
        }
    }
    /// Slower, hardware-like timing with a legacy-ownership capability; not a QEMU behaviour.
    pub fn slow() -> Self {
        Self { erst_fetch: ErstFetch::AtRun, reset_polls: 3, halt_polls: 2, start_polls: 2, completion_polls: 2,
               legacy: true, bios_release_polls: Some(3), ..Self::qemu() }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct CmdTrb {
    pub kind: u8,
    pub cycle: bool,
    pub toggle: bool,
    pub target: u64,
}

#[derive(Clone, Debug)]
pub struct Device {
    pub q: Quirks,
    pub bme: bool,
    pub rs: bool,
    pub hch: bool,
    pub hse: bool,
    pub hce: bool,
    pub cnr: u32,
    halt_left: Option<u32>,
    start_left: Option<u32>,
    bios_left: Option<u32>,
    pub bios_owned: bool,
    pub os_owned: bool,
    pub smi_off: bool,
    pub config: u32,
    pub dcbaap: u64,
    pub crcr: u64,
    pub crcr_rcs: bool,
    pub erstsz: u32,
    pub erdp: u64,
    pub erstba: u64,
    last_erdp: Option<u64>,
    pub fetched: bool,
    pub er_start: u64,
    pub er_size: u16,
    pub er_enq: u16,
    pub er_pcs: bool,
    pub cmd_deq: u64,
    pub cmd_ccs: bool,
    pub crr: bool,
    pub connected: u32,
    pub cmd_mem: HashMap<u64, CmdTrb>,
    pub ev_mem: HashMap<u64, EventTrb>,
    pub erst_mem: HashMap<u64, (u64, u16)>,
    delayed: Vec<(u32, EventTrb)>,
    pub violations: Vec<Violation>,
    pub writes: Vec<ModelEvent>,
    pub completions_posted: u32,
}

impl Device {
    pub fn new(q: Quirks) -> Self {
        let running = q.firmware_running;
        Self {
            bme: q.firmware_bme,
            rs: running,
            hch: !running,
            hse: false,
            hce: false,
            cnr: 0,
            halt_left: None,
            start_left: None,
            bios_left: None,
            bios_owned: q.legacy,
            os_owned: false,
            smi_off: false,
            config: 0,
            dcbaap: 0,
            crcr: 0,
            crcr_rcs: false,
            erstsz: 0,
            erdp: 0,
            erstba: 0,
            last_erdp: None,
            fetched: false,
            er_start: 0,
            er_size: 0,
            er_enq: 0,
            er_pcs: true,
            cmd_deq: 0,
            cmd_ccs: true,
            crr: false,
            connected: q.connected,
            cmd_mem: HashMap::new(),
            ev_mem: HashMap::new(),
            erst_mem: HashMap::new(),
            delayed: Vec::new(),
            violations: Vec::new(),
            writes: Vec::new(),
            completions_posted: 0,
            q,
        }
    }

    fn violate(&mut self, v: Violation) {
        self.violations.push(v);
    }
    fn controller_error(&mut self) {
        self.hce = true;
        self.rs = false;
        self.hch = true;
        self.crr = false;
    }
    /// A DMA access: with Bus Master Enable clear it is a violation and, as in QEMU, raises HCE.
    fn dma_ok(&mut self) -> bool {
        if !self.bme {
            self.violate(Violation::DmaWithoutBme);
            self.controller_error();
            return false;
        }
        true
    }

    pub fn facts(&self) -> Facts {
        Facts {
            profile: Profile::Qemu1021,
            version: 0x0100,
            page_4k: true,
            slots: 64,
            ports: self.q.ports,
            interrupters: 16,
            scratchpads: self.q.scratchpads,
            legacy: if self.q.legacy { Legacy::Present { bios: self.bios_owned, os: self.os_owned } } else { Legacy::Absent },
            dbc: self.q.dbc,
            iommu: self.q.iommu,
            identity_dma: self.q.identity,
            no_snoop: self.q.no_snoop,
            msi: self.q.msi,
            bme: self.bme,
            running: self.rs,
            halted: self.hch,
            hse: self.hse,
            hce: self.hce,
            connected: self.connected,
            dma_limit: 1 << 30,
        }
    }
    pub fn status(&self) -> Status {
        Status { hch: self.hch, hse: self.hse, hce: self.hce }
    }
    pub fn pci(&self) -> (bool, bool) {
        (self.bme, self.q.msi)
    }
    pub fn handoff(&self) -> (bool, bool) {
        (self.bios_owned, self.os_owned)
    }
    pub fn reset_state(&self) -> ResetState {
        ResetState {
            cnr: self.cnr > 0,
            hch: self.hch,
            usbcmd_zero: !self.rs,
            pointers_zero: self.config == 0 && self.dcbaap == 0 && self.crcr == 0 && self.erstba == 0
                && self.erstsz == 0 && self.erdp == 0,
            bme: self.bme,
            ports_quiet: true,
        }
    }
    /// Software read of the event ring at `index` with consumer cycle `ccs`.
    pub fn read_event(&self, index: u16, ccs: bool) -> Option<EventTrb> {
        if self.er_size == 0 {
            return None;
        }
        let t = self.ev_mem.get(&(self.er_start + 16 * u64::from(index)))?;
        (t.cycle == ccs).then_some(*t)
    }

    /// One poll interval elapses.
    pub fn tick(&mut self) {
        if let Some(n) = self.halt_left {
            if n == 0 {
                self.halt_left = None;
                if !self.q.never_halts {
                    self.hch = true;
                }
            } else {
                self.halt_left = Some(n - 1);
            }
        }
        if let Some(n) = self.start_left {
            if n == 0 {
                self.start_left = None;
                self.hch = false;
            } else {
                self.start_left = Some(n - 1);
            }
        }
        self.cnr = self.cnr.saturating_sub(1);
        if let Some(n) = self.bios_left {
            if n == 0 {
                self.bios_left = None;
                self.bios_owned = false;
            } else {
                self.bios_left = Some(n - 1);
            }
        }
        let mut due = Vec::new();
        self.delayed.retain_mut(|(n, t)| {
            if *n == 0 {
                due.push(*t);
                false
            } else {
                *n -= 1;
                true
            }
        });
        for t in due {
            self.post(t);
        }
    }

    pub fn attach(&mut self, mask: u32) {
        let new = mask & !self.connected;
        self.connected |= mask;
        if self.rs && !self.hch && self.fetched {
            for p in 0..self.q.ports {
                if new & (1 << p) != 0 {
                    self.post(EventTrb { index: 0, cycle: false, kind: 34, pointer: 0, code: 1, param: 0, slot: 0, vf: 0, port: p + 1 });
                }
            }
        }
    }

    fn post(&mut self, mut t: EventTrb) {
        if self.er_size == 0 {
            self.violate(Violation::EventRingMissing);
            return;
        }
        if !self.dma_ok() {
            return;
        }
        if self.erdp < self.er_start || self.erdp >= self.er_start + 16 * u64::from(self.er_size) {
            self.violate(Violation::ErdpOutsideRing);
            self.controller_error();
            return;
        }
        t.index = self.er_enq;
        t.cycle = self.er_pcs;
        self.ev_mem.insert(self.er_start + 16 * u64::from(self.er_enq), t);
        if self.er_enq == self.er_size - 1 {
            self.er_enq = 0;
            self.er_pcs = !self.er_pcs;
        } else {
            self.er_enq += 1;
        }
    }

    fn fetch_erst(&mut self) {
        if !self.dma_ok() {
            return;
        }
        if self.erstsz != 1 {
            self.violate(Violation::ErstSizeInvalid);
            self.controller_error();
            return;
        }
        match self.erst_mem.get(&self.erstba) {
            Some(&(seg, size)) if size >= 16 => {
                self.er_start = seg;
                self.er_size = size;
                self.er_enq = 0;
                self.er_pcs = true;
                self.fetched = true;
            }
            _ => self.controller_error(),
        }
    }

    fn guard_cnr(&mut self) {
        if self.cnr > 0 {
            self.violate(Violation::WriteWhileCnr);
        }
    }

    /// Perform one accepted register write.
    pub fn write(&mut self, e: ModelEvent) {
        self.writes.push(e);
        match e.target {
            Target::LegacyOsOwned => {
                self.os_owned = true;
                self.bios_left = self.q.bios_release_polls;
            }
            Target::LegacySmi => self.smi_off = true,
            Target::RunStop => {
                if e.value == 1 {
                    self.guard_cnr();
                    if self.q.erst_fetch == ErstFetch::AtRun && !self.fetched && self.erstba != 0 {
                        self.fetch_erst();
                    }
                    if self.hce {
                        return;
                    }
                    if !(self.fetched && self.crcr != 0 && self.dcbaap != 0 && self.config != 0) {
                        self.violate(Violation::RunBeforeProgrammed);
                    }
                    self.rs = true;
                    if self.q.start_polls == 0 {
                        self.hch = false;
                    } else {
                        self.start_left = Some(self.q.start_polls - 1);
                    }
                } else {
                    self.rs = false;
                    self.crr = false;
                    self.start_left = None;
                    if self.q.halt_polls == 0 {
                        if !self.q.never_halts {
                            self.hch = true;
                        }
                    } else {
                        self.halt_left = Some(self.q.halt_polls - 1);
                    }
                }
            }
            Target::Hcrst => {
                if self.rs && !self.hch {
                    self.violate(Violation::ResetWhileRunning);
                }
                if self.bme {
                    self.violate(Violation::ResetWithBme);
                }
                self.config = 0;
                self.crcr = 0;
                self.crcr_rcs = false;
                self.erstsz = 0;
                self.erdp = 0;
                self.erstba = 0;
                if !self.q.reset_leaves_pointers {
                    self.dcbaap = 0;
                }
                self.fetched = false;
                self.er_size = 0;
                self.last_erdp = None;
                self.rs = false;
                self.hch = true;
                self.crr = false;
                self.cnr = self.q.reset_polls;
                self.delayed.clear();
            }
            Target::PciBme => {
                if e.value == 1 {
                    self.bme = true;
                } else {
                    if self.rs && !self.hch {
                        self.violate(Violation::BmeClearWhileRunning);
                    }
                    if !self.q.stuck_bme {
                        self.bme = false;
                    }
                }
            }
            Target::Config => {
                self.guard_cnr();
                self.config = e.value as u32;
            }
            Target::Dcbaap => {
                self.guard_cnr();
                self.dcbaap = e.value;
            }
            Target::Crcr => {
                self.guard_cnr();
                if self.crr {
                    self.violate(Violation::CrcrWhileRunning);
                }
                self.crcr = e.value & !0x3f;
                self.crcr_rcs = e.value & 1 != 0;
                self.cmd_deq = self.crcr;
                self.cmd_ccs = self.crcr_rcs;
            }
            Target::Erstsz => {
                self.guard_cnr();
                self.erstsz = e.value as u32;
            }
            Target::Erdp => {
                self.guard_cnr();
                let addr = e.value & !0xf;
                if self.last_erdp == Some(addr) {
                    self.violate(Violation::ErdpRepeat);
                }
                self.last_erdp = Some(addr);
                self.erdp = addr;
                if self.fetched && (addr < self.er_start || addr >= self.er_start + 16 * u64::from(self.er_size)) {
                    self.violate(Violation::ErdpOutsideRing);
                }
            }
            Target::Erstba => {
                self.guard_cnr();
                self.erstba = e.value;
                if self.q.erst_fetch == ErstFetch::AtErstbaWrite {
                    self.fetch_erst();
                }
            }
            Target::Doorbell0 => self.doorbell(),
        }
    }

    fn doorbell(&mut self) {
        if !self.rs || self.hch {
            self.violate(Violation::DoorbellWhileHalted);
            return;
        }
        self.crr = true;
        for _ in 0..4 {
            let Some(&t) = self.cmd_mem.get(&self.cmd_deq) else { return };
            if t.cycle != self.cmd_ccs {
                return;
            }
            match t.kind {
                23 => {
                    let ev = EventTrb { index: 0, cycle: false, kind: 33, pointer: self.cmd_deq, code: 1, param: 0, slot: 0, vf: 0, port: 0 };
                    self.cmd_deq += 16;
                    self.completions_posted += 1;
                    if self.q.completion_polls == 0 {
                        self.post(ev);
                    } else {
                        self.delayed.push((self.q.completion_polls - 1, ev));
                    }
                    return;
                }
                6 => {
                    if t.toggle {
                        self.cmd_ccs = !self.cmd_ccs;
                    }
                    self.cmd_deq = t.target;
                }
                _ => return,
            }
        }
    }
}

// ---- harness: machine + device ----------------------------------------------------------------

pub const CMD: u64 = 0x40_0000;
pub const EVT: u64 = 0x40_1000;
pub const DCB: u64 = 0x40_2000;
pub const ERS: u64 = 0x40_3000;

pub fn page_addr(k: PageKind) -> u64 {
    match k {
        PageKind::Command => CMD,
        PageKind::Event => EVT,
        PageKind::Dcbaa => DCB,
        PageKind::Erst => ERS,
    }
}

pub struct Harness {
    pub m: Machine,
    pub d: Device,
    pub now: u64,
    /// No-Op target used by `goto` and `next`.
    pub target: u32,
    pub log: Vec<(Operation, Result<Accepted, Reason>)>,
    /// Terminal cause after each logged step (`-` while none).
    pub causes: Vec<&'static str>,
}

impl Harness {
    pub fn new(q: Quirks, policy: ReleasePolicy) -> Self {
        Self { m: Machine::new(policy), d: Device::new(q), now: 0, target: u32::MAX, log: Vec::new(), causes: Vec::new() }
    }

    /// Apply `op` to the machine; on acceptance perform its host memory effect and forward its
    /// write event to the device. A refused operation never reaches the device.
    pub fn apply(&mut self, op: Operation) -> Result<Accepted, Reason> {
        let before = self.m.clone();
        let r = self.m.apply(op);
        if r.is_err() {
            assert_eq!(before, self.m, "a refused operation changed the state: {op:?}");
        }
        if let Ok(a) = &r {
            match op {
                Operation::PrepareErst { segment, size } => {
                    self.d.erst_mem.insert(ERS, (segment, size));
                }
                Operation::PublishNoop { index, cycle } => {
                    self.d.cmd_mem.insert(CMD + 16 * u64::from(index), CmdTrb { kind: 23, cycle, toggle: false, target: 0 });
                }
                Operation::PublishLink { index, cycle, toggle, target } => {
                    self.d.cmd_mem.insert(CMD + 16 * u64::from(index), CmdTrb { kind: 6, cycle, toggle, target });
                }
                Operation::Tick(t) => {
                    self.now = t;
                    self.d.tick();
                }
                _ => {}
            }
            if let Some(e) = a.event {
                self.d.write(e);
            }
        }
        record(&r);
        if let Some(t) = self.m.terminal_reason() {
            COVERAGE.lock().unwrap().get_or_insert_with(Default::default).1.insert(t.name());
        }
        self.causes.push(self.m.terminal_reason().map_or("-", |c| c.name()));
        self.log.push((op, r));
        r
    }

    /// The canonical next operation for the current state, or `None` when the session is over.
    pub fn next(&self, noops: u32) -> Option<Operation> {
        use Operation as O;
        let m = &self.m;
        let tick = O::Tick(self.now + 1);
        let waiting = |op: Operation| -> Option<Operation> {
            // alternate time and observation so delayed devices make progress
            if self.log.last().is_some_and(|(o, _)| matches!(o, O::Tick(_))) { Some(op) } else { Some(tick) }
        };
        let pages = [PageKind::Command, PageKind::Event, PageKind::Dcbaa, PageKind::Erst];
        Some(match m.phase() {
            Phase::Idle => O::Qualify(self.d.facts()),
            Phase::Qualified => {
                if matches!(self.d.facts().legacy, Legacy::Present { os: true, bios: false }) { O::DisableLegacySmi } else { O::RequestOsOwnership }
            }
            Phase::Handoff => return waiting(O::ObserveHandoff { bios: self.d.handoff().0, os: self.d.handoff().1 }),
            Phase::HandedOff => O::DisableLegacySmi,
            Phase::Owned => {
                if m.run_stop() { O::ClearRunStop } else { O::ObserveStatus(self.d.status()) }
            }
            Phase::Halting => return waiting(O::ObserveStatus(self.d.status())),
            Phase::Halted => {
                if !m.bus_master_verified() {
                    let (bme, msi) = self.d.pci();
                    O::ObservePci { bme, msi }
                } else if m.bus_master() {
                    O::ClearBusMaster
                } else {
                    O::AssertReset
                }
            }
            Phase::Resetting => return waiting(O::ObserveReset(self.d.reset_state())),
            Phase::Contained | Phase::Programming => {
                for k in pages {
                    match m.page(k).state {
                        PageState::Free => return Some(O::DmaAllocate { kind: k, addr: page_addr(k) }),
                        PageState::Allocated => return Some(O::DmaZero(k)),
                        _ => {}
                    }
                }
                for k in pages {
                    if m.page(k).state == PageState::Zeroed {
                        return Some(match k {
                            PageKind::Command => O::PrepareCommandRing,
                            PageKind::Event => O::PrepareEventRing,
                            PageKind::Dcbaa => O::PrepareDcbaa { entry0: 0 },
                            PageKind::Erst => O::PrepareErst { segment: EVT, size: 256 },
                        });
                    }
                }
                let w = m.statics_written();
                if w & 1 == 0 {
                    O::SetMaxSlots(1)
                } else if w & 2 == 0 {
                    O::SetDcbaap(DCB)
                } else if w & 4 == 0 {
                    O::SetCommandRing { addr: CMD, rcs: true }
                } else if w & 8 == 0 {
                    O::SetErstSize(1)
                } else {
                    O::SetEventDequeue { addr: EVT, ehb: false }
                }
            }
            Phase::Programmed => O::SetBusMaster,
            Phase::BusMastering => {
                if !m.bus_master_verified() {
                    let (bme, msi) = self.d.pci();
                    O::ObservePci { bme, msi }
                } else {
                    O::SetErstBase(ERS)
                }
            }
            Phase::EventRingArmed => {
                if !m.status_clean() { O::ObserveStatus(self.d.status()) } else { O::SetRunStop }
            }
            Phase::Starting => return waiting(O::ObserveStatus(self.d.status())),
            Phase::Running => {
                if m.poll_events() >= POLL_BUDGET {
                    O::BeginPollStep
                } else if m.pending_command() {
                    let (idx, ccs) = m.event_cursor();
                    match self.d.read_event(idx, ccs) {
                        Some(t) => O::ConsumeEvent(Some(t)),
                        None => return waiting(O::ConsumeEvent(None)),
                    }
                } else if m.unacked() > 0 {
                    O::SetEventDequeue { addr: m.expected_erdp(), ehb: true }
                } else if m.noops_completed() >= noops {
                    O::ClearRunStop
                } else if let Some(i) = m.staged_command() {
                    let _ = i;
                    if m.link_required() {
                        let (enq, pcs) = m.command_cursor();
                        O::PublishLink { index: enq, cycle: pcs, toggle: true, target: CMD }
                    } else {
                        O::RingCommandDoorbell
                    }
                } else {
                    let (enq, pcs) = m.command_cursor();
                    O::PublishNoop { index: enq, cycle: pcs }
                }
            }
            Phase::Quiesced | Phase::Failed => {
                for k in pages {
                    if m.page(k).state != PageState::Free {
                        return Some(O::DmaRelease(k));
                    }
                }
                return None;
            }
            Phase::Quarantined => match m.tail() {
                "need-halt" => O::ClearRunStop,
                "await-halt" => return waiting(O::ObserveStatus(self.d.status())),
                "need-bme-clear" => O::ClearBusMaster,
                "await-verify" => {
                    let (bme, msi) = self.d.pci();
                    O::ObservePci { bme, msi }
                }
                _ => return None,
            },
            Phase::Refused => return None,
        })
    }

    /// Run the canonical session to its end (at most `limit` operations). Returns the count.
    pub fn run(&mut self, noops: u32, limit: usize) -> usize {
        let mut n = 0;
        while n < limit {
            let Some(op) = self.next(noops) else { break };
            let _ = self.apply(op);
            n += 1;
        }
        n
    }
}

// ---- seeded generator ---------------------------------------------------------------------------

pub struct Rng(pub u64);
impl Rng {
    pub fn next(&mut self) -> u64 {
        // xorshift64*
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
    pub fn chance(&mut self, num: u64, den: u64) -> bool {
        self.below(den) < num
    }
    pub fn pick<T: Copy>(&mut self, xs: &[T]) -> T {
        xs[self.below(xs.len() as u64) as usize]
    }
}

pub fn random_quirks(r: &mut Rng) -> Quirks {
    let mut q = if r.chance(1, 2) { Quirks::qemu() } else { Quirks::slow() };
    if r.chance(1, 2) {
        q.erst_fetch = if r.chance(1, 2) { ErstFetch::AtErstbaWrite } else { ErstFetch::AtRun };
    }
    q.legacy = r.chance(1, 3);
    q.bios_release_polls = if r.chance(1, 8) { None } else { Some(r.below(4) as u32) };
    q.firmware_running = r.chance(1, 4);
    q.firmware_bme = r.chance(1, 4);
    q.stuck_bme = r.chance(1, 12);
    q.never_halts = r.chance(1, 12);
    q.reset_leaves_pointers = r.chance(1, 12);
    q.reset_polls = r.below(4) as u32;
    q.halt_polls = r.below(3) as u32;
    q.start_polls = r.below(3) as u32;
    q.completion_polls = r.below(3) as u32;
    q.scratchpads = if r.chance(1, 14) { 4 } else { 0 };
    q.dbc = if r.chance(1, 14) { r.pick(&[Dbc::Enabled, Dbc::Unknown, Dbc::Disabled]) } else { Dbc::Absent };
    q.iommu = if r.chance(1, 14) { r.pick(&[Tri::Yes, Tri::Unknown]) } else { Tri::No };
    q.identity = if r.chance(1, 14) { r.pick(&[Tri::No, Tri::Unknown]) } else { Tri::Yes };
    q.no_snoop = if r.chance(1, 14) { r.pick(&[Tri::Yes, Tri::Unknown]) } else { Tri::No };
    q.msi = r.chance(1, 14);
    q.connected = if r.chance(1, 10) { 1 << r.below(8) } else { 0 };
    q
}

fn addr(r: &mut Rng) -> u64 {
    r.pick(&[CMD, EVT, DCB, ERS, CMD, EVT, DCB, ERS, 0, 0x10_0001, 0x40_0800, 0x3fff_f000, 0x4000_0000, 0xfff0_0000, CMD + 16, EVT + 16 * 3])
}
fn kind(r: &mut Rng) -> PageKind {
    r.pick(&[PageKind::Command, PageKind::Event, PageKind::Dcbaa, PageKind::Erst])
}

/// A random operation, biased towards values that are legal for the current state.
pub fn random_op(r: &mut Rng, h: &Harness) -> Operation {
    use Operation as O;
    let d = &h.d;
    let (bme, msi) = d.pci();
    let (idx, ccs) = h.m.event_cursor();
    let (enq, pcs) = h.m.command_cursor();
    match r.below(40) {
        0 => O::Qualify(d.facts()),
        1 => O::Tick(h.now + if r.chance(1, 12) { 100 + r.below(200) } else { r.below(40) }),
        2 => O::Tick(h.now.saturating_sub(r.below(3))),
        3 => O::ObserveHandoff { bios: r.chance(1, 2), os: r.chance(2, 3) },
        4 => O::ObserveStatus(d.status()),
        5 => O::ObserveStatus(Status { hch: r.chance(1, 2), hse: r.chance(1, 20), hce: r.chance(1, 20) }),
        6 => O::ObserveReset(d.reset_state()),
        7 => O::ObserveReset(ResetState { cnr: r.chance(1, 4), hch: r.chance(5, 6), usbcmd_zero: r.chance(5, 6), pointers_zero: r.chance(5, 6), bme: r.chance(1, 8), ports_quiet: r.chance(5, 6) }),
        8 => O::ObservePci { bme, msi },
        9 => O::ObservePci { bme: r.chance(1, 2), msi: r.chance(1, 16) },
        10 => O::ObservePorts { connected: if r.chance(1, 2) { d.connected } else { r.below(300) as u32 } },
        11 => O::ConsumeEvent(d.read_event(idx, ccs)),
        12 => O::ConsumeEvent(None),
        13 => O::ConsumeEvent(Some(EventTrb {
            index: if r.chance(2, 3) { idx } else { r.below(300) as u16 },
            cycle: if r.chance(2, 3) { ccs } else { r.chance(1, 2) },
            kind: r.pick(&[33, 33, 34, 34, 32, 37]),
            pointer: if r.chance(1, 2) { CMD + 16 * u64::from(enq.wrapping_sub(1) % 255) } else { r.below(1 << 24) },
            code: r.pick(&[1, 1, 1, 4, 5]),
            param: if r.chance(9, 10) { 0 } else { 7 },
            slot: if r.chance(9, 10) { 0 } else { 1 },
            vf: 0,
            port: r.pick(&[0, 1, 2, 8, 9, 40]),
        })),
        14 => O::DmaAllocate { kind: kind(r), addr: addr(r) },
        15 => { let k = kind(r); O::DmaAllocate { kind: k, addr: page_addr(k) } }
        16 => O::DmaZero(kind(r)),
        17 => O::PrepareCommandRing,
        18 => O::PrepareEventRing,
        19 => O::PrepareDcbaa { entry0: if r.chance(9, 10) { 0 } else { 0x5000_0000 } },
        20 => O::PrepareErst { segment: if r.chance(5, 6) { EVT } else { addr(r) }, size: r.pick(&[256, 256, 256, 8, 16, 512, 0]) },
        21 => O::DmaRelease(kind(r)),
        22 => O::RequestOsOwnership,
        23 => O::DisableLegacySmi,
        24 => O::ClearRunStop,
        25 => O::ClearBusMaster,
        26 => O::AssertReset,
        27 => O::SetMaxSlots(r.pick(&[1, 1, 1, 0, 2, 64])),
        28 => O::SetDcbaap(addr(r)),
        29 => O::SetCommandRing { addr: addr(r), rcs: r.chance(5, 6) },
        30 => O::SetErstSize(r.pick(&[1, 1, 1, 0, 2])),
        31 => O::SetEventDequeue { addr: if r.chance(1, 2) { h.m.expected_erdp() } else { addr(r) }, ehb: r.chance(2, 3) },
        32 => O::SetBusMaster,
        33 => O::SetErstBase(addr(r)),
        34 => O::SetRunStop,
        35 => O::PublishNoop { index: if r.chance(3, 4) { enq } else { r.below(300) as u16 }, cycle: if r.chance(3, 4) { pcs } else { !pcs } },
        36 => O::PublishLink { index: if r.chance(3, 4) { enq } else { 255 }, cycle: if r.chance(3, 4) { pcs } else { !pcs }, toggle: r.chance(5, 6), target: if r.chance(5, 6) { CMD } else { addr(r) } },
        37 => O::RingCommandDoorbell,
        38 => O::BeginPollStep,
        _ => if r.chance(1, 3) { O::Cancel } else if r.chance(1, 40) { O::BackendFault } else { O::Forbidden(r.pick(&[Forbidden::InterruptEnable, Forbidden::PortWrite, Forbidden::SlotDoorbell, Forbidden::Command])) },
    }
}

// ---- text encoding for the independent oracle ---------------------------------------------------------

fn b(x: bool) -> u8 {
    x as u8
}
fn idx<T: PartialEq + Copy>(all: &[T], x: T) -> usize {
    all.iter().position(|y| *y == x).unwrap()
}

pub fn encode(op: &Operation) -> String {
    use Operation as O;
    match op {
        O::Qualify(f) => {
            let (lp, lb, lo) = match f.legacy { Legacy::Absent => (0, 0, 0), Legacy::Present { bios, os } => (1, b(bios), b(os)) };
            format!("Qualify {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}", idx(Profile::ALL, f.profile), f.version, b(f.page_4k),
                f.slots, f.ports, f.interrupters, f.scratchpads, lp, lb, lo, idx(Dbc::ALL, f.dbc), idx(Tri::ALL, f.iommu),
                idx(Tri::ALL, f.identity_dma), idx(Tri::ALL, f.no_snoop), b(f.msi), b(f.bme), b(f.running), b(f.halted), b(f.hse), b(f.hce),
                f.connected, f.dma_limit, 0)
        }
        O::Tick(t) => format!("Tick {t}"),
        O::ObserveHandoff { bios, os } => format!("ObserveHandoff {} {}", b(*bios), b(*os)),
        O::ObserveStatus(s) => format!("ObserveStatus {} {} {}", b(s.hch), b(s.hse), b(s.hce)),
        O::ObserveReset(r) => format!("ObserveReset {} {} {} {} {} {}", b(r.cnr), b(r.hch), b(r.usbcmd_zero), b(r.pointers_zero), b(r.bme), b(r.ports_quiet)),
        O::ObservePci { bme, msi } => format!("ObservePci {} {}", b(*bme), b(*msi)),
        O::ObservePorts { connected } => format!("ObservePorts {connected}"),
        O::ConsumeEvent(None) => "ConsumeEvent none".into(),
        O::ConsumeEvent(Some(t)) => format!("ConsumeEvent {} {} {} {} {} {} {} {} {}", t.index, b(t.cycle), t.kind, t.pointer, t.code, t.param, t.slot, t.vf, t.port),
        O::DmaAllocate { kind, addr } => format!("DmaAllocate {} {addr}", kind.name()),
        O::DmaZero(k) => format!("DmaZero {}", k.name()),
        O::PrepareCommandRing => "PrepareCommandRing".into(),
        O::PrepareEventRing => "PrepareEventRing".into(),
        O::PrepareDcbaa { entry0 } => format!("PrepareDcbaa {entry0}"),
        O::PrepareErst { segment, size } => format!("PrepareErst {segment} {size}"),
        O::DmaRelease(k) => format!("DmaRelease {}", k.name()),
        O::RequestOsOwnership => "RequestOsOwnership".into(),
        O::DisableLegacySmi => "DisableLegacySmi".into(),
        O::ClearRunStop => "ClearRunStop".into(),
        O::ClearBusMaster => "ClearBusMaster".into(),
        O::AssertReset => "AssertReset".into(),
        O::SetMaxSlots(n) => format!("SetMaxSlots {n}"),
        O::SetDcbaap(a) => format!("SetDcbaap {a}"),
        O::SetCommandRing { addr, rcs } => format!("SetCommandRing {addr} {}", b(*rcs)),
        O::SetErstSize(n) => format!("SetErstSize {n}"),
        O::SetEventDequeue { addr, ehb } => format!("SetEventDequeue {addr} {}", b(*ehb)),
        O::SetBusMaster => "SetBusMaster".into(),
        O::SetErstBase(a) => format!("SetErstBase {a}"),
        O::SetRunStop => "SetRunStop".into(),
        O::PublishNoop { index, cycle } => format!("PublishNoop {index} {}", b(*cycle)),
        O::PublishLink { index, cycle, toggle, target } => format!("PublishLink {index} {} {} {target}", b(*cycle), b(*toggle)),
        O::RingCommandDoorbell => "RingCommandDoorbell".into(),
        O::BeginPollStep => "BeginPollStep".into(),
        O::Cancel => "Cancel".into(),
        O::BackendFault => "BackendFault".into(),
        O::Forbidden(f) => format!("Forbidden {}", f.name()),
    }
}

pub fn outcome(r: &Result<Accepted, Reason>) -> String {
    match r {
        Ok(a) => {
            let ev = a.event.map_or("-".to_string(), |e| format!("{}:{}:{}:{}", e.id, e.target.name(), e.width, e.value));
            format!("ok {}>{} {} {}", a.from.name(), a.to.name(), a.why.name(), ev)
        }
        Err(e) => format!("err {}", e.name()),
    }
}

/// Replace every observation in `op` by what the device would truthfully report. Observations are
/// the machine's only link to reality, so device-safety properties are only meaningful for true ones.
pub fn truthify(op: Operation, h: &Harness) -> Operation {
    use Operation as O;
    let d = &h.d;
    let (bme, msi) = d.pci();
    let (idx, ccs) = h.m.event_cursor();
    match op {
        O::ObserveHandoff { .. } if d.q.legacy => O::ObserveHandoff { bios: d.handoff().0, os: d.handoff().1 },
        O::ObserveStatus(_) => O::ObserveStatus(d.status()),
        O::ObserveReset(_) => O::ObserveReset(d.reset_state()),
        O::ObservePci { .. } => O::ObservePci { bme, msi },
        O::ObservePorts { .. } => O::ObservePorts { connected: d.connected },
        O::ConsumeEvent(Some(_)) => O::ConsumeEvent(d.read_event(idx, ccs)),
        other => other,
    }
}
