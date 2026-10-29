//! xHCI 1.2b sections 5.3, 7.1, 7.2; initial operational profile is xHCI 1.0.
use super::header::{Header, end, overlaps};
impl From<super::header::LayoutError> for Error {
    fn from(error: super::header::LayoutError) -> Self {
        use super::header::LayoutError;
        match error {
            LayoutError::Alignment => Self::Alignment,
            LayoutError::Overflow => Self::Overflow,
            LayoutError::Bounds => Self::Bounds,
            LayoutError::InvalidCapability => Self::InvalidCapability,
        }
    }
}
use super::{BAR_BYTES, Error, PAGE};

/// Inputs from a future trusted platform qualifier, not application authority.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Facts {
    pub bdf: u32,
    pub vendor: u16,
    pub device: u16,
    pub bar_base: u64,
    pub bar_bytes: u32,
    pub bar_address_bits: u8,
    pub physical_bits: u8,
    pub candidates: u8,
    pub complete_scan: bool,
    pub memory_enabled: bool,
    pub d0: bool,
    pub identity_dma: bool,
    pub coherent_dma: bool,
    pub qualified_resource: bool,
    pub ram_disjoint: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Capabilities {
    pub bar_base: u64,
    pub operational: u32,
    pub doorbell: u32,
    pub runtime: u32,
    pub legacy: Option<u32>,
    pub slots: u8,
    pub ports: u8,
    pub interrupters: u16,
    pub ac64: bool,
}

fn word(bytes: &[u8], offset: u32) -> Result<u32, Error> {
    let first = usize::try_from(offset).map_err(|_| Error::Bounds)?;
    let last = first.checked_add(4).ok_or(Error::Overflow)?;
    let s = bytes.get(first..last).ok_or(Error::Truncated)?;
    Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

/// A bounded source of copied little-endian register words. The inspector
/// asks only for DWORD offsets it has proven lie inside the 16 KiB profile
/// extent; an implementer may refuse further words (budget, mapping state).
pub trait Words {
    fn word(&mut self, offset: u32) -> Result<u32, Error>;
}
/// Copied bytes, for the operational model and host tests.
pub struct Bytes<'a>(pub &'a [u8]);
impl Words for Bytes<'_> {
    fn word(&mut self, offset: u32) -> Result<u32, Error> {
        word(self.0, offset)
    }
}

/// USB Legacy Support capability (xHCI 1.2b 7.1), observed and never written.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Legacy {
    pub offset: u32,
    pub usblegsup: u32,
    pub usblegctlsts: u32,
}
impl Legacy {
    pub fn bios_owned(self) -> bool {
        self.usblegsup & (1 << 16) != 0
    }
    pub fn os_owned(self) -> bool {
        self.usblegsup & (1 << 24) != 0
    }
}

/// Structural and profile facts from copied registers; no platform facts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Inspection {
    pub header: [u32; 7],
    pub operational: u32,
    pub doorbell: u32,
    pub runtime: u32,
    pub pagesize: u32,
    pub legacy: Option<Legacy>,
    pub slots: u8,
    pub ports: u8,
    pub interrupters: u16,
    pub ac64: bool,
    pub extended: u8,
    pub protocol_ports: u32,
}

/// Inspect the initial profile (xHCI 1.0, <=16 ports, no scratchpads, 4 KiB
/// pages) and walk at most 64 extended capabilities inside the 16 KiB extent.
/// Each link is a nonzero forward DWORD offset, so offsets strictly increase:
/// a cycle is unrepresentable and the walk terminates within its bound.
/// Overlap with register blocks, truncation and duplicate legacy support fail.
/// One dynamically dispatched instance serves both copied bytes and the C1
/// register reader, keeping a single parser in the kernel image.
pub fn inspect(w: &mut dyn Words) -> Result<Inspection, Error> {
    let mut first = [0u32; 7];
    for (i, slot) in first.iter_mut().enumerate() {
        *slot = w.word(i as u32 * 4)?;
    }
    let header = Header::from_words(first);
    let operational = header.operational;
    if header.version != 0x100 {
        return Err(Error::Unsupported);
    }
    if !header.valid_structure(8) {
        return Err(Error::InvalidCapability);
    }
    let slots = header.slots;
    let interrupters = header.interrupters;
    let ports = header.ports;
    if ports > 16 {
        return Err(Error::Unsupported);
    }
    let hcs2 = header.hcs2;
    let scratchpads = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if scratchpads != 0 {
        return Err(Error::Unsupported);
    }
    let pagesize = w.word(operational.checked_add(8).ok_or(Error::Overflow)?)?;
    if pagesize & 1 == 0 {
        return Err(Error::Unsupported);
    }
    let hcc = header.hcc1;
    let doorbell = header.doorbell;
    let runtime = header.runtime;
    header.layout_end(BAR_BYTES)?;
    let oe = end(operational, 0x400 + u32::from(ports) * 16, BAR_BYTES)?;
    let de = end(doorbell, (u32::from(slots) + 1) * 4, BAR_BYTES)?;
    let re = end(runtime, 0x20 + u32::from(interrupters) * 0x20, BAR_BYTES)?;
    let mut offset = (hcc >> 16) * 4;
    let mut legacy = None;
    let mut protocol_ports = 0u32;
    let mut extended = 0u8;
    let done = |legacy, protocol_ports, extended| {
        if protocol_ports == 0 {
            return Err(Error::InvalidCapability);
        }
        Ok(Inspection {
            header: first,
            operational,
            doorbell,
            runtime,
            pagesize,
            legacy,
            slots,
            ports,
            interrupters,
            ac64: hcc & 1 != 0,
            extended,
            protocol_ports,
        })
    };
    for _ in 0..64 {
        if offset == 0 {
            return done(legacy, protocol_ports, extended);
        }
        if offset < 0x20 {
            return Err(Error::InvalidCapability);
        }
        let header = w.word(offset)?;
        extended += 1;
        let id = header & 0xff;
        if id == 0 {
            return Err(Error::InvalidCapability);
        }
        let next = ((header >> 8) & 0xff) * 4;
        let size = match id {
            1 => {
                if legacy.is_some() {
                    return Err(Error::InvalidCapability);
                }
                legacy = Some(Legacy {
                    offset,
                    usblegsup: header,
                    usblegctlsts: 0,
                });
                8
            }
            2 => {
                let major = header >> 24;
                let minor = (header >> 16) & 0xff;
                if !matches!(major, 2 | 3)
                    || minor & 15 > 9
                    || minor >> 4 > 9
                    || w.word(offset.checked_add(4).ok_or(Error::Overflow)?)? != 0x2042_5355
                {
                    return Err(Error::Unsupported);
                }
                let port = w.word(offset.checked_add(8).ok_or(Error::Overflow)?)?;
                let count = (port >> 8) & 0xff;
                let first_port = port & 0xff;
                if count == 0 || first_port == 0 || first_port + count - 1 > u32::from(ports) {
                    return Err(Error::InvalidCapability);
                }
                // Above bounds prove count <= 16 and shift/end representability.
                let mask = ((1u32 << count) - 1) << (first_port - 1);
                if protocol_ports & mask != 0 {
                    return Err(Error::InvalidCapability);
                }
                protocol_ports |= mask;
                16 + ((port >> 28) & 15) * 4
            }
            // Unknown capability bodies are not interpreted or dereferenced.
            _ => 4,
        };
        let last = end(offset, size, BAR_BYTES)?;
        if overlaps(offset, last, operational, oe)
            || overlaps(offset, last, doorbell, de)
            || overlaps(offset, last, runtime, re)
            || (next != 0 && next < size)
        {
            return Err(Error::InvalidCapability);
        }
        if let Some(l) = legacy.as_mut()
            && l.offset == offset
        {
            // Body read only after its extent is proven inside the profile.
            l.usblegctlsts = w.word(offset + 4)?;
        }
        if next == 0 {
            offset = 0;
        } else {
            offset = offset.checked_add(next).ok_or(Error::Overflow)?;
        }
    }
    // A 64th terminal entry is valid; a 65th traversal is not.
    if offset == 0 {
        done(legacy, protocol_ports, extended)
    } else {
        Err(Error::InvalidCapability)
    }
}

impl Capabilities {
    /// Parse copied bytes only. This function cannot access MMIO.
    pub fn parse(bytes: &[u8], facts: Facts) -> Result<Self, Error> {
        if facts.bdf & !0xff != 0
            || facts.vendor != 0x1b36
            || facts.device != 0x000d
            || facts.candidates != 1
            || !facts.complete_scan
            || !facts.memory_enabled
            || !facts.d0
            || !facts.identity_dma
            || !facts.coherent_dma
            || !facts.qualified_resource
            || !facts.ram_disjoint
            || facts.bar_bytes != BAR_BYTES
            || !(32..=52).contains(&facts.physical_bits)
            || !matches!(facts.bar_address_bits, 32 | 64)
        {
            return Err(Error::Unsupported);
        }
        if facts.bar_base == 0 || facts.bar_base % PAGE != 0 {
            return Err(Error::Alignment);
        }
        let bar_end = facts
            .bar_base
            .checked_add(u64::from(facts.bar_bytes))
            .ok_or(Error::Overflow)?;
        if bar_end > (1u64 << facts.physical_bits)
            || (facts.bar_address_bits == 32 && bar_end > (1u64 << 32))
        {
            return Err(Error::Bounds);
        }
        if bytes.len() < BAR_BYTES as usize {
            return Err(Error::Truncated);
        }
        let i = inspect(&mut Bytes(&bytes[..BAR_BYTES as usize]))?;
        Ok(Self {
            bar_base: facts.bar_base,
            operational: i.operational,
            doorbell: i.doorbell,
            runtime: i.runtime,
            legacy: i.legacy.map(|l| l.offset),
            slots: i.slots,
            ports: i.ports,
            interrupters: i.interrupters,
            ac64: i.ac64,
        })
    }
}
