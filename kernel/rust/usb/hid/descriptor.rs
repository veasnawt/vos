//! Strict subset: one configuration, at most eight alt-zero interfaces, sixteen endpoints,
//! one boot keyboard and one boot mouse. Unbound interfaces are validated, never activated.
use crate::Error;
pub const MAX_CONFIGURATION: usize = 4096;
pub const MAX_INTERFACES: u8 = 8;
pub const MAX_ENDPOINTS: u8 = 16;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Speed {
    Low,
    Full,
    High,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Keyboard,
    Mouse,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Device {
    speed: Speed,
    pub vendor: u16,
    pub product: u16,
    pub ep0_packet: u8,
}
fn word(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}
impl Device {
    pub fn parse(b: &[u8], speed: Speed) -> Result<Self, Error> {
        if b.len() != 18 || b[0] != 18 || b[1] != 1 {
            return Err(Error::Descriptor);
        }
        if !matches!(word(b, 2), 0x0100 | 0x0110 | 0x0200) || b[17] != 1 {
            return Err(Error::Unsupported);
        }
        let packet = b[7];
        if !match speed {
            Speed::Low => packet == 8,
            Speed::Full => matches!(packet, 8 | 16 | 32 | 64),
            Speed::High => packet == 64 && word(b, 2) == 0x0200,
        } {
            return Err(Error::Descriptor);
        }
        // Per-interface class devices and the standard IAD composite class only.
        if !((b[4] == 0 && b[5] == 0 && b[6] == 0) || (b[4] == 0xef && b[5] == 2 && b[6] == 1)) {
            return Err(Error::Unsupported);
        }
        Ok(Self {
            speed,
            vendor: word(b, 8),
            product: word(b, 10),
            ep0_packet: packet,
        })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Binding {
    interface: u8,
    endpoint: u8,
    packet: u16,
    interval: u8,
    kind: Kind,
    configuration: u8,
}
impl Binding {
    pub fn interface(self) -> u8 {
        self.interface
    }
    pub fn endpoint(self) -> u8 {
        self.endpoint
    }
    pub fn packet(self) -> u16 {
        self.packet
    }
    pub fn interval(self) -> u8 {
        self.interval
    }
    pub fn kind(self) -> Kind {
        self.kind
    }
    pub fn configuration(self) -> u8 {
        self.configuration
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Selection {
    pub keyboard: Option<Binding>,
    pub mouse: Option<Binding>,
}
#[derive(Clone, Copy)]
struct Interface {
    number: u8,
    expected: u8,
    seen: u8,
    kind: Option<Kind>,
    hid: bool,
    bound: bool,
}
fn finish(i: Option<Interface>) -> Result<(), Error> {
    if let Some(i) = i {
        if i.expected != i.seen || (i.kind.is_some() && (!i.hid || !i.bound)) {
            return Err(Error::Descriptor);
        }
    }
    Ok(())
}
impl Selection {
    pub fn parse(device: Device, b: &[u8]) -> Result<Self, Error> {
        if b.len() < 9
            || b.len() > MAX_CONFIGURATION
            || b[0] != 9
            || b[1] != 2
            || usize::from(word(b, 2)) != b.len()
            || b[4] == 0
            || b[4] > MAX_INTERFACES
            || b[5] == 0
            || b[7] & 0x9f != 0x80
            || b[8] > 250
        {
            return Err(Error::Descriptor);
        }
        let mut result = Self::default();
        let mut pos = 9;
        let mut current = None;
        let mut interfaces = 0u8;
        let mut numbers = 0u8;
        let mut endpoints = 0u32;
        let mut endpoint_count = 0u8;
        let mut iad_mask = 0u8;
        while pos < b.len() {
            let rest = &b[pos..];
            if rest.len() < 2 || rest[0] < 2 || usize::from(rest[0]) > rest.len() {
                return Err(Error::Descriptor);
            }
            let d = &rest[..usize::from(rest[0])];
            pos += d.len();
            match d[1] {
                1 | 2 => return Err(Error::Descriptor),
                4 => {
                    finish(current)?;
                    if d.len() != 9 || d[2] >= b[4] || d[3] != 0 || d[4] > MAX_ENDPOINTS {
                        return Err(Error::Unsupported);
                    }
                    let bit = 1u8 << d[2];
                    if numbers & bit != 0 {
                        return Err(Error::Ambiguous);
                    }
                    numbers |= bit;
                    interfaces += 1;
                    let kind = if d[5] == 3 && d[6] == 1 {
                        match d[7] {
                            1 => Some(Kind::Keyboard),
                            2 => Some(Kind::Mouse),
                            _ => return Err(Error::Unsupported),
                        }
                    } else {
                        None
                    };
                    if kind.is_some() && d[4] != 1 {
                        return Err(Error::Unsupported);
                    }
                    current = Some(Interface {
                        number: d[2],
                        expected: d[4],
                        seen: 0,
                        kind,
                        hid: false,
                        bound: false,
                    });
                }
                5 => {
                    let i = current.as_mut().ok_or(Error::Descriptor)?;
                    if d.len() != 7 || d[2] & 0x70 != 0 || d[2] & 15 == 0 || d[3] & 0xfc != 0 {
                        return Err(Error::Descriptor);
                    }
                    let typ = d[3] & 3;
                    let packet = word(d, 4);
                    if !matches!(typ, 2 | 3) {
                        return Err(Error::Unsupported);
                    }
                    let limit = match (device.speed, typ) {
                        (Speed::Low, 3) => 8,
                        (Speed::Low, _) => return Err(Error::Unsupported),
                        (Speed::Full, _) => 64,
                        (Speed::High, 2) => 512,
                        (Speed::High, _) => 1024,
                    };
                    if packet == 0
                        || packet > limit
                        || (typ == 2
                            && match device.speed {
                                Speed::Full => !matches!(packet, 8 | 16 | 32 | 64),
                                Speed::High => packet != 512,
                                Speed::Low => true,
                            })
                    {
                        return Err(Error::Descriptor);
                    }
                    if typ == 3
                        && (d[6] == 0
                            || (device.speed == Speed::Low && d[6] < 10)
                            || (device.speed == Speed::High && d[6] > 16))
                    {
                        return Err(Error::Descriptor);
                    }
                    let bit = 1u32 << ((d[2] & 15) + if d[2] & 0x80 != 0 { 16 } else { 0 });
                    if endpoints & bit != 0 {
                        return Err(Error::Ambiguous);
                    }
                    endpoints |= bit;
                    endpoint_count += 1;
                    if endpoint_count > MAX_ENDPOINTS {
                        return Err(Error::Bounds);
                    }
                    i.seen += 1;
                    if i.seen > i.expected {
                        return Err(Error::Descriptor);
                    }
                    if let Some(kind) = i.kind {
                        if !i.hid
                            || typ != 3
                            || d[2] & 0x80 == 0
                            || packet > 64
                            || packet
                                < match kind {
                                    Kind::Keyboard => 8,
                                    Kind::Mouse => 3,
                                }
                        {
                            return Err(Error::Unsupported);
                        }
                        let slot = match kind {
                            Kind::Keyboard => &mut result.keyboard,
                            Kind::Mouse => &mut result.mouse,
                        };
                        if slot.is_some() {
                            return Err(Error::Ambiguous);
                        }
                        *slot = Some(Binding {
                            interface: i.number,
                            endpoint: d[2],
                            packet,
                            interval: d[6],
                            kind,
                            configuration: b[5],
                        });
                        i.bound = true;
                    }
                }
                0x21 => {
                    let i = current.as_mut().ok_or(Error::Descriptor)?;
                    if i.hid
                        || i.seen != 0
                        || d.len() != 9
                        || d[5] != 1
                        || d[6] != 0x22
                        || word(d, 7) == 0
                        || usize::from(word(d, 7)) > MAX_CONFIGURATION
                    {
                        return Err(Error::Descriptor);
                    }
                    i.hid = true;
                }
                0x0b => {
                    // Nonoverlapping, bounded Interface Association Descriptor; all named interfaces must exist.
                    if d.len() != 8 || d[3] == 0 || d[2] >= 8 || d[3] > 8 - d[2] {
                        return Err(Error::Descriptor);
                    }
                    let mask = (((1u16 << d[3]) - 1) << d[2]) as u8;
                    if iad_mask & mask != 0 || numbers & mask != 0 {
                        return Err(Error::Ambiguous);
                    }
                    iad_mask |= mask;
                }
                // Other class-specific data may be skipped only as bounded opaque descriptors.
                0x24 | 0x25 => {
                    if current.is_none() {
                        return Err(Error::Descriptor);
                    }
                }
                _ => return Err(Error::Unsupported),
            }
        }
        finish(current)?;
        if interfaces != b[4] || iad_mask & !numbers != 0 {
            return Err(Error::Descriptor);
        }
        if result.keyboard.is_none() && result.mouse.is_none() {
            return Err(Error::Unsupported);
        }
        Ok(result)
    }
}
