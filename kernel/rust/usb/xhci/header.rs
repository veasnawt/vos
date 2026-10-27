//! Shared copied capability header and register layout, without profile policy.
//! USB.1 and the operational model deliberately qualify different profiles.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LayoutError {
    Alignment,
    Overflow,
    Bounds,
    InvalidCapability,
}
use LayoutError as Error;

#[derive(Clone, Copy, Debug)]
pub struct Header {
    pub first: u32,
    pub hcs1: u32,
    pub hcs2: u32,
    pub hcs3: u32,
    pub hcc1: u32,
    pub doorbell: u32,
    pub runtime: u32,
    pub operational: u32,
    pub version: u16,
    pub slots: u8,
    pub interrupters: u16,
    pub ports: u8,
}

impl Header {
    pub fn from_words(c: [u32; 7]) -> Self {
        Self {
            first: c[0],
            hcs1: c[1],
            hcs2: c[2],
            hcs3: c[3],
            hcc1: c[4],
            doorbell: c[5],
            runtime: c[6],
            operational: c[0] & 255,
            version: (c[0] >> 16) as u16,
            slots: c[1] as u8,
            interrupters: ((c[1] >> 8) & 0x7ff) as u16,
            ports: (c[1] >> 24) as u8,
        }
    }
    pub fn valid_structure(self, alignment: u32) -> bool {
        self.first != u32::MAX
            && self.first & 0xff00 == 0
            && self.operational >= 0x20
            && alignment != 0
            && self.operational % alignment == 0
            && self.slots != 0
            && self.interrupters != 0
            && self.ports != 0
    }
    pub fn layout_end(self, bound: u32) -> Result<u32, Error> {
        if self.doorbell % 4 != 0 || self.runtime % 32 != 0 {
            return Err(Error::Alignment);
        }
        let oe = end(self.operational, 0x400 + u32::from(self.ports) * 16, bound)?;
        let de = end(self.doorbell, (u32::from(self.slots) + 1) * 4, bound)?;
        let re = end(
            self.runtime,
            0x20 + u32::from(self.interrupters) * 0x20,
            bound,
        )?;
        if self.doorbell < oe || self.runtime < oe || overlaps(self.doorbell, de, self.runtime, re)
        {
            return Err(Error::InvalidCapability);
        }
        Ok(de.max(re))
    }
}

pub(super) fn end(first: u32, size: u32, bound: u32) -> Result<u32, Error> {
    let last = first.checked_add(size).ok_or(Error::Overflow)?;
    if last > bound {
        Err(Error::Bounds)
    } else {
        Ok(last)
    }
}
pub(super) fn overlaps(a: u32, ae: u32, b: u32, be: u32) -> bool {
    a < be && b < ae
}
