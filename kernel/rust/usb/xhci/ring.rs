//! Logical xHCI 1.2b sections 4.9.3-4.9.4, 6.4.2 and 6.4.3. No ring storage/DMA.
use super::{Error, PAGE, TRBS};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Trb {
    pub parameter: u64,
    pub status: u32,
    pub control: u32,
}
const _: () = assert!(core::mem::size_of::<Trb>() == 16);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Region {
    base: u64,
}
impl Region {
    pub fn new(base: u64, bytes: u64, address_limit: u64) -> Result<Self, Error> {
        if bytes != PAGE {
            return Err(Error::Bounds);
        }
        if base % PAGE != 0 {
            return Err(Error::Alignment);
        }
        let last = base.checked_add(bytes).ok_or(Error::Overflow)?;
        if base == 0 || last > address_limit || (base >> 16) != ((last - 1) >> 16) {
            return Err(Error::Bounds);
        }
        Ok(Self { base })
    }
    pub fn base(self) -> u64 {
        self.base
    }
    pub fn address(self, index: u16) -> Result<u64, Error> {
        if index >= TRBS {
            return Err(Error::Bounds);
        }
        self.base
            .checked_add(u64::from(index) * 16)
            .ok_or(Error::Overflow)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CommandId {
    address: u64,
    epoch: u64,
    serial: u64,
}
impl CommandId {
    pub fn address(self) -> u64 {
        self.address
    }
    pub fn epoch(self) -> u64 {
        self.epoch
    }
    pub fn serial(self) -> u64 {
        self.serial
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Submission {
    pub id: CommandId,
    pub index: u16,
    pub trb: Trb,
    pub link: Option<Trb>,
}

#[derive(Debug)]
pub struct CommandRing {
    region: Region,
    epoch: u64,
    serial: u64,
    next: u16,
    cycle: bool,
    pending: Option<CommandId>,
}
impl CommandRing {
    pub fn new(region: Region, epoch: u64) -> Result<Self, Error> {
        if epoch == 0 {
            return Err(Error::Stale);
        }
        Ok(Self {
            region,
            epoch,
            serial: 0,
            next: 0,
            cycle: true,
            pending: None,
        })
    }
    pub fn pending(&self) -> Option<CommandId> {
        self.pending
    }
    pub fn cursor(&self) -> (u16, bool) {
        (self.next, self.cycle)
    }
    pub fn prepare_noop(&mut self) -> Result<Submission, Error> {
        if self.pending.is_some() {
            return Err(Error::RingFull);
        }
        let serial = self.serial.checked_add(1).ok_or(Error::Overflow)?;
        let id = CommandId {
            address: self.region.address(self.next)?,
            epoch: self.epoch,
            serial,
        };
        let cycle = u32::from(self.cycle);
        let link = if self.next == TRBS - 2 {
            Some(Trb {
                parameter: self.region.base(),
                status: 0,
                control: (6 << 10) | 2 | cycle,
            })
        } else {
            None
        };
        let submission = Submission {
            id,
            index: self.next,
            trb: Trb {
                parameter: 0,
                status: 0,
                control: (23 << 10) | cycle,
            },
            link,
        };
        self.serial = serial;
        self.pending = Some(id);
        if link.is_some() {
            self.next = 0;
            self.cycle = !self.cycle;
        } else {
            self.next += 1;
        }
        Ok(submission)
    }
    /// `expected` is a software request handle, not a wire nonce.
    pub fn complete(
        &mut self,
        expected: CommandId,
        epoch: u64,
        event: Completion,
    ) -> Result<(), Error> {
        let trb = event.trb;
        let pending = self.pending.ok_or(Error::UnexpectedEvent)?;
        if expected != pending || epoch != self.epoch {
            return Err(Error::Stale);
        }
        if trb.control & !((0x3f << 10) | 1) != 0
            || (trb.control >> 10) & 0x3f != 33
            || trb.status != 1 << 24
            || trb.parameter != pending.address
        {
            return Err(Error::UnexpectedEvent);
        }
        self.pending = None;
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Completion {
    trb: Trb,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    Completion(Completion),
    PortChange(u8),
}

#[derive(Debug)]
pub struct EventRing {
    region: Region,
    head: u16,
    cycle: bool,
    consumed: u64,
}
impl EventRing {
    pub fn new(region: Region) -> Self {
        Self {
            region,
            head: 0,
            cycle: true,
            consumed: 0,
        }
    }
    pub fn cursor(&self) -> (u16, bool) {
        (self.head, self.cycle)
    }
    pub fn dequeue_address(&self) -> Result<u64, Error> {
        self.region.address(self.head)
    }
    pub fn inspect(&self, trb: Trb, ports: u8) -> Result<Option<Event>, Error> {
        if trb.control & 1 != u32::from(self.cycle) {
            return Ok(None);
        }
        if trb.control & !((0x3f << 10) | 1) != 0 || trb.status != 1 << 24 {
            return Err(Error::UnexpectedEvent);
        }
        match (trb.control >> 10) & 0x3f {
            33 => Ok(Some(Event::Completion(Completion { trb }))),
            34 => {
                let port = (trb.parameter >> 24) as u8;
                if trb.parameter & !(0xff << 24) != 0 || port == 0 || port > ports {
                    return Err(Error::UnexpectedEvent);
                }
                Ok(Some(Event::PortChange(port)))
            }
            _ => Err(Error::UnexpectedEvent),
        }
    }
    pub fn advance(&mut self) -> Result<(), Error> {
        let consumed = self.consumed.checked_add(1).ok_or(Error::Overflow)?;
        if self.head == TRBS - 1 {
            self.head = 0;
            self.cycle = !self.cycle;
        } else {
            self.head += 1;
        }
        self.consumed = consumed;
        Ok(())
    }
    pub fn consumed(&self) -> u64 {
        self.consumed
    }
}
