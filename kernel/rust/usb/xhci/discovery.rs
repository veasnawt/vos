//! USB.1 read-only decisions. No MMIO pointers, allocation or platform effects.
use super::header::Header;

pub const CAP_BYTES: usize = 28;
pub const LAYOUT_LIMIT: u32 = 64 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u32)]
pub enum Status {
    #[default]
    NotFound,
    Ok,
    Unsupported,
    Invalid,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Reason {
    #[default]
    Interface,
    Absent,
    Class,
    Endpoint,
    IoBar,
    LowBar,
    ReservedBar,
    Unassigned,
    Alignment,
    BarOverflow,
    PhysicalWidth,
    MemoryDisabled,
    Range,
    Mapping,
    InvalidMapping,
    Truncated,
    Structure,
    Version,
    OffsetAlignment,
    Layout,
    Inspected,
    RegisterOverflow,
    RegisterWidth,
}
impl Reason {
    pub fn c_string(self) -> &'static [u8] {
        match self {
            Self::Interface => b"invalid probe interface\0",
            Self::Absent => b"PCI function absent\0",
            Self::Class => b"PCI class is not USB xHCI\0",
            Self::Endpoint => b"invalid PCI endpoint header\0",
            Self::IoBar => b"I/O BAR unsupported\0",
            Self::LowBar => b"legacy below-1-MiB BAR unsupported\0",
            Self::ReservedBar => b"reserved memory BAR type\0",
            Self::Unassigned => b"unassigned memory BAR\0",
            Self::Alignment => b"BAR is not page aligned\0",
            Self::BarOverflow => b"BAR address overflow\0",
            Self::PhysicalWidth => b"BAR exceeds supported physical width\0",
            Self::MemoryDisabled => b"firmware left memory decoding disabled\0",
            Self::Range => b"device-memory range rejected\0",
            Self::Mapping => b"device-memory mapping failed\0",
            Self::InvalidMapping => b"invalid capability mapping\0",
            Self::Truncated => b"truncated capability header\0",
            Self::Structure => b"invalid capability structure\0",
            Self::Version => b"unsupported xHCI interface version\0",
            Self::OffsetAlignment => b"misaligned register offsets\0",
            Self::Layout => b"invalid or unsupported register layout\0",
            Self::Inspected => b"capabilities inspected; controller unchanged\0",
            Self::RegisterOverflow => b"register address overflow\0",
            Self::RegisterWidth => b"register layout exceeds address width\0",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Report {
    pub status: Status,
    pub reason: Reason,
    pub vendor: u16,
    pub device: u16,
    pub command: u16,
    pub pci_status: u16,
    pub version: u16,
    pub class_revision: u32,
    pub bar_low: u32,
    pub bar_high: u32,
    pub hcs1: u32,
    pub hcs2: u32,
    pub hcs3: u32,
    pub hcc1: u32,
    pub doorbell: u32,
    pub runtime: u32,
    pub layout_end: u32,
    pub base: u64,
    pub caplength: u32,
    pub slots: u32,
    pub interrupters: u32,
    pub ports: u32,
    pub address_bits: u32,
    pub mapping_error: i32,
}
impl Report {
    pub fn outcome(&mut self, status: Status, reason: Reason) -> Status {
        self.status = status;
        self.reason = reason;
        status
    }
}
pub fn class_matches(revision: u32) -> bool {
    revision >> 8 == 0x0c0330
}

pub fn decode_caps(words: Option<[u32; 7]>, r: &mut Report) -> Status {
    let Some(words) = words else {
        return r.outcome(Status::Invalid, Reason::Truncated);
    };
    let h = Header::from_words(words);
    r.caplength = h.operational;
    r.version = h.version;
    r.hcs1 = h.hcs1;
    r.hcs2 = h.hcs2;
    r.hcs3 = h.hcs3;
    r.hcc1 = h.hcc1;
    r.slots = u32::from(h.slots);
    r.interrupters = u32::from(h.interrupters);
    r.ports = u32::from(h.ports);
    r.doorbell = h.doorbell;
    r.runtime = h.runtime;
    if !h.valid_structure(4) {
        return r.outcome(Status::Invalid, Reason::Structure);
    }
    if !matches!(h.version, 0x100 | 0x110 | 0x120) {
        return r.outcome(Status::Unsupported, Reason::Version);
    }
    if h.doorbell & 3 != 0 || h.runtime & 31 != 0 {
        return r.outcome(Status::Invalid, Reason::OffsetAlignment);
    }
    match h.layout_end(LAYOUT_LIMIT) {
        Ok(end) => r.layout_end = end,
        Err(_) => return r.outcome(Status::Invalid, Reason::Layout),
    }
    r.outcome(Status::Ok, Reason::Inspected)
}

pub enum MapError {
    Mapping(i32),
    Invalid,
}
pub trait ProbeIo {
    fn config(&mut self, offset: u32) -> u32;
    fn check(&mut self, base: u64, bytes: u64) -> i32;
    fn snapshot(&mut self, base: u64) -> Result<[u32; 7], MapError>;
}
/// Reads at most seven PCI DWORDs, checks once and copies seven MMIO DWORDs once.
/// The trusted caller supplies callbacks and valid physical-width information.
#[inline(never)]
pub fn probe<B: ProbeIo>(io: &mut B, physical_bits: u32, r: &mut Report) -> Status {
    *r = Report::default();
    if !(32..=52).contains(&physical_bits) {
        r.outcome(Status::Invalid, Reason::Interface);
        return r.status;
    }
    let id = io.config(0);
    r.vendor = id as u16;
    r.device = (id >> 16) as u16;
    if r.vendor == 0xffff {
        r.outcome(Status::NotFound, Reason::Absent);
        return r.status;
    }
    r.class_revision = io.config(8);
    if !class_matches(r.class_revision) {
        r.outcome(Status::NotFound, Reason::Class);
        return r.status;
    }
    if r.vendor == 0 || (io.config(12) >> 16) & 0x7f != 0 {
        r.outcome(Status::Invalid, Reason::Endpoint);
        return r.status;
    }
    let cs = io.config(4);
    r.command = cs as u16;
    r.pci_status = (cs >> 16) as u16;
    r.bar_low = io.config(0x10);
    let bar_failure = if r.bar_low & 1 != 0 {
        Some((Status::Unsupported, Reason::IoBar))
    } else {
        match (r.bar_low >> 1) & 3 {
            1 => Some((Status::Unsupported, Reason::LowBar)),
            3 => Some((Status::Invalid, Reason::ReservedBar)),
            _ => None,
        }
    };
    if let Some((status, reason)) = bar_failure {
        r.outcome(status, reason);
        return r.status;
    }
    r.address_bits = if (r.bar_low >> 1) & 3 == 2 { 64 } else { 32 };
    if r.address_bits == 64 {
        r.bar_high = io.config(0x14);
    }
    r.base = (u64::from(r.bar_high) << 32) | u64::from(r.bar_low & !15);
    let failure = if r.base == 0 {
        Some((Status::Invalid, Reason::Unassigned))
    } else if r.base & 0xfff != 0 {
        Some((Status::Unsupported, Reason::Alignment))
    } else if r.base.checked_add(CAP_BYTES as u64).is_none() {
        Some((Status::Invalid, Reason::BarOverflow))
    } else if r.base + CAP_BYTES as u64 > 1u64 << physical_bits {
        Some((Status::Unsupported, Reason::PhysicalWidth))
    } else if r.command & 2 == 0 {
        Some((Status::Unsupported, Reason::MemoryDisabled))
    } else {
        None
    };
    if let Some((status, reason)) = failure {
        r.outcome(status, reason);
        return r.status;
    }
    r.mapping_error = io.check(r.base, CAP_BYTES as u64);
    if r.mapping_error != 0 {
        r.outcome(Status::Unsupported, Reason::Range);
        return r.status;
    }
    let words = match io.snapshot(r.base) {
        Ok(words) => words,
        Err(MapError::Mapping(error)) => {
            r.mapping_error = error;
            r.outcome(Status::Unsupported, Reason::Mapping);
            return r.status;
        }
        Err(MapError::Invalid) => {
            r.outcome(Status::Invalid, Reason::InvalidMapping);
            return r.status;
        }
    };
    if decode_caps(Some(words), r) != Status::Ok {
        return r.status;
    }
    let Some(last) = r.base.checked_add(u64::from(r.layout_end)) else {
        r.outcome(Status::Invalid, Reason::RegisterOverflow);
        return r.status;
    };
    if last > 1u64 << physical_bits || (r.address_bits == 32 && last > 1u64 << 32) {
        r.outcome(Status::Unsupported, Reason::RegisterWidth);
    }
    r.status
}
