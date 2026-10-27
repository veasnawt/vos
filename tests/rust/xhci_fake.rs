//! Deterministic software-only backend. Arrays are HOST fixtures, never DMA.
use vos_xhci::xhci::capabilities::Facts;
use vos_xhci::xhci::controller::{Action, Backend, Observation, Sample};
use vos_xhci::xhci::resources::{Kind, Lease};
use vos_xhci::xhci::ring::{Submission, Trb};
use vos_xhci::xhci::{BAR_BYTES, Error, TRBS};

pub fn put(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
pub fn capabilities() -> ([u8; BAR_BYTES as usize], Facts) {
    let mut bytes = [0; BAR_BYTES as usize];
    put(&mut bytes, 0, 0x0100_0040);
    put(&mut bytes, 4, 0x0800_1040);
    put(&mut bytes, 8, 0xf);
    put(&mut bytes, 16, 0x0008_7001);
    put(&mut bytes, 20, 0x2000);
    put(&mut bytes, 24, 0x1000);
    put(&mut bytes, 0x20, 0x0200_0002); // supported-protocol extended capability
    put(&mut bytes, 0x24, 0x2042_5355);
    put(&mut bytes, 0x28, 0x0801); // ports 1..8, no protocol speed IDs
    put(&mut bytes, 0x48, 1); // 4 KiB PAGESIZE
    (
        bytes,
        Facts {
            bdf: 0x10,
            vendor: 0x1b36,
            device: 0xd,
            bar_base: 0xfebf0000,
            bar_bytes: BAR_BYTES,
            bar_address_bits: 64,
            physical_bits: 40,
            candidates: 1,
            complete_scan: true,
            memory_enabled: true,
            d0: true,
            identity_dma: true,
            coherent_dma: true,
            qualified_resource: true,
            ram_disjoint: true,
        },
    )
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Trace {
    Empty,
    Acquire(Kind),
    Release(Kind),
    Action(Action),
    Observe(Observation),
    Payload(u16),
    Cycle(u16),
    WriteBarrier,
    LinkPayload,
    LinkCycle,
    Doorbell,
    EventCycle(u16),
    ReadBarrier,
    Dequeue(u16),
}

pub struct Fake {
    identity: u64,
    pub trace: [Trace; 512],
    pub trace_len: usize,
    pub held: [bool; 7],
    tokens: [u64; 7],
    next_token: u64,
    pub acquire_calls: usize,
    pub fail_acquire: Option<usize>,
    pub action_calls: usize,
    pub fail_action: Option<usize>,
    pub fail_observe: Option<Observation>,
    pub fail_release: Option<Kind>,
    pub fail_publish: bool,
    pub fail_consume: bool,
    pub ready: [bool; 6],
    pub waits: [u16; 6],
    pub epoch: u64,
    pub drop_completion: bool,
    pub delay_polls: u16,
    pending: Option<Sample>,
    pub events: [Sample; TRBS as usize],
    pub commands: [Trb; TRBS as usize],
    pub queued: u16,
    producer: u16,
    producer_cycle: bool,
    reader: u16,
    command_index: u16,
    command_cycle: bool,
    pub owned: bool,
    halted: bool,
    reset_sent: bool,
    reset_ready: bool,
    programmed: bool,
    master: bool,
    run_sent: bool,
    pub running: bool,
    stop_sent: bool,
    pub quiescent: bool,
    finish_sent: bool,
    pub reaped: bool,
}
impl Fake {
    pub fn new(epoch: u64) -> Self {
        Self {
            identity: epoch,
            trace: [Trace::Empty; 512],
            trace_len: 0,
            held: [false; 7],
            tokens: [0; 7],
            next_token: 1,
            acquire_calls: 0,
            fail_acquire: None,
            action_calls: 0,
            fail_action: None,
            fail_observe: None,
            fail_release: None,
            fail_publish: false,
            fail_consume: false,
            ready: [true; 6],
            waits: [0; 6],
            epoch,
            drop_completion: false,
            delay_polls: 0,
            pending: None,
            events: [Sample {
                epoch: 0,
                trb: Trb {
                    parameter: 0,
                    status: 0,
                    control: 0,
                },
            }; TRBS as usize],
            commands: [Trb::default(); TRBS as usize],
            queued: 0,
            producer: 0,
            producer_cycle: true,
            reader: 0,
            command_index: 0,
            command_cycle: true,
            owned: false,
            halted: false,
            reset_sent: false,
            reset_ready: false,
            programmed: false,
            master: false,
            run_sent: false,
            running: false,
            stop_sent: false,
            quiescent: false,
            finish_sent: false,
            reaped: false,
        }
    }
    pub fn clear_trace(&mut self) {
        self.trace_len = 0;
    }
    fn log(&mut self, t: Trace) -> Result<(), Error> {
        let slot = self.trace.get_mut(self.trace_len).ok_or(Error::Backend)?;
        *slot = t;
        self.trace_len += 1;
        Ok(())
    }
    pub fn enqueue(&mut self, mut sample: Sample) -> Result<(), Error> {
        if self.queued == TRBS {
            return Err(Error::RingFull);
        }
        sample.trb.control = (sample.trb.control & !1) | u32::from(self.producer_cycle);
        self.events[self.producer as usize] = sample;
        self.queued += 1;
        if self.producer == TRBS - 1 {
            self.producer = 0;
            self.producer_cycle = !self.producer_cycle;
        } else {
            self.producer += 1;
        }
        Ok(())
    }
    pub fn completion(address: u64, epoch: u64) -> Sample {
        Sample {
            epoch,
            trb: Trb {
                parameter: address,
                status: 1 << 24,
                control: 33 << 10,
            },
        }
    }
    pub fn port(port: u8, epoch: u64) -> Sample {
        Sample {
            epoch,
            trb: Trb {
                parameter: u64::from(port) << 24,
                status: 1 << 24,
                control: 34 << 10,
            },
        }
    }
    pub fn address(kind: Kind) -> u64 {
        match kind {
            Kind::Worker => 0,
            Kind::Mapping => 0xfebf0000,
            Kind::Control => 0x100000,
            Kind::Command => 0x101000,
            Kind::Event => 0x102000,
            Kind::Dcbaa => 0x103000,
            Kind::Erst => 0x104000,
        }
    }
}
impl Backend for Fake {
    fn identity(&self) -> u64 {
        self.identity
    }
    fn acquire(&mut self, kind: Kind) -> Result<Lease, Error> {
        self.log(Trace::Acquire(kind))?;
        self.acquire_calls += 1;
        if self.fail_acquire == Some(self.acquire_calls) {
            return Err(Error::Allocation);
        }
        if self.held[kind as usize] {
            return Err(Error::Backend);
        }
        if kind == Kind::Worker && !self.held[Kind::Control as usize] {
            return Err(Error::Backend);
        }
        let token = self.next_token;
        self.next_token = token.checked_add(1).ok_or(Error::Overflow)?;
        let lease = Lease::new(kind, token, Self::address(kind))?;
        self.held[kind as usize] = true;
        self.tokens[kind as usize] = token;
        Ok(lease)
    }
    fn release(&mut self, lease: &Lease) -> Result<(), Error> {
        let k = lease.kind();
        self.log(Trace::Release(k))?;
        if self.fail_release == Some(k) {
            return Err(Error::Backend);
        }
        if !self.held[k as usize]
            || self.tokens[k as usize] != lease.token()
            || ((k.dma() || k == Kind::Mapping) && self.programmed && !self.quiescent)
            || (k == Kind::Worker && !self.reaped)
            || (k == Kind::Control && self.held[Kind::Worker as usize] && !self.reaped)
        {
            return Err(Error::Backend);
        }
        self.held[k as usize] = false;
        Ok(())
    }
    fn action(&mut self, a: Action) -> Result<(), Error> {
        self.log(Trace::Action(a))?;
        self.action_calls += 1;
        if self.fail_action == Some(self.action_calls) {
            return Err(Error::Backend);
        }
        match a {
            Action::RequestOwnership { .. } => {
                self.quiescent = false;
                self.reaped = false;
                self.finish_sent = false;
                self.owned = false;
                self.halted = false;
                self.reset_ready = false;
                self.reset_sent = false;
                self.programmed = false;
                self.stop_sent = false;
                self.run_sent = false;
            }
            Action::Halt => {
                if !self.owned {
                    return Err(Error::Backend);
                }
            }
            Action::DisableMaster => {
                if !self.halted {
                    return Err(Error::Backend);
                }
                self.master = false;
            }
            Action::Reset => {
                if !self.halted || self.master {
                    return Err(Error::Backend);
                }
                self.reset_sent = true;
            }
            Action::Program {
                command,
                event,
                dcbaa,
                erst,
            } => {
                if !self.reset_ready
                    || self.master
                    || !self.held.iter().all(|x| *x)
                    || [command, event, dcbaa, erst]
                        != [
                            Self::address(Kind::Command),
                            Self::address(Kind::Event),
                            Self::address(Kind::Dcbaa),
                            Self::address(Kind::Erst),
                        ]
                {
                    return Err(Error::Backend);
                }
                self.programmed = true;
                self.commands.fill(Trb::default());
                self.events.fill(Sample {
                    epoch: 0,
                    trb: Trb::default(),
                });
                self.command_index = 0;
                self.command_cycle = true;
                self.producer = 0;
                self.producer_cycle = true;
                self.reader = 0;
                self.queued = 0;
                self.pending = None;
            }
            Action::EnableMaster => {
                if !self.programmed {
                    return Err(Error::Backend);
                }
                self.master = true;
            }
            Action::Run => {
                if !self.master || !self.programmed {
                    return Err(Error::Backend);
                }
                self.run_sent = true;
            }
            Action::Stop => {
                if !self.owned {
                    return Err(Error::Backend);
                }
                self.stop_sent = true;
            }
            Action::WithdrawOwnership => {
                if self.owned {
                    return Err(Error::Backend);
                }
            }
            Action::FinishWorker => {
                self.finish_sent = true;
            }
        }
        Ok(())
    }
    fn observe(&mut self, o: Observation) -> Result<bool, Error> {
        self.log(Trace::Observe(o))?;
        if self.fail_observe == Some(o) {
            return Err(Error::Backend);
        }
        let index = o as usize;
        if self.waits[index] != 0 {
            self.waits[index] -= 1;
            return Ok(false);
        }
        if !self.ready[index] {
            return Ok(false);
        }
        match o {
            Observation::Ownership => self.owned = true,
            Observation::Halted => {
                self.halted = true;
                self.running = false;
            }
            Observation::ResetReady => {
                if !self.reset_sent {
                    return Err(Error::Backend);
                }
                self.reset_ready = true;
            }
            Observation::Running => {
                if !self.run_sent {
                    return Err(Error::Backend);
                }
                self.running = true;
            }
            Observation::Quiescent => {
                if !self.stop_sent {
                    return Err(Error::Backend);
                }
                self.running = false;
                self.master = false;
                self.quiescent = true;
            }
            Observation::WorkerReaped => {
                if !self.finish_sent {
                    return Err(Error::Backend);
                }
                self.reaped = true;
            }
        }
        Ok(true)
    }
    fn publish(&mut self, s: &Submission) -> Result<(), Error> {
        if !self.running || !self.master || self.reaped {
            return Err(Error::Backend);
        }
        if self.fail_publish {
            return Err(Error::Backend);
        }
        if s.index != self.command_index
            || s.trb.control != (23 << 10) | u32::from(self.command_cycle)
            || s.trb.parameter != 0
            || s.trb.status != 0
            || s.id.address() != Self::address(Kind::Command) + u64::from(s.index) * 16
        {
            return Err(Error::Backend);
        }
        self.commands[s.index as usize] = Trb {
            control: s.trb.control ^ 1,
            ..s.trb
        };
        self.log(Trace::Payload(s.index))?;
        self.commands[s.index as usize].control = s.trb.control;
        self.log(Trace::Cycle(s.index))?;
        self.log(Trace::WriteBarrier)?;
        if s.index == TRBS - 2 {
            let link = s.link.ok_or(Error::Backend)?;
            if link.parameter != Self::address(Kind::Command)
                || link.status != 0
                || link.control != (6 << 10) | 2 | u32::from(self.command_cycle)
            {
                return Err(Error::Backend);
            }
            self.commands[TRBS as usize - 1] = Trb {
                control: link.control ^ 1,
                ..link
            };
            self.log(Trace::LinkPayload)?;
            self.commands[TRBS as usize - 1].control = link.control;
            self.log(Trace::LinkCycle)?;
            self.log(Trace::WriteBarrier)?;
            self.command_index = 0;
            self.command_cycle = !self.command_cycle;
        } else {
            if s.link.is_some() {
                return Err(Error::Backend);
            }
            self.command_index += 1;
        }
        self.log(Trace::Doorbell)?;
        if !self.drop_completion {
            let sample = Self::completion(s.id.address(), self.epoch);
            if self.delay_polls != 0 {
                self.pending = Some(sample);
            } else {
                self.enqueue(sample)?;
            }
        }
        Ok(())
    }
    fn peek(&mut self, index: u16) -> Result<Option<Sample>, Error> {
        if index != self.reader || self.reaped {
            return Err(Error::Backend);
        }
        self.log(Trace::EventCycle(index))?;
        if self.pending.is_some() {
            if self.delay_polls != 0 {
                self.delay_polls -= 1;
            } else if let Some(sample) = self.pending.take() {
                self.enqueue(sample)?;
            }
        }
        if self.queued == 0 {
            return Ok(None);
        }
        self.log(Trace::ReadBarrier)?;
        Ok(Some(self.events[index as usize]))
    }
    fn consumed(&mut self, next_address: u64) -> Result<(), Error> {
        if self.fail_consume {
            return Err(Error::Backend);
        }
        if self.queued == 0 {
            return Err(Error::Backend);
        }
        let next = (self.reader + 1) % TRBS;
        if next_address != Self::address(Kind::Event) + u64::from(next) * 16 {
            return Err(Error::Backend);
        }
        self.reader = next;
        self.queued -= 1;
        self.log(Trace::Dequeue(next))
    }
}
