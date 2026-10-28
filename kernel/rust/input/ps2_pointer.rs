//! Standard three-byte PS/2 decoding only. No device, clock or allocator access.
#![forbid(unsafe_code)]

#[repr(C)]
#[derive(Debug, Default)]
pub struct Decoder {
    pub bytes: [u8; 3],
    pub used: u8,
    pub idle: u8,
}

#[repr(C)]
#[derive(Debug, PartialEq, Eq)]
pub struct Packet {
    pub dx: i16,
    pub dy: i16,
    pub buttons: u8,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Step {
    Rejected,
    Pending,
    Complete(Packet),
}

impl Decoder {
    fn valid(&self) -> bool {
        self.used < 3 && (self.used == 0 || self.idle < 16)
    }

    fn discard(&mut self) {
        self.used = 0;
        self.idle = 0;
    }

    pub fn push(&mut self, byte: u8) -> Step {
        // C callers can overwrite the representation. Check before indexing;
        // malformed internal state is a lost packet, not a panic or OOB write.
        if !self.valid() {
            self.discard();
            return Step::Rejected;
        }
        self.idle = 0;
        if self.used == 0 && byte & 8 == 0 {
            return Step::Rejected;
        }
        self.bytes[usize::from(self.used)] = byte;
        self.used += 1;
        if self.used < 3 {
            return Step::Pending;
        }
        self.used = 0;
        let header = self.bytes[0];
        // Consume the whole overflowing packet, including data with bit 3 set.
        if header & 0xc0 != 0 {
            return Step::Rejected;
        }
        Step::Complete(Packet {
            dx: i16::from(self.bytes[1]) - if header & 0x10 != 0 { 256 } else { 0 },
            dy: i16::from(self.bytes[2]) - if header & 0x20 != 0 { 256 } else { 0 },
            buttons: header & 1,
        })
    }

    /// Poll count, not elapsed time. Preserve the existing sixteenth-poll expiry.
    pub fn poll_idle(&mut self) -> bool {
        if !self.valid() {
            self.discard();
            return true;
        }
        if self.used != 0 {
            self.idle += 1; // valid partial states have idle < 16
            if self.idle == 16 {
                self.discard();
                return true;
            }
        }
        false
    }
}
