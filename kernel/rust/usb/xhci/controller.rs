//! Cooperative lifecycle MODEL. There is deliberately no live Backend in USB.2-A.
use super::capabilities::{Capabilities, Facts};
use super::resources::{KINDS, Kind, Lease, Usage};
use super::ring::{CommandId, CommandRing, Event, EventRing, Region, Submission, Trb};
use super::{Error, PAGE};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Uninitialized,
    Qualified,
    Ownership,
    Halting,
    Resetting,
    Starting,
    Ready,
    Polling,
    Stopping,
    Joining,
    Stopped,
    Failed,
    Quarantined,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Observation {
    Ownership,
    Halted,
    ResetReady,
    Running,
    Quiescent,
    WorkerReaped,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    RequestOwnership {
        legacy: bool,
    },
    Halt,
    DisableMaster,
    Reset,
    Program {
        command: u64,
        event: u64,
        dcbaa: u64,
        erst: u64,
    },
    EnableMaster,
    Run,
    Stop,
    WithdrawOwnership,
    FinishWorker,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sample {
    pub epoch: u64,
    pub trb: Trb,
}

/// Trusted kernel-side effect contract, NOT an application or plugin interface.
/// acquire must return a unique owned lease of the requested kind or roll back
/// internally on Err. release must preserve ownership on Err. Observations are
/// proofs supplied by a future reviewed adapter; USB.2-A supplies only a fake.
pub trait Backend {
    /// Stable, nonzero resource namespace, unique among simultaneous backends.
    /// A future platform must mint this with its lease owner, not a task config.
    fn identity(&self) -> u64;
    fn acquire(&mut self, kind: Kind) -> Result<Lease, Error>;
    fn release(&mut self, lease: &Lease) -> Result<(), Error>;
    fn action(&mut self, action: Action) -> Result<(), Error>;
    fn observe(&mut self, observation: Observation) -> Result<bool, Error>;
    /// Payload, cycle-last, DMA barrier, link when needed, then doorbell.
    fn publish(&mut self, command: &Submission) -> Result<(), Error>;
    /// Future adapter must implement cycle-first DMA visibility before decoding.
    fn peek(&mut self, index: u16) -> Result<Option<Sample>, Error>;
    fn consumed(&mut self, next_address: u64) -> Result<(), Error>;
}

#[derive(Clone, Copy, Debug)]
pub struct Timeouts {
    pub ownership: u64,
    pub halt: u64,
    pub reset: u64,
    pub run: u64,
    pub command: u64,
    pub stop: u64,
    pub join: u64,
}
impl Timeouts {
    /// Units are caller-supplied monotonic ticks, not claimed wall-clock seconds.
    pub fn validate(self) -> Result<Self, Error> {
        for x in [
            self.ownership,
            self.halt,
            self.reset,
            self.run,
            self.command,
            self.stop,
            self.join,
        ] {
            if x == 0 || x >= 1 << 63 {
                return Err(Error::Bounds);
            }
        }
        Ok(self)
    }
}
#[derive(Clone, Copy, Debug)]
pub struct Deadline {
    first: u64,
    ticks: u64,
}
impl Deadline {
    pub fn new(first: u64, ticks: u64) -> Result<Self, Error> {
        if ticks == 0 || ticks >= 1 << 63 {
            return Err(Error::Bounds);
        }
        Ok(Self { first, ticks })
    }
    pub fn expired(self, now: u64) -> Result<bool, Error> {
        let elapsed = now.wrapping_sub(self.first);
        if elapsed >= 1 << 63 {
            return Err(Error::Clock);
        }
        Ok(elapsed >= self.ticks)
    }
}

/// A diagnostic snapshot, not a hardware release proof or activated global owner.
#[derive(Debug)]
pub struct PermanentSnapshot {
    pub tokens: [u64; 7],
    pub addresses: [u64; 7],
    pub epoch: u64,
    pub backend_owner: u64,
    pub usage: Usage,
    pub failure: Option<Error>,
}
const _: () = assert!(core::mem::size_of::<PermanentSnapshot>() <= 512);

#[must_use]
#[derive(Debug)]
pub struct Controller {
    _local: core::marker::PhantomData<*mut ()>,
    state: State,
    caps: Option<Capabilities>,
    leases: [Option<Lease>; 7],
    usage: Usage,
    high_water: u64,
    epoch: u64,
    backend_owner: u64,
    command: Option<CommandRing>,
    events: Option<EventRing>,
    timeouts: Timeouts,
    deadline: Option<Deadline>,
    owned: bool,
    visible: bool,
    quarantine: bool,
    failure: Option<Error>,
    cleanup_error: Option<Error>,
    port_events: u64,
}
const _: () = assert!(core::mem::size_of::<Controller>() <= 2048);

impl Controller {
    pub fn new(timeouts: Timeouts) -> Result<Self, Error> {
        Ok(Self {
            state: State::Uninitialized,
            _local: core::marker::PhantomData,
            caps: None,
            leases: [const { None }; 7],
            usage: Usage::default(),
            high_water: 0,
            epoch: 0,
            backend_owner: 0,
            command: None,
            events: None,
            timeouts: timeouts.validate()?,
            deadline: None,
            owned: false,
            visible: false,
            quarantine: false,
            failure: None,
            cleanup_error: None,
            port_events: 0,
        })
    }
    pub fn state(&self) -> State {
        self.state
    }
    pub fn usage(&self) -> Usage {
        self.usage
    }
    pub fn high_water(&self) -> u64 {
        self.high_water
    }
    pub fn device_visible(&self) -> bool {
        self.visible
    }
    pub fn failure(&self) -> Option<Error> {
        self.failure
    }
    pub fn cleanup_error(&self) -> Option<Error> {
        self.cleanup_error
    }
    pub fn port_events(&self) -> u64 {
        self.port_events
    }
    pub fn pending(&self) -> Option<CommandId> {
        self.command.as_ref().and_then(CommandRing::pending)
    }
    pub fn qualify(&mut self, bytes: &[u8], facts: Facts) -> Result<(), Error> {
        if !matches!(self.state, State::Uninitialized | State::Stopped) {
            return Err(Error::IllegalState);
        }
        let caps = Capabilities::parse(bytes, facts)?;
        self.caps = Some(caps);
        self.state = State::Qualified;
        Ok(())
    }
    fn at(&mut self, state: State, now: u64, ticks: u64) -> Result<(), Error> {
        let deadline = Deadline::new(now, ticks)?;
        self.state = state;
        self.deadline = Some(deadline);
        Ok(())
    }
    fn acquire<B: Backend>(&mut self, b: &mut B, kind: Kind) -> Result<(), Error> {
        let i = kind as usize;
        if self.leases[i].is_some() {
            return Err(Error::IllegalState);
        }
        let usage = self.usage.add(kind.charge())?; // admission BEFORE the backend effect
        let lease = b.acquire(kind)?;
        // Retain ownership even if a backend violates the requested-kind contract.
        let valid = lease.kind() == kind
            && !self
                .leases
                .iter()
                .flatten()
                .any(|x| x.token() == lease.token() || x.overlaps(&lease));
        self.leases[i] = Some(lease);
        self.usage = usage;
        self.high_water = self.high_water.max(usage.physical_bytes());
        if valid { Ok(()) } else { Err(Error::Backend) }
    }
    fn bound<B: Backend>(&self, b: &B) -> Result<(), Error> {
        if self.backend_owner == 0 || b.identity() != self.backend_owner {
            Err(Error::Stale)
        } else {
            Ok(())
        }
    }
    fn address(&self, kind: Kind) -> Result<u64, Error> {
        self.leases[kind as usize]
            .as_ref()
            .map(Lease::address)
            .ok_or(Error::IllegalState)
    }
    fn release<B: Backend>(&mut self, b: &mut B, kind: Kind) -> Result<(), Error> {
        if let Some(lease) = &self.leases[kind as usize] {
            let remaining = self.usage.subtract(kind.charge())?;
            b.release(lease)?; // a failed release must NOT drop the ownership record
            self.leases[kind as usize] = None;
            self.usage = remaining;
        }
        Ok(())
    }
    pub fn start<B: Backend>(&mut self, b: &mut B, epoch: u64, now: u64) -> Result<(), Error> {
        if self.state != State::Qualified {
            return Err(Error::IllegalState);
        }
        if epoch == 0 || epoch <= self.epoch || b.identity() == 0 {
            return Err(Error::Stale);
        }
        self.epoch = epoch;
        self.backend_owner = b.identity();
        self.owned = false;
        self.visible = false;
        self.quarantine = false;
        self.failure = None;
        self.cleanup_error = None;
        self.command = None;
        self.events = None;
        let result = (|| {
            for kind in [Kind::Control, Kind::Worker, Kind::Mapping] {
                self.acquire(b, kind)?;
            }
            let caps = self.caps.ok_or(Error::IllegalState)?;
            if self.address(Kind::Mapping)? != caps.bar_base {
                return Err(Error::Backend);
            }
            b.action(Action::RequestOwnership {
                legacy: caps.legacy.is_some(),
            })?;
            self.at(State::Ownership, now, self.timeouts.ownership)
        })();
        if let Err(error) = result {
            self.fail(b, error, now);
        }
        result
    }
    fn remember(&mut self, error: Error) {
        if self.failure.is_none() {
            self.failure = Some(error);
        }
    }
    fn cleanup_fault(&mut self, error: Error) {
        if self.cleanup_error.is_none() {
            self.cleanup_error = Some(error);
        }
        self.quarantine = true;
    }
    fn terminal(&mut self) {
        self.deadline = None;
        self.state = if self.quarantine {
            State::Quarantined
        } else if self.failure.is_some() {
            State::Failed
        } else {
            State::Stopped
        };
    }
    fn join<B: Backend>(&mut self, b: &mut B, now: u64) {
        if !self.quarantine {
            for kind in [
                Kind::Erst,
                Kind::Dcbaa,
                Kind::Event,
                Kind::Command,
                Kind::Mapping,
            ] {
                if let Err(e) = self.release(b, kind) {
                    self.cleanup_fault(e);
                    break;
                }
            }
        }
        if self.leases[Kind::Worker as usize].is_none() {
            if let Err(e) = self.release(b, Kind::Control) {
                self.cleanup_fault(e);
            }
            self.terminal();
            return;
        }
        if let Err(e) = b.action(Action::FinishWorker) {
            self.cleanup_fault(e);
            self.terminal();
            return;
        }
        // Timeouts were validated at construction; no caller-supplied unchecked duration.
        if let Err(e) = self.at(State::Joining, now, self.timeouts.join) {
            self.cleanup_fault(e);
            self.terminal();
        }
    }
    fn stop<B: Backend>(&mut self, b: &mut B, now: u64) {
        if self.owned {
            match b.action(Action::Stop) {
                Ok(()) => {
                    if let Err(e) = self.at(State::Stopping, now, self.timeouts.stop) {
                        self.cleanup_fault(e);
                        self.join(b, now);
                    }
                }
                Err(e) => {
                    self.cleanup_fault(e);
                    self.join(b, now);
                }
            }
        } else {
            if let Err(e) = b.action(Action::WithdrawOwnership) {
                self.cleanup_fault(e);
            }
            self.join(b, now);
        }
    }
    fn fail<B: Backend>(&mut self, b: &mut B, error: Error, now: u64) {
        self.remember(error);
        self.stop(b, now);
    }
    pub fn shutdown<B: Backend>(&mut self, b: &mut B, now: u64) -> Result<(), Error> {
        if matches!(self.state, State::Uninitialized | State::Qualified) {
            return Err(Error::IllegalState);
        }
        self.bound(b)?;
        match self.state {
            State::Stopped | State::Failed | State::Stopping | State::Joining => Ok(()),
            State::Quarantined => Err(Error::Quarantined),
            State::Ownership
            | State::Halting
            | State::Resetting
            | State::Starting
            | State::Ready
            | State::Polling => {
                self.stop(b, now);
                Ok(())
            }
            _ => Err(Error::IllegalState),
        }
    }
    pub fn cancel<B: Backend>(&mut self, b: &mut B, now: u64) -> Result<(), Error> {
        if matches!(
            self.state,
            State::Stopped | State::Failed | State::Quarantined
        ) {
            return self.shutdown(b, now);
        }
        if matches!(self.state, State::Uninitialized | State::Qualified) {
            return Err(Error::IllegalState);
        }
        self.bound(b)?;
        self.remember(Error::Cancelled);
        self.shutdown(b, now)
    }
    fn prepare<B: Backend>(&mut self, b: &mut B, now: u64) -> Result<(), Error> {
        for kind in [Kind::Command, Kind::Event, Kind::Dcbaa, Kind::Erst] {
            self.acquire(b, kind)?;
        }
        let command = self.address(Kind::Command)?;
        let event = self.address(Kind::Event)?;
        self.command = Some(CommandRing::new(
            Region::new(command, PAGE, 1 << 30)?,
            self.epoch,
        )?);
        self.events = Some(EventRing::new(Region::new(event, PAGE, 1 << 30)?));
        // Treat even a partially failed publication as device-visible.
        self.visible = true;
        b.action(Action::Program {
            command,
            event,
            dcbaa: self.address(Kind::Dcbaa)?,
            erst: self.address(Kind::Erst)?,
        })?;
        b.action(Action::EnableMaster)?;
        b.action(Action::Run)?;
        self.at(State::Starting, now, self.timeouts.run)
    }
    pub fn submit_noop<B: Backend>(&mut self, b: &mut B, now: u64) -> Result<CommandId, Error> {
        if !matches!(self.state, State::Ready | State::Polling) {
            return Err(Error::IllegalState);
        }
        self.bound(b)?;
        if self.state == State::Polling {
            return Err(Error::RingFull);
        }
        if self.state != State::Ready {
            return Err(Error::IllegalState);
        }
        let submission = self
            .command
            .as_mut()
            .ok_or(Error::IllegalState)?
            .prepare_noop()?;
        if let Err(e) = b.publish(&submission) {
            self.fail(b, e, now);
            return Err(e);
        }
        self.at(State::Polling, now, self.timeouts.command)?;
        Ok(submission.id)
    }
    fn poll<B: Backend>(&mut self, b: &mut B) -> Result<(), Error> {
        let ports = self.caps.ok_or(Error::IllegalState)?.ports;
        for _ in 0..32 {
            let ring = self.events.as_mut().ok_or(Error::IllegalState)?;
            let Some(sample) = b.peek(ring.cursor().0)? else {
                return Ok(());
            };
            let Some(event) = ring.inspect(sample.trb, ports)? else {
                return Ok(());
            };
            if sample.epoch != self.epoch {
                return Err(Error::Stale);
            }
            match event {
                Event::Completion(trb) => {
                    let command = self.command.as_mut().ok_or(Error::IllegalState)?;
                    let id = command.pending().ok_or(Error::UnexpectedEvent)?;
                    command.complete(id, sample.epoch, trb)?;
                }
                Event::PortChange(_) => {
                    self.port_events = self.port_events.checked_add(1).ok_or(Error::Overflow)?;
                }
            }
            ring.advance()?;
            b.consumed(ring.dequeue_address()?)?;
            if matches!(event, Event::Completion(_)) {
                self.state = State::Ready;
                self.deadline = None;
                return Ok(());
            }
        }
        Ok(())
    }
    /// At most one condition observation, or 32 event reads, per invocation.
    /// Caller must wait/yield; this function neither sleeps nor spins on time.
    pub fn step<B: Backend>(&mut self, b: &mut B, now: u64) -> Result<State, Error> {
        if matches!(self.state, State::Uninitialized | State::Qualified) {
            return Err(Error::IllegalState);
        }
        self.bound(b)?;
        if matches!(
            self.state,
            State::Stopped | State::Failed | State::Quarantined | State::Ready
        ) {
            return Ok(self.state);
        }
        if matches!(self.state, State::Uninitialized | State::Qualified) {
            return Err(Error::IllegalState);
        }
        let expired = self.deadline.ok_or(Error::IllegalState)?.expired(now);
        if expired != Ok(false) {
            let error = expired.err().unwrap_or(Error::Timeout);
            self.remember(error);
            match self.state {
                State::Stopping => {
                    self.quarantine = true;
                    self.join(b, now);
                }
                State::Joining => {
                    self.quarantine = true;
                    self.terminal();
                }
                _ => self.stop(b, now),
            }
            return Err(error);
        }
        let result = match self.state {
            State::Ownership => b.observe(Observation::Ownership).and_then(|ready| {
                if ready {
                    self.owned = true;
                    b.action(Action::Halt)?;
                    self.at(State::Halting, now, self.timeouts.halt)?;
                }
                Ok(())
            }),
            State::Halting => b.observe(Observation::Halted).and_then(|ready| {
                if ready {
                    b.action(Action::DisableMaster)?;
                    b.action(Action::Reset)?;
                    self.at(State::Resetting, now, self.timeouts.reset)?;
                }
                Ok(())
            }),
            State::Resetting => b.observe(Observation::ResetReady).and_then(|ready| {
                if ready {
                    self.prepare(b, now)?;
                }
                Ok(())
            }),
            State::Starting => b.observe(Observation::Running).map(|ready| {
                if ready {
                    self.state = State::Ready;
                    self.deadline = None;
                }
            }),
            State::Polling => self.poll(b),
            State::Stopping => b.observe(Observation::Quiescent).map(|ready| {
                if ready {
                    self.visible = false;
                    self.owned = false;
                    self.join(b, now);
                }
            }),
            State::Joining => b.observe(Observation::WorkerReaped).map(|ready| {
                if ready {
                    for kind in [Kind::Control, Kind::Worker] {
                        if let Err(e) = self.release(b, kind) {
                            self.cleanup_fault(e);
                        }
                    }
                    self.terminal();
                }
            }),
            _ => Err(Error::IllegalState),
        };
        if let Err(error) = result {
            match self.state {
                State::Stopping => {
                    self.remember(error);
                    self.quarantine = true;
                    self.join(b, now);
                }
                State::Joining => {
                    self.remember(error);
                    self.quarantine = true;
                    self.terminal();
                }
                _ => self.fail(b, error, now),
            }
            return Err(error);
        }
        Ok(self.state)
    }
    pub fn snapshot(&self) -> PermanentSnapshot {
        let mut tokens = [0; 7];
        let mut addresses = [0; 7];
        for kind in KINDS {
            if let Some(lease) = &self.leases[kind as usize] {
                tokens[kind as usize] = lease.token();
                addresses[kind as usize] = lease.address();
            }
        }
        PermanentSnapshot {
            tokens,
            addresses,
            epoch: self.epoch,
            backend_owner: self.backend_owner,
            usage: self.usage,
            failure: self.failure,
        }
    }
}
