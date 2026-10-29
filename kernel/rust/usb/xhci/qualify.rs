//! USB.2-C1 read-only PCI qualification for the QEMU xHCI profile.
//! Pure decisions over configuration words supplied by a trusted platform
//! reader; this module performs no access itself and grants no authority.
use super::BAR_BYTES;
use super::discovery::class_matches;

pub const VENDOR: u16 = 0x1b36;
pub const DEVICE: u16 = 0x000d;
/// At most 48 PCI capabilities are walked (offsets 0x40..=0xfc).
pub const CAPABILITY_LIMIT: usize = 48;

/// Configuration reads of one already-selected function. The reader rejects
/// unaligned or out-of-range offsets; it never exposes a write operation.
pub trait Config {
    fn read32(&mut self, offset: u8) -> Result<u32, Reason>;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Reason {
    Qualified = 0,
    NotFound = 1,
    Ambiguous = 2,
    IncompleteScan = 3,
    Interface = 4,
    Absent = 5,
    Class = 6,
    Header = 7,
    Profile = 8,
    IoBar = 9,
    LowBar = 10,
    ReservedBar = 11,
    Unassigned = 12,
    BarAlignment = 13,
    BarOverflow = 14,
    PhysicalWidth = 15,
    MemoryDisabled = 16,
    NoCapabilities = 17,
    CapabilityOffset = 18,
    CapabilityCycle = 19,
    CapabilityLimit = 20,
    DuplicateCapability = 21,
    PowerState = 22,
    MsixLayout = 23,
    Mapping = 24,
    Usb1Compatibility = 25,
    ControllerProfile = 26,
    ExtendedTruncated = 27,
    ExtendedInvalid = 28,
    RegisterLayout = 29,
    ReadBudget = 30,
    RegisterRead = 31,
    StateChanged = 32,
    Release = 33,
}
impl Reason {
    pub fn c_string(self) -> &'static [u8] {
        match self {
            // Short fixed strings: rodata counts against the C1 image budget.
            Self::Qualified => b"profile qualified\0",
            Self::NotFound => b"no xHCI on bus 0\0",
            Self::Ambiguous => b"ambiguous xHCI candidates\0",
            Self::IncompleteScan => b"incomplete bus scan\0",
            Self::Interface => b"invalid interface\0",
            Self::Absent => b"function absent\0",
            Self::Class => b"not xHCI class\0",
            Self::Header => b"not type-0 header\0",
            Self::Profile => b"unsupported identity\0",
            Self::IoBar => b"I/O BAR\0",
            Self::LowBar => b"below-1-MiB BAR\0",
            Self::ReservedBar => b"reserved BAR type\0",
            Self::Unassigned => b"unassigned BAR\0",
            Self::BarAlignment => b"BAR misaligned for 16 KiB\0",
            Self::BarOverflow => b"BAR overflow\0",
            Self::PhysicalWidth => b"BAR beyond address width\0",
            Self::MemoryDisabled => b"memory decoding off\0",
            Self::NoCapabilities => b"no capability list\0",
            Self::CapabilityOffset => b"bad capability offset\0",
            Self::CapabilityCycle => b"capability cycle\0",
            Self::CapabilityLimit => b"over 48 capabilities\0",
            Self::DuplicateCapability => b"duplicate capability\0",
            Self::PowerState => b"not in D0\0",
            Self::MsixLayout => b"MSI-X outside BAR\0",
            Self::Mapping => b"BAR lease refused\0",
            Self::Usb1Compatibility => b"USB.1 decode rejects header\0",
            Self::ControllerProfile => b"unsupported register profile\0",
            Self::ExtendedTruncated => b"truncated capabilities\0",
            Self::ExtendedInvalid => b"malformed capabilities\0",
            Self::RegisterLayout => b"bad register layout\0",
            Self::ReadBudget => b"read budget exhausted\0",
            Self::RegisterRead => b"read refused\0",
            Self::StateChanged => b"state changed\0",
            Self::Release => b"lease retained\0",
        }
    }
}
impl From<super::Error> for Reason {
    fn from(error: super::Error) -> Self {
        use super::Error;
        match error {
            Error::Truncated => Self::ExtendedTruncated,
            Error::InvalidCapability => Self::ExtendedInvalid,
            Error::Unsupported => Self::ControllerProfile,
            Error::Overflow | Error::Bounds | Error::Alignment => Self::RegisterLayout,
            Error::Budget => Self::ReadBudget,
            _ => Self::RegisterRead,
        }
    }
}

/// Mutable configuration state compared before and after qualification.
/// PCI status is excluded: hardware may set its RW1C bits independently.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct State {
    pub command: u16,
    pub bar_low: u32,
    pub bar_high: u32,
    pub pmcsr: u16,
    pub msi_control: u16,
    pub msix_control: u16,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Pci {
    pub vendor: u16,
    pub device: u16,
    pub class_revision: u32,
    pub status: u16,
    pub base: u64,
    pub bar_bits: u8,
    pub capabilities: u8,
    pub pm: u8,
    pub power_state: u8,
    pub msi: u8,
    pub msix: u8,
    /// PCI Express capability offset (0 if absent); read-only, for Transactions Pending.
    pub pcie: u8,
    pub msix_table: u32,
    pub msix_pba: u32,
    pub state: State,
}

fn read<C: Config>(c: &mut C, offset: u32) -> Result<u32, Reason> {
    match u8::try_from(offset) {
        Ok(o) if o % 4 == 0 => c.read32(o),
        _ => Err(Reason::CapabilityOffset),
    }
}

/// Candidate count and completeness come from the platform's full bus scan.
pub fn select(candidates: u32, complete: bool) -> Result<(), Reason> {
    match (candidates, complete) {
        (_, false) => Err(Reason::IncompleteScan),
        (0, _) => Err(Reason::NotFound),
        (1, _) => Ok(()),
        _ => Err(Reason::Ambiguous),
    }
}

/// Identity, BAR form/extent, memory decoding and a bounded capability walk.
/// A function without a PM capability is in D0 by PCI PM definition.
pub fn inspect_pci<C: Config>(c: &mut C, physical_bits: u32) -> Result<Pci, Reason> {
    if !(32..=52).contains(&physical_bits) {
        return Err(Reason::Interface);
    }
    let mut p = Pci::default();
    let id = read(c, 0)?;
    p.vendor = id as u16;
    p.device = (id >> 16) as u16;
    if p.vendor == 0xffff {
        return Err(Reason::Absent);
    }
    p.class_revision = read(c, 8)?;
    if !class_matches(p.class_revision) {
        return Err(Reason::Class);
    }
    if p.vendor == 0 || (read(c, 12)? >> 16) & 0x7f != 0 {
        return Err(Reason::Header);
    }
    if p.vendor != VENDOR || p.device != DEVICE {
        return Err(Reason::Profile);
    }
    let cs = read(c, 4)?;
    p.state.command = cs as u16;
    p.status = (cs >> 16) as u16;
    p.state.bar_low = read(c, 0x10)?;
    let low = p.state.bar_low;
    if low & 1 != 0 {
        return Err(Reason::IoBar);
    }
    match (low >> 1) & 3 {
        1 => return Err(Reason::LowBar),
        3 => return Err(Reason::ReservedBar),
        2 => {
            p.bar_bits = 64;
            p.state.bar_high = read(c, 0x14)?;
        }
        _ => p.bar_bits = 32,
    }
    p.base = (u64::from(p.state.bar_high) << 32) | u64::from(low & !15);
    let bytes = u64::from(BAR_BYTES);
    if p.base == 0 {
        return Err(Reason::Unassigned);
    }
    if p.base % bytes != 0 {
        return Err(Reason::BarAlignment);
    }
    let end = p.base.checked_add(bytes).ok_or(Reason::BarOverflow)?;
    if end > 1u64 << physical_bits || (p.bar_bits == 32 && end > 1u64 << 32) {
        return Err(Reason::PhysicalWidth);
    }
    if p.state.command & 2 == 0 {
        return Err(Reason::MemoryDisabled);
    }
    if p.status & (1 << 4) == 0 {
        return Err(Reason::NoCapabilities);
    }
    walk(c, &mut p)?;
    check_profile(&p)?;
    Ok(p)
}

fn walk<C: Config>(c: &mut C, p: &mut Pci) -> Result<(), Reason> {
    let mut offset = read(c, 0x34)? & 0xff;
    let mut seen = 0u64; // one bit per configuration DWORD
    for _ in 0..CAPABILITY_LIMIT {
        if offset == 0 {
            return Ok(());
        }
        if !(0x40..=0xfc).contains(&offset) || offset % 4 != 0 {
            return Err(Reason::CapabilityOffset);
        }
        let bit = 1u64 << (offset / 4);
        if seen & bit != 0 {
            return Err(Reason::CapabilityCycle);
        }
        seen |= bit;
        p.capabilities += 1;
        let header = read(c, offset)?;
        let at = offset as u8; // proven <= 0xfc above
        match header & 0xff {
            0x01 => {
                if p.pm != 0 {
                    return Err(Reason::DuplicateCapability);
                }
                p.pm = at;
                p.state.pmcsr = read(c, offset + 4)? as u16;
                p.power_state = (p.state.pmcsr & 3) as u8;
            }
            0x05 => {
                if p.msi != 0 {
                    return Err(Reason::DuplicateCapability);
                }
                p.msi = at;
                p.state.msi_control = (header >> 16) as u16;
            }
            0x10 => {
                if p.pcie != 0 {
                    return Err(Reason::DuplicateCapability);
                }
                p.pcie = at;
            }
            0x11 => {
                if p.msix != 0 {
                    return Err(Reason::DuplicateCapability);
                }
                p.msix = at;
                p.state.msix_control = (header >> 16) as u16;
                p.msix_table = read(c, offset + 4)?;
                p.msix_pba = read(c, offset + 8)?;
            }
            _ => {}
        }
        offset = (header >> 8) & 0xff;
    }
    if offset != 0 {
        return Err(Reason::CapabilityLimit);
    }
    Ok(())
}

/// Profile checks over a completed walk: D0, and MSI-X structures inside the
/// fixed BAR. MSI-X placement is consistency evidence for the 16 KiB extent.
pub fn check_profile(p: &Pci) -> Result<(), Reason> {
    if p.pm != 0 && p.power_state != 0 {
        return Err(Reason::PowerState);
    }
    if p.msix != 0 {
        let entries = u32::from(p.state.msix_control & 0x7ff) + 1;
        let table_end = (p.msix_table & !7).checked_add(entries * 16);
        let pba_end = (p.msix_pba & !7).checked_add(entries.div_ceil(64) * 8);
        let inside = |end: Option<u32>| end.is_some_and(|e| e <= BAR_BYTES);
        if p.msix_table & 7 != 0 || p.msix_pba & 7 != 0 || !inside(table_end) || !inside(pba_end) {
            return Err(Reason::MsixLayout);
        }
    }
    Ok(())
}

/// Reread exactly the configuration words that define `State`.
pub fn observe<C: Config>(c: &mut C, p: &Pci) -> Result<State, Reason> {
    let mut s = State {
        command: read(c, 4)? as u16,
        bar_low: read(c, 0x10)?,
        ..State::default()
    };
    if p.bar_bits == 64 {
        s.bar_high = read(c, 0x14)?;
    }
    if p.pm != 0 {
        s.pmcsr = read(c, u32::from(p.pm) + 4)? as u16;
    }
    if p.msi != 0 {
        s.msi_control = (read(c, u32::from(p.msi))? >> 16) as u16;
    }
    if p.msix != 0 {
        s.msix_control = (read(c, u32::from(p.msix))? >> 16) as u16;
    }
    Ok(s)
}
