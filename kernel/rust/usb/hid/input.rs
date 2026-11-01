//! Trusted broker supplies Focus; trusted transport supplies actual completion status. Neither is a user capability.
//! No caller can feed reports before confirmation, and reset invalidates old protocol request generations.
use crate::{Binding, Error, Kind};
pub const QUEUE: usize = 32;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Focus {
    pub owner: u64,
    pub epoch: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    Key { usage: u8, down: bool },
    Pointer { dx: i16, dy: i16, buttons: u8 },
    Reset,
}
const EMPTY: Event = Event::Reset;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProtocolRequest {
    device: u64,
    generation: u64,
    interface: u8,
}
impl ProtocolRequest {
    /// Exact USB SET_PROTOCOL(Boot) setup packet. Returning bytes does not send a request.
    pub fn setup(self) -> [u8; 8] {
        [0x21, 0x0b, 0, 0, self.interface, 0, 0, 0]
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Completion {
    Success,
    Stall,
    Timeout,
    Failed,
}
pub struct Session {
    binding: Binding,
    device: u64,
    generation: u64,
    pending: bool,
    confirmed: bool,
    disconnected: bool,
    focus: Option<Focus>,
    last_epoch: u64,
    queue: [Event; QUEUE],
    head: u8,
    count: u8,
    barrier: bool,
    modifiers: u8,
    keys: [u8; 6],
    buttons: u8,
    armed: bool,
}
impl Session {
    /// `device` must be a broker-minted nonzero lifetime ID, never reused while an old request could survive.
    pub fn new(binding: Binding, device: u64, generation: u64) -> Result<Self, Error> {
        if device == 0 || generation == 0 {
            return Err(Error::Stale);
        }
        Ok(Self {
            binding,
            device,
            generation,
            pending: false,
            confirmed: false,
            disconnected: false,
            focus: None,
            last_epoch: 0,
            queue: [EMPTY; QUEUE],
            head: 0,
            count: 0,
            barrier: true,
            modifiers: 0,
            keys: [0; 6],
            buttons: 0,
            armed: false,
        })
    }
    fn clear(&mut self) {
        self.queue = [EMPTY; QUEUE];
        self.head = 0;
        self.count = 0;
        self.modifiers = 0;
        self.keys = [0; 6];
        self.buttons = 0;
        self.armed = false;
        self.barrier = true;
    }
    /// Called only after real input-broker authority checks. Epochs strictly increase, even for PID reuse.
    pub fn focus(&mut self, f: Focus) -> Result<(), Error> {
        if f.owner == 0 || f.epoch <= self.last_epoch {
            return Err(Error::Ownership);
        }
        self.clear();
        self.last_epoch = f.epoch;
        self.focus = Some(f);
        Ok(())
    }
    pub fn revoke(&mut self, epoch: u64) -> Result<(), Error> {
        if epoch <= self.last_epoch {
            return Err(Error::Ownership);
        }
        self.clear();
        self.last_epoch = epoch;
        self.focus = None;
        Ok(())
    }
    pub fn request_protocol(&mut self) -> Result<ProtocolRequest, Error> {
        if self.pending || self.confirmed || self.disconnected {
            return Err(Error::Protocol);
        }
        self.pending = true;
        Ok(ProtocolRequest {
            device: self.device,
            generation: self.generation,
            interface: self.binding.interface(),
        })
    }
    /// Only the trusted transport may assert Success after the entire zero-data control transfer completes.
    /// A request is consumed once, including failure; retry needs a new transport generation.
    pub fn confirm_protocol(
        &mut self,
        r: ProtocolRequest,
        status: Completion,
    ) -> Result<(), Error> {
        if !self.pending
            || self.disconnected
            || r.device != self.device
            || r.generation != self.generation
            || r.interface != self.binding.interface()
        {
            return Err(Error::Stale);
        }
        self.pending = false;
        if status != Completion::Success {
            self.disconnected = true;
            self.clear();
            return Err(Error::Protocol);
        }
        self.confirmed = true;
        Ok(())
    }
    /// Any transport reset requires a strictly newer generation and a new successful protocol confirmation.
    pub fn reset(&mut self, generation: u64) -> Result<(), Error> {
        if generation <= self.generation {
            return Err(Error::Stale);
        }
        self.clear();
        self.generation = generation;
        self.pending = false;
        self.confirmed = false;
        self.disconnected = false;
        Ok(())
    }
    pub fn disconnect(&mut self) {
        self.clear();
        self.pending = false;
        self.confirmed = false;
        self.disconnected = true;
    }
    /// Reset is delivered out of band before queued events. Wrong owners cannot consume or clear it.
    pub fn take(&mut self, f: Focus) -> Result<Option<Event>, Error> {
        if self.focus != Some(f) {
            return Err(Error::Ownership);
        }
        if self.barrier {
            self.barrier = false;
            return Ok(Some(Event::Reset));
        }
        if self.count == 0 {
            return Ok(None);
        }
        let i = usize::from(self.head);
        let slot = self.queue.get_mut(i).ok_or(Error::Bounds)?;
        let e = *slot;
        *slot = EMPTY;
        self.head = ((i + 1) % QUEUE) as u8;
        self.count -= 1;
        Ok(Some(e))
    }
    fn enqueue(&mut self, events: &[Event]) -> Result<(), Error> {
        if events.len() > QUEUE - usize::from(self.count) {
            self.clear();
            return Err(Error::Overflow);
        }
        for &e in events {
            let i = (usize::from(self.head) + usize::from(self.count)) % QUEUE;
            self.queue[i] = e;
            self.count += 1;
        }
        Ok(())
    }
    /// Input is copied bytes from a completed interrupt-IN transfer, never a device/DMA pointer.
    /// dy is screen-oriented (positive down); the legacy PS/2 GUI adapter must NOT invert it again.
    pub fn report(&mut self, b: &[u8]) -> Result<(), Error> {
        if !self.confirmed || self.disconnected {
            return Err(Error::Protocol);
        }
        if self.focus.is_none() {
            self.clear();
            return Err(Error::Ownership);
        }
        let result = match self.binding.kind() {
            Kind::Keyboard => self.keyboard(b),
            Kind::Mouse => self.mouse(b),
        };
        if result == Err(Error::Report) {
            self.clear();
        }
        result
    }
    fn keyboard(&mut self, b: &[u8]) -> Result<(), Error> {
        if b.len() != 8 || b[1] != 0 {
            return Err(Error::Report);
        }
        let mut next = [0u8; 6];
        next.copy_from_slice(&b[2..8]);
        for i in 0..6 {
            let k = next[i];
            if k != 0 && (!(4..=0xdd).contains(&k) || next[..i].contains(&k)) {
                return Err(Error::Report);
            }
        }
        if !self.armed {
            if b[0] == 0 && next == [0; 6] {
                self.armed = true;
            }
            return Ok(());
        }
        let mut events = [EMPTY; 20];
        let mut n = 0;
        // Release before press; maximum is 8 modifier transitions plus six old and six new keys.
        for down in [false, true] {
            for bit in 0..8 {
                let mask = 1 << bit;
                if (self.modifiers ^ b[0]) & mask != 0 && (b[0] & mask != 0) == down {
                    *events.get_mut(n).ok_or(Error::Bounds)? = Event::Key {
                        usage: 0xe0 + bit,
                        down,
                    };
                    n += 1;
                }
            }
            let (from, other) = if down {
                (&next, &self.keys)
            } else {
                (&self.keys, &next)
            };
            for &usage in from {
                if usage != 0 && !other.contains(&usage) {
                    *events.get_mut(n).ok_or(Error::Bounds)? = Event::Key { usage, down };
                    n += 1;
                }
            }
        }
        self.enqueue(events.get(..n).ok_or(Error::Bounds)?)?;
        self.modifiers = b[0];
        self.keys = next;
        Ok(())
    }
    fn mouse(&mut self, b: &[u8]) -> Result<(), Error> {
        if b.len() < 3
            || b.len() > 8
            || b.len() > usize::from(self.binding.packet())
            || b[0] & !7 != 0
        {
            return Err(Error::Report);
        }
        let buttons = b[0];
        if !self.armed {
            if buttons == 0 {
                self.armed = true;
            }
            return Ok(());
        }
        let dx = i16::from(b[1] as i8);
        let dy = i16::from(b[2] as i8);
        if dx != 0 || dy != 0 || buttons != self.buttons {
            self.enqueue(&[Event::Pointer { dx, dy, buttons }])?;
        }
        self.buttons = buttons;
        Ok(())
    }
}
