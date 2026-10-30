//! Hosted support for the C4.2a tests: a register-level fake `Backend` over the C4.1 `Device` model with
//! sequence, fault and asynchronous-change injection and an interaction recorder, a session harness with a
//! canonical reference driver, and the text encoding replayed by the independent oracle.
//! Nothing here touches PCI, MMIO or physical memory: all state is host data.
#![allow(dead_code)]
#[path = "xhci_init_support.rs"]
pub mod base;
use base::*;
use std::cell::RefCell;
use vos_xhci_init::*;

/// One injected misbehaviour, applied when the backend call with a given index is served.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Inj {
    // response corruption
    Fault(ReadFault),
    WrongReg,
    WrongIndex,
    BadWidth,
    Lie(u64),
    SeqDup,
    SeqBack,
    SeqSkip,
    ForeignSession,
    TickBack,
    // asynchronous hardware change, applied just before the call is served
    Halt(bool),
    Bme(bool),
    Rs(bool),
    Bios(bool),
    Attach(u32),
    Vendor(u16),
    Bar(u64),
    Hcs2(u16),
    SecondXhci,
    ScanIncomplete,
    Cnr(u32),
    ReservedSts,
    HceSet,
    // write path
    WriteRefused,
    WriteBadTicket,
    WriteFault,
    WriteUnverifiedDone,
    WriteUnverifiedNotDone,
    ReadbackMissing,
    ReadbackWrong,
    ReadbackWrongRegister,
    Msi(bool),
}

#[derive(Clone)]
pub struct Fake {
    pub d: Device,
    pub q: Quirks,
    pub seq: u64,
    pub now: u64,
    pub session: u32,
    pub calls: u64,
    pub sched: Vec<(u64, Inj)>,
    pub log: RefCell<Vec<String>>,
    pub expect_nonce: u64,
    pub terminal: bool,
    pub tail_used: u8,
    pub vendor: u16,
    pub bar: u64,
    pub hcs2_scratch: u16,
    pub xhci_functions: u8,
    pub scan_complete: bool,
    pub performed: Vec<ModelEvent>,
    pub refused_writes: u32,
    /// A CRCR that keeps reporting this pointer (QEMU reads the pointer; xHCI 1.2 section 5.4.5 says zero).
    pub sticky_crcr: Option<u64>,
    /// Per-write injections keyed by inventory id (consumed once).
    pub on_write: Vec<(&'static str, Inj)>,
    /// Device changes applied just before the store of the named write (a check-then-act race).
    pub write_race: Vec<(&'static str, fn(&mut Device))>,
}

impl Fake {
    pub fn new(q: Quirks) -> Self {
        let s = q.scratchpads;
        Self {
            d: Device::new(q.clone()),
            q,
            seq: 100,
            now: 1,
            session: 7,
            calls: 0,
            sched: Vec::new(),
            log: RefCell::new(Vec::new()),
            expect_nonce: 1,
            terminal: false,
            tail_used: 0,
            vendor: 0x1b36,
            bar: 0xc0_0000_0000,
            hcs2_scratch: s,
            xhci_functions: 1,
            scan_complete: true,
            performed: Vec::new(),
            refused_writes: 0,
            sticky_crcr: None,
            on_write: Vec::new(),
            write_race: Vec::new(),
        }
    }
    pub fn at(&mut self, call: u64, i: Inj) {
        self.sched.push((call, i));
    }
    /// Time passes: the tick counter and the device's latencies advance.
    pub fn advance(&mut self) {
        self.now += 1;
        self.d.tick();
    }
    fn note(&self, s: String) {
        self.log.borrow_mut().push(s);
    }

    /// Begin one stamped backend call: apply asynchronous changes, pick response-altering injections.
    fn begin(&mut self) -> (Stamp, u32, Vec<Inj>) {
        self.calls += 1;
        let mut alter = Vec::new();
        let due: Vec<Inj> = self.sched.iter().filter(|(c, _)| *c == self.calls).map(|(_, i)| *i).collect();
        for i in due {
            match i {
                Inj::Halt(h) => self.d.hch = h,
                Inj::Bme(b) => self.d.bme = b,
                Inj::Rs(r) => self.d.rs = r,
                Inj::Bios(b) => self.d.bios_owned = b,
                Inj::Attach(m) => self.d.connected |= m,
                Inj::Vendor(v) => self.vendor = v,
                Inj::Bar(b) => self.bar = b,
                Inj::Hcs2(s) => self.hcs2_scratch = s,
                Inj::SecondXhci => self.xhci_functions = 2,
                Inj::ScanIncomplete => self.scan_complete = false,
                Inj::Cnr(n) => self.d.cnr = n,
                Inj::Msi(b) => self.q.msi = b,
                other => alter.push(other),
            }
        }
        let mut seq = self.seq + 1;
        let mut tick = self.now;
        let mut session = self.session;
        for a in &alter {
            match a {
                Inj::SeqDup => seq = self.seq,
                Inj::SeqBack => seq = self.seq.saturating_sub(1),
                Inj::SeqSkip => seq = self.seq + 2,
                Inj::ForeignSession => session += 1,
                Inj::TickBack => tick = self.now.saturating_sub(1),
                _ => {}
            }
        }
        self.seq = if alter.contains(&Inj::SeqBack) || alter.contains(&Inj::SeqDup) { self.seq } else { seq };
        (Stamp { seq, tick }, session, alter)
    }

    /// Register value as the model device would present it (None = capability not present).
    pub fn reg_value(&self, reg: Reg, index: u8) -> Result<Option<u64>, ReadFault> {
        let d = &self.d;
        Ok(Some(match reg {
            Reg::PciCommand => 0x0002 | (u64::from(d.bme) << 2),
            Reg::PciMsiControl => u64::from(self.q.msi),
            Reg::PciMsixControl => 0,
            Reg::PcieDevCtl => match self.q.no_snoop {
                Tri::Unknown => return Ok(None),
                Tri::Yes => 1 << 11,
                Tri::No => 0,
            },
            Reg::Hciversion => 0x0100,
            Reg::Hcsparams1 => 64 | (16 << 8) | (u64::from(self.q.ports) << 24),
            Reg::Hcsparams2 => {
                let s = u64::from(self.hcs2_scratch);
                0xf | ((s >> 5) << 21) | ((s & 31) << 27)
            }
            Reg::Hccparams1 => 0x0000_0001,
            Reg::Pagesize => 1,
            Reg::Usbcmd => u64::from(d.rs),
            Reg::Usbsts => u64::from(d.hch) | (u64::from(d.hse) << 2) | (u64::from(d.cnr > 0) << 11) | (u64::from(d.hce) << 12),
            Reg::Config => u64::from(d.config),
            Reg::Crcr => self.sticky_crcr.unwrap_or(d.crcr),
            Reg::Dcbaap => d.dcbaap,
            Reg::Erstsz => u64::from(d.erstsz),
            Reg::Erstba => d.erstba,
            Reg::Erdp => d.erdp,
            Reg::Usblegsup => {
                if !self.q.legacy {
                    return Ok(None);
                }
                (u64::from(d.bios_owned) << 16) | (u64::from(d.os_owned) << 24)
            }
            Reg::DbcCtrl => match self.q.dbc {
                Dbc::Absent => return Ok(None),
                Dbc::Disabled => 0,
                Dbc::Enabled => 1 << 31,
                Dbc::Unknown => return Err(ReadFault::Other),
            },
            Reg::Portsc => u64::from((d.connected >> index) & 1),
        }))
    }

    pub fn identity(&self) -> Identity {
        Identity {
            bdf: 0x10,
            vendor: self.vendor,
            device: 0x000d,
            revision: 1,
            class: 0x0c0330,
            subsystem: 0x1100_1af4,
            bar0: self.bar,
            hciversion: 0x0100,
            hcs1: 64 | (16 << 8) | (u32::from(self.q.ports) << 24),
            hcs2: u32::from(self.hcs2_scratch),
            hcs3: 0,
            hcc1: 1,
            dboff: 0x2000,
            rtsoff: 0x1000,
        }
    }
}

fn ident_text(i: &Identity) -> String {
    format!("{} {} {} {} {} {} {} {} {} {} {} {} {} {}", i.bdf, i.vendor, i.device, i.revision, i.class, i.subsystem, i.bar0, i.hciversion, i.hcs1, i.hcs2, i.hcs3, i.hcc1, i.dboff, i.rtsoff)
}

impl Backend for Fake {
    fn read(&mut self, reg: Reg, index: u8) -> Result<RegRead, ReadErr> {
        let (stamp, session, alter) = self.begin();
        let err = |f: &Fake, kind: ReadFault| {
            f.note(format!("RF {} {} {} {} {} {}", reg.name(), index, kind.name(), stamp.seq, stamp.tick, session));
            Err(ReadErr { kind, stamp, session })
        };
        for a in &alter {
            if let Inj::Fault(k) = a {
                return err(self, *k);
            }
        }
        let v = match self.reg_value(reg, index) {
            Ok(Some(v)) => v,
            Ok(None) => return err(self, ReadFault::NotPresent),
            Err(k) => return err(self, k),
        };
        let mut r = RegRead { reg, index, width: reg.width(), value: v, stamp, session };
        for a in &alter {
            match a {
                Inj::WrongReg => r.reg = if reg == Reg::Usbsts { Reg::Usbcmd } else { Reg::Usbsts },
                Inj::WrongIndex => r.index = index.wrapping_add(1),
                Inj::BadWidth => r.width = if reg.width() == Width::W32 { Width::W16 } else { Width::W32 },
                Inj::Lie(x) => r.value = *x,
                Inj::ReservedSts if reg == Reg::Usbsts => r.value |= 0x8000_0000,
                Inj::HceSet if reg == Reg::Usbsts => r.value |= 1 << 12,
                _ => {}
            }
        }
        self.note(format!("R {} {} {} {} {} {} {}", r.reg.name(), r.index, r.width.name(), r.value, r.stamp.seq, r.stamp.tick, r.session));
        Ok(r)
    }

    fn read_identity(&mut self) -> ReadResult<Identity> {
        let (stamp, session, alter) = self.begin();
        for a in &alter {
            if let Inj::Fault(k) = a {
                self.note(format!("IF {} {} {} {}", k.name(), stamp.seq, stamp.tick, session));
                return Err(ReadErr { kind: *k, stamp, session });
            }
        }
        let id = self.identity();
        self.note(format!("I {} {} {} {}", ident_text(&id), stamp.seq, stamp.tick, session));
        Ok(Stamped { value: id, stamp, session })
    }

    fn read_topology(&mut self) -> ReadResult<Topology> {
        let (stamp, session, alter) = self.begin();
        for a in &alter {
            if let Inj::Fault(k) = a {
                self.note(format!("TF {} {} {} {}", k.name(), stamp.seq, stamp.tick, session));
                return Err(ReadErr { kind: *k, stamp, session });
            }
        }
        let t = Topology { xhci_functions: self.xhci_functions, scan_complete: self.scan_complete, behind_bridge: false };
        self.note(format!("T {} {} {} {} {} {}", t.xhci_functions, u8::from(t.scan_complete), u8::from(t.behind_bridge), stamp.seq, stamp.tick, session));
        Ok(Stamped { value: t, stamp, session })
    }

    fn read_platform(&mut self) -> ReadResult<PlatformFacts> {
        let (stamp, session, alter) = self.begin();
        for a in &alter {
            if let Inj::Fault(k) = a {
                self.note(format!("PF {} {} {} {}", k.name(), stamp.seq, stamp.tick, session));
                return Err(ReadErr { kind: *k, stamp, session });
            }
        }
        let p = PlatformFacts { profile: Profile::Qemu1021, iommu: self.q.iommu, identity_dma: self.q.identity, dma_limit: 1 << 30 };
        let ix = |t: Tri| Tri::ALL.iter().position(|x| *x == t).unwrap();
        self.note(format!("P {} {} {} {} {} {} {}", Profile::ALL.iter().position(|x| *x == p.profile).unwrap(), ix(p.iommu), ix(p.identity_dma), p.dma_limit, stamp.seq, stamp.tick, session));
        Ok(Stamped { value: p, stamp, session })
    }

    fn read_event(&mut self, index: u16, ccs: bool) -> ReadResult<Option<EventTrb>> {
        let (stamp, session, alter) = self.begin();
        for a in &alter {
            if let Inj::Fault(k) = a {
                self.note(format!("EF {} {} {} {}", k.name(), stamp.seq, stamp.tick, session));
                return Err(ReadErr { kind: *k, stamp, session });
            }
        }
        let mut e = self.d.read_event(index, ccs);
        if alter.contains(&Inj::WrongIndex) {
            e = e.map(|t| EventTrb { index: t.index.wrapping_add(1), ..t });
        }
        let text = e.map_or("none".to_string(), |t| format!("{} {} {} {} {} {} {} {} {}", t.index, u8::from(t.cycle), t.kind, t.pointer, t.code, t.param, t.slot, t.vf, t.port));
        self.note(format!("E {} {} {} {}", text, stamp.seq, stamp.tick, session));
        Ok(Stamped { value: e, stamp, session })
    }

    fn perform(&mut self, ev: &ModelEvent, ticket: WriteTicket) -> Result<Receipt, WriteErr> {
        let (stamp, session, mut alter) = self.begin();
        if let Some(pos) = self.on_write.iter().position(|(id, _)| *id == ev.id) {
            alter.push(self.on_write.remove(pos).1);
        }
        self.note(format!("W {} {} {} {} {}", ev.id, ev.target.name(), ev.width, ev.value, ticket.nonce()));
        let fail = |f: &mut Fake, kind: WriteFault| {
            f.note(format!("WF {} {} {} {}", kind.name(), stamp.seq, stamp.tick, session));
            Err(WriteErr { kind, stamp, session })
        };
        if self.terminal {
            let bit = match ev.id {
                "N-16" => 1,
                "N-17" => 2,
                _ => 0,
            };
            if bit == 0 || self.tail_used & bit != 0 {
                self.refused_writes += 1;
                return fail(self, WriteFault::Refused);
            }
            self.tail_used |= bit;
        }
        // A ticket is single-use: its nonce must be newer than every ticket already accepted (not necessarily consecutive).
        if ticket.nonce() < self.expect_nonce || ticket.id() != ev.id || alter.contains(&Inj::WriteBadTicket) {
            self.refused_writes += 1;
            return fail(self, WriteFault::BadTicket);
        }
        self.expect_nonce = ticket.nonce() + 1;
        if let Some(pos) = self.write_race.iter().position(|(id, _)| *id == ev.id) {
            let f = self.write_race.remove(pos).1;
            f(&mut self.d);
        }
        if alter.contains(&Inj::WriteRefused) {
            return fail(self, WriteFault::Refused);
        }
        if alter.contains(&Inj::WriteFault) {
            return fail(self, WriteFault::Fault);
        }
        if alter.contains(&Inj::WriteUnverifiedNotDone) {
            return fail(self, WriteFault::Unverified);
        }
        self.d.write(*ev);
        self.performed.push(*ev);
        if alter.contains(&Inj::WriteUnverifiedDone) {
            return fail(self, WriteFault::Unverified);
        }
        let rb = match ev.id {
            "N-04" => Some(Reg::Usblegsup),
            "N-01" | "N-13" | "N-16" => Some(Reg::Usbcmd),
            "N-02" | "N-11" | "N-17" => Some(Reg::PciCommand),
            "N-06" => Some(Reg::Config),
            "N-07" => Some(Reg::Dcbaap),
            "N-09" => Some(Reg::Erstsz),
            "N-10" => Some(Reg::Erdp),
            "N-12" => Some(Reg::Erstba),
            _ => None,
        };
        let mut readback = rb.map(|r| Readback { reg: r, index: 0, width: r.width(), value: self.reg_value(r, 0).ok().flatten().unwrap_or(0) });
        if alter.contains(&Inj::ReadbackMissing) {
            readback = None;
        }
        if alter.contains(&Inj::ReadbackWrongRegister) {
            readback = readback.map(|b| Readback { reg: if b.reg == Reg::Usbcmd { Reg::Usbsts } else { Reg::Usbcmd }, ..b });
        }
        if alter.contains(&Inj::ReadbackWrong) {
            readback = readback.map(|b| Readback { value: b.value ^ 0x1005, ..b });
        }
        let rbt = readback.map_or("none".to_string(), |b| format!("{} {} {} {}", b.reg.name(), b.index, b.width.name(), b.value));
        self.note(format!("WR {} {} {} {}", stamp.seq, stamp.tick, session, rbt));
        Ok(Receipt { stamp, session, readback })
    }

    fn latch_terminal(&mut self) {
        self.terminal = true;
        self.note("L".to_string());
    }
    fn now(&self) -> u64 {
        self.note(format!("N {}", self.now));
        self.now
    }
}

// ---- session harness --------------------------------------------------------------------------------------

pub const ALL_PAGES: [PageKind; 4] = [PageKind::Command, PageKind::Event, PageKind::Dcbaa, PageKind::Erst];

pub struct Entry {
    pub req: Req,
    pub res: Result<Done, Refusal>,
    pub io: Vec<String>,
}

pub struct SH {
    pub s: Session<Fake>,
    pub log: Vec<Entry>,
    pub target: u32,
}

pub fn snapshot(s: &Session<Fake>) -> String {
    format!("{:?}|{:?}|{:?}|{}|{:?}|{:?}|{}|{}", s.machine().phase(), s.closed(), s.last_seq(), s.last_write_seq(),
            (0..5).map(|k| s.fresh(k)).collect::<Vec<_>>(), s.knowledge(), s.backend().calls, s.backend().expect_nonce)
}

impl SH {
    pub fn new(q: Quirks, policy: ReleasePolicy) -> Self {
        Self::new_cfg(q, policy, Config { crcr_reads_zero: false })
    }
    pub fn new_cfg(q: Quirks, policy: ReleasePolicy, cfg: Config) -> Self {
        let s = Session::new(Fake::new(q), Machine::new(policy), cfg);
        Self { s, log: Vec::new(), target: u32::MAX }
    }
    pub fn qemu() -> Self {
        Self::new(Quirks::qemu(), ReleasePolicy::AfterQuiesce)
    }
    pub fn with(f: impl FnOnce(&mut Quirks)) -> Self {
        let mut q = Quirks::qemu();
        f(&mut q);
        Self::new(q, ReleasePolicy::AfterQuiesce)
    }
    pub fn fake(&mut self) -> &mut Fake {
        self.s.backend_mut()
    }

    pub fn req(&mut self, r: Req) -> Result<Done, Refusal> {
        let before = snapshot(&self.s);
        if r == Req::Tick {
            self.fake().advance();
        }
        let res = self.s.request(r);
        let io: Vec<String> = self.fake().log.borrow_mut().drain(..).collect();
        let observation = matches!(r, Req::Status | Req::Pci | Req::Reset | Req::Handoff | Req::Ports | Req::Event | Req::Qualify | Req::Tick);
        if let Err(e) = &res
            && (!observation || matches!(e, Refusal::Session(_)))
        {
            // A refusal that is not a machine refusal of a completed read consumes nothing: no backend
            // interaction (a tick may sample the clock) and no state change.
            assert!(io.iter().all(|l| l.starts_with("N ")) && (io.is_empty() || r == Req::Tick), "{r:?} refused {e:?} but touched the backend: {io:?}");
            assert_eq!(before, snapshot(&self.s), "a refused request changed the session: {r:?}");
        }
        if let Ok(Done { accepted: Some(_), .. }) = &res {
            if let Req::Op(op) = r {
                let fk = self.s.backend_mut();
                match op {
                    Operation::PrepareErst { segment, size } => {
                        fk.d.erst_mem.insert(ERS, (segment, size));
                    }
                    Operation::PublishNoop { index, cycle } => {
                        fk.d.cmd_mem.insert(CMD + 16 * u64::from(index), CmdTrb { kind: 23, cycle, toggle: false, target: 0 });
                    }
                    Operation::PublishLink { index, cycle, toggle, target } => {
                        fk.d.cmd_mem.insert(CMD + 16 * u64::from(index), CmdTrb { kind: 6, cycle, toggle, target });
                    }
                    _ => {}
                }
            }
        }
        record_session(&res, &self.s);
        self.log.push(Entry { req: r, res, io });
        res
    }

    /// Canonical next request, or `None` when the session is over.
    pub fn next(&self) -> Option<Req> {
        use Operation as O;
        let s = &self.s;
        let m = s.machine();
        let fk = s.backend();
        let last_tick = matches!(self.log.last().map(|e| e.req), Some(Req::Tick));
        let wait = |r: Req| if last_tick { Some(r) } else { Some(Req::Tick) };
        if s.closed().is_some() && m.phase() == Phase::Idle {
            return None;
        }
        let pages = ALL_PAGES;
        Some(match m.phase() {
            Phase::Idle => {
                if s.ports() == 0 && s.last_seq().is_none() { Req::Bind } else { Req::Qualify }
            }
            Phase::Qualified => {
                if fk.d.os_owned && !fk.d.bios_owned { Req::Op(O::DisableLegacySmi) } else { Req::Op(O::RequestOsOwnership) }
            }
            Phase::Handoff => return wait(Req::Handoff),
            Phase::HandedOff => Req::Op(O::DisableLegacySmi),
            Phase::Owned => {
                if m.run_stop() { Req::Op(O::ClearRunStop) } else { Req::Status }
            }
            Phase::Halting => return wait(Req::Status),
            Phase::Halted => {
                if !m.bus_master_verified() {
                    Req::Pci
                } else if m.bus_master() {
                    Req::Op(O::ClearBusMaster)
                } else {
                    Req::Op(O::AssertReset)
                }
            }
            Phase::Resetting => return wait(Req::Reset),
            Phase::Contained | Phase::Programming => {
                for k in pages {
                    match m.page(k).state {
                        PageState::Free => return Some(Req::Op(O::DmaAllocate { kind: k, addr: page_addr(k) })),
                        PageState::Allocated => return Some(Req::Op(O::DmaZero(k))),
                        _ => {}
                    }
                }
                for k in pages {
                    if m.page(k).state == PageState::Zeroed {
                        return Some(Req::Op(match k {
                            PageKind::Command => O::PrepareCommandRing,
                            PageKind::Event => O::PrepareEventRing,
                            PageKind::Dcbaa => O::PrepareDcbaa { entry0: 0 },
                            PageKind::Erst => O::PrepareErst { segment: EVT, size: 256 },
                        }));
                    }
                }
                let w = m.statics_written();
                Req::Op(if w & 1 == 0 {
                    O::SetMaxSlots(1)
                } else if w & 2 == 0 {
                    O::SetDcbaap(DCB)
                } else if w & 4 == 0 {
                    O::SetCommandRing { addr: CMD, rcs: true }
                } else if w & 8 == 0 {
                    O::SetErstSize(1)
                } else {
                    O::SetEventDequeue { addr: EVT, ehb: false }
                })
            }
            Phase::Programmed => Req::Op(O::SetBusMaster),
            Phase::BusMastering => {
                if !m.bus_master_verified() { Req::Pci } else { Req::Op(O::SetErstBase(ERS)) }
            }
            Phase::EventRingArmed => {
                if !m.status_clean() { Req::Status } else { Req::Op(O::SetRunStop) }
            }
            Phase::Starting => return wait(Req::Status),
            Phase::Running => {
                if m.poll_events() >= POLL_BUDGET {
                    Req::Op(O::BeginPollStep)
                } else if m.pending_command() {
                    let (idx, ccs) = m.event_cursor();
                    if fk.d.read_event(idx, ccs).is_some() { Req::Event } else { return wait(Req::Event) }
                } else if m.unacked() > 0 {
                    if s.fresh(4) { Req::Op(O::SetEventDequeue { addr: m.expected_erdp(), ehb: true }) } else { Req::Event }
                } else if m.noops_completed() >= self.target {
                    if s.fresh(0) { Req::Op(O::ClearRunStop) } else { Req::Status }
                } else if m.staged_command().is_some() {
                    if m.link_required() {
                        let (enq, pcs) = m.command_cursor();
                        Req::Op(O::PublishLink { index: enq, cycle: pcs, toggle: true, target: CMD })
                    } else if !s.fresh(0) {
                        Req::Status
                    } else {
                        Req::Op(O::RingCommandDoorbell)
                    }
                } else {
                    let (enq, pcs) = m.command_cursor();
                    Req::Op(O::PublishNoop { index: enq, cycle: pcs })
                }
            }
            Phase::Quiesced | Phase::Failed => {
                for k in pages {
                    if m.page(k).state != PageState::Free {
                        return Some(Req::Op(O::DmaRelease(k)));
                    }
                }
                return None;
            }
            Phase::Quarantined => match m.tail() {
                "need-halt" => Req::Op(O::ClearRunStop),
                "await-halt" => return wait(Req::Status),
                "need-bme-clear" => {
                    if s.fresh(0) { Req::Op(O::ClearBusMaster) } else { Req::Status }
                }
                "await-verify" => Req::Pci,
                _ => return None,
            },
            Phase::Refused => return None,
        })
    }

    pub fn run(&mut self, target: u32, limit: usize) -> usize {
        self.target = target;
        let mut n = 0;
        while n < limit {
            let Some(r) = self.next() else { break };
            let _ = self.req(r);
            n += 1;
        }
        n
    }
    pub fn goto(&mut self, phase: Phase) {
        for _ in 0..6000 {
            if self.s.machine().phase() == phase {
                return;
            }
            let r = self.next().unwrap_or_else(|| panic!("session ended in {} before {}", self.s.machine().phase().name(), phase.name()));
            let _ = self.req(r);
        }
        panic!("never reached {}", phase.name());
    }
}

use std::collections::HashSet;
use std::sync::Mutex;
/// Names of every session cause, session refusal and ledger reason observed by any test.
pub static COV2: Mutex<Option<[HashSet<&'static str>; 3]>> = Mutex::new(None);
fn cov(i: usize, n: &'static str) {
    COV2.lock().unwrap().get_or_insert_with(Default::default)[i].insert(n);
}
fn record_session(res: &Result<Done, Refusal>, s: &Session<Fake>) {
    match res {
        Ok(d) => {
            if let Some(c) = d.closed {
                cov(0, c.name());
            }
        }
        Err(Refusal::Session(r)) => cov(1, r.name()),
        Err(Refusal::Machine(r)) => {
            COVERAGE.lock().unwrap().get_or_insert_with(Default::default).1.insert(r.name());
        }
    }
    if let Some(c) = s.closed() {
        cov(0, c.name());
    }
}
pub fn rec_d<T>(r: Result<T, DReason>) -> Result<T, DReason> {
    if let Err(e) = &r {
        cov(2, e.name());
    }
    r
}

// ---- text encoding for the oracle -----------------------------------------------------------------------------------

pub fn req_text(r: &Req) -> String {
    match r {
        Req::Bind => "bind".into(),
        Req::Qualify => "qualify".into(),
        Req::Tick => "tick".into(),
        Req::Status => "status".into(),
        Req::Pci => "pci".into(),
        Req::Reset => "reset".into(),
        Req::Handoff => "handoff".into(),
        Req::Ports => "ports".into(),
        Req::Event => "event".into(),
        Req::Op(o) => format!("op {}", encode(o)),
    }
}

pub fn done_text(res: &Result<Done, Refusal>) -> String {
    match res {
        Ok(d) => format!("done {} closed={}", d.accepted.map_or("-".to_string(), |a| outcome(&Ok(a))), d.closed.map_or("-", |c| c.name())),
        Err(Refusal::Machine(r)) => format!("err m:{}", r.name()),
        Err(Refusal::Session(r)) => format!("err s:{}", r.name()),
    }
}

// ---- seeded generator for session requests --------------------------------------------------------------------------

pub fn random_req(r: &mut Rng, h: &SH) -> Req {
    let m = h.s.machine();
    match r.below(30) {
        0 => Req::Bind,
        1 => Req::Qualify,
        2 | 3 => Req::Tick,
        4..=6 => Req::Status,
        7 | 8 => Req::Pci,
        9 => Req::Reset,
        10 => Req::Handoff,
        11 => Req::Ports,
        12 | 13 => Req::Event,
        14 => Req::Op(Operation::Tick(5)),
        15 => Req::Op(Operation::ObserveStatus(Status { hch: true, hse: false, hce: false })),
        16 => {
            let h2 = SH::qemu();
            Req::Op(Operation::Qualify(h2.s.backend().d.facts()))
        }
        17 => Req::Op(Operation::ConsumeEvent(None)),
        _ => {
            // reuse the C4.1 random operation generator for everything else
            let hh = Harness::new(h.s.backend().q.clone(), ReleasePolicy::AfterQuiesce);
            let mut op = random_op(r, &hh);
            if matches!(op, Operation::SetEventDequeue { .. }) && m.phase() == Phase::Running && r.chance(2, 3) {
                op = Operation::SetEventDequeue { addr: m.expected_erdp(), ehb: true };
            }
            Req::Op(op)
        }
    }
}

pub fn random_inj(r: &mut Rng, calls: u64) -> (u64, Inj) {
    let at = calls + 1 + r.below(6);
    let i = match r.below(24) {
        0 => Inj::Fault(ReadFault::AllOnes),
        1 => Inj::Fault(ReadFault::Torn),
        2 => Inj::Fault(ReadFault::Partial),
        3 => Inj::Fault(ReadFault::Width),
        4 => Inj::WrongReg,
        5 => Inj::BadWidth,
        6 => Inj::SeqDup,
        7 => Inj::SeqBack,
        8 => Inj::SeqSkip,
        9 => Inj::ForeignSession,
        10 => Inj::Halt(r.chance(1, 2)),
        11 => Inj::Bme(r.chance(1, 2)),
        12 => Inj::Rs(r.chance(1, 2)),
        13 => Inj::Bios(r.chance(1, 2)),
        14 => Inj::Attach(1 << r.below(8)),
        15 => Inj::Vendor(0x8086),
        16 => Inj::SecondXhci,
        17 => Inj::ReservedSts,
        18 => Inj::HceSet,
        19 => Inj::WriteRefused,
        20 => Inj::WriteUnverifiedDone,
        21 => Inj::WriteUnverifiedNotDone,
        22 => Inj::ReadbackWrong,
        _ => Inj::ReadbackMissing,
    };
    (at, i)
}
