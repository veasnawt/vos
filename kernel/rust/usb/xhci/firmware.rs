//! USB input bring-up, Increment 2: read-only firmware facts for the survey. Firmware independent: the Multiboot2
//! information block (the same on a Legacy BIOS and a UEFI boot, because GRUB builds it) supplies the memory map and the
//! ACPI RSDP; ACPI tables are read through a `Phys` reader that the platform bounds and keeps read-only.
//!
//! Four facts about DMA protection are kept apart on purpose, because they are different claims:
//!  1. an ACPI table that describes remapping hardware is *present* (DMAR on Intel, IVRS on AMD);
//!  2. a remapping unit's registers *respond*;
//!  3. translation is *enabled* in that unit (Intel GSTS.TES, AMD control IommuEn);
//!  4. the xHCI is *covered* by an enabled unit.
//! None of them is DMA isolation. Isolation would also need the unit's translation tables, which this survey does not read,
//! so the report always says "isolation NOT ESTABLISHED". No table or register value is ever trusted to size an allocation:
//! every length is bounded before use and every read can fail without consequence.

use super::survey::{Line, Mmio, Out};

pub const MAP_LIMIT: usize = 128;
pub const TAG_LIMIT: usize = 256;
pub const TABLE_LIMIT: u32 = 16 * 1024;
pub const ROOT_LIMIT: u32 = 1024;
pub const ENTRY_LIMIT: usize = 64;
pub const SIG_LIMIT: usize = 24;
pub const UNIT_LIMIT: usize = 4;
/// Intel and AMD remapping units are conventionally in the chipset's MMIO hole; anything else is not read.
pub const UNIT_WINDOW: (u64, u64) = (0xfe00_0000, 0xfeff_ffff);

/// Read-only copy of physical memory that the platform has already proven to be firmware-table memory.
pub trait Phys {
    fn read(&mut self, pa: u64, out: &mut [u8]) -> bool;
}

fn le32(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(<[u8; 4]>::try_from(b.get(o..o.checked_add(4)?)?).ok()?))
}
fn le64(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(<[u8; 8]>::try_from(b.get(o..o.checked_add(8)?)?).ok()?))
}

// ---- Multiboot2 information block ------------------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemKind {
    Available,
    Reserved,
    AcpiReclaim,
    AcpiNvs,
    Bad,
    Other,
    Unlisted,
}
impl MemKind {
    pub fn name(self) -> &'static str {
        match self {
            Self::Available => "ram",
            Self::Reserved => "reserved",
            Self::AcpiReclaim => "acpi-reclaim",
            Self::AcpiNvs => "acpi-nvs",
            Self::Bad => "bad",
            Self::Other => "other",
            Self::Unlisted => "unlisted",
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct MemMap {
    entries: [(u64, u64, u32); MAP_LIMIT],
    n: usize,
    pub truncated: bool,
}
impl MemMap {
    pub fn empty() -> Self {
        Self { entries: [(0, 0, 0); MAP_LIMIT], n: 0, truncated: false }
    }
    pub fn len(&self) -> usize {
        self.n
    }
    pub fn is_empty(&self) -> bool {
        self.n == 0
    }
    /// Type of the page containing `addr`. A page that any non-RAM entry claims is not RAM; overlapping entries never make
    /// reserved memory look available.
    pub fn kind(&self, addr: u64) -> MemKind {
        let mut found = MemKind::Unlisted;
        for &(base, len, ty) in self.entries.iter().take(self.n) {
            let end = base.saturating_add(len);
            if addr < base || addr >= end {
                continue;
            }
            let k = match ty {
                1 => MemKind::Available,
                2 => MemKind::Reserved,
                3 => MemKind::AcpiReclaim,
                4 => MemKind::AcpiNvs,
                5 => MemKind::Bad,
                _ => MemKind::Other,
            };
            if k != MemKind::Available {
                return k;
            }
            found = MemKind::Available;
        }
        found
    }
    /// Is `[pa, pa + len)` entirely firmware-table memory (reserved, ACPI reclaimable or ACPI NVS), never RAM or unlisted?
    pub fn firmware_range(&self, pa: u64, len: u64) -> bool {
        let Some(end) = pa.checked_add(len) else { return false };
        if len == 0 || len > (1 << 20) {
            return false;
        }
        let mut page = pa & !0xfff;
        while page < end {
            if !matches!(self.kind(page), MemKind::Reserved | MemKind::AcpiReclaim | MemKind::AcpiNvs) {
                return false;
            }
            page = match page.checked_add(0x1000) {
                Some(p) => p,
                None => return false,
            };
        }
        true
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rsdp {
    pub revision: u8,
    pub rsdt: u32,
    pub xsdt: u64,
}

/// Walk the tags of a Multiboot2 information block (total size, reserved, then 8-byte-aligned tags) for the memory map
/// (type 6) and the ACPI RSDP copies (type 14 old, 15 new). Malformed input yields less, never a panic.
pub fn parse_info(info: &[u8]) -> (MemMap, Result<Rsdp, &'static str>) {
    let mut map = MemMap::empty();
    let mut rsdp: Result<Rsdp, &'static str> = Err("no ACPI RSDP tag in the boot information");
    let Some(total) = le32(info, 0) else { return (map, Err("boot information block too small")) };
    let total = (total as usize).min(info.len());
    let mut off = 8usize;
    for _ in 0..TAG_LIMIT {
        let (Some(ty), Some(size)) = (le32(info, off), le32(info, off + 4)) else { break };
        let size = size as usize;
        if ty == 0 || size < 8 || off.checked_add(size).is_none_or(|e| e > total) {
            break;
        }
        let body = info.get(off + 8..off + size).unwrap_or(&[]);
        match ty {
            6 => {
                // entry_size, entry_version, then entries of (base, length, type, reserved)
                let es = le32(body, 0).unwrap_or(0) as usize;
                if es >= 24 && es <= 256 {
                    let mut at = 8usize;
                    while let (Some(base), Some(len), Some(t)) = (le64(body, at), le64(body, at + 8), le32(body, at + 16)) {
                        if let Some(slot) = map.entries.get_mut(map.n) {
                            *slot = (base, len, t);
                            map.n += 1;
                        } else {
                            map.truncated = true;
                            break;
                        }
                        at += es;
                    }
                }
            }
            14 | 15 => {
                if rsdp.is_err() || ty == 15 {
                    rsdp = parse_rsdp(body);
                }
            }
            _ => {}
        }
        off = match off.checked_add((size + 7) & !7) {
            Some(o) => o,
            None => break,
        };
    }
    (map, rsdp)
}

fn parse_rsdp(b: &[u8]) -> Result<Rsdp, &'static str> {
    if b.get(..8) != Some(b"RSD PTR ") {
        return Err("RSDP signature wrong");
    }
    let first = b.get(..20).ok_or("RSDP truncated")?;
    if first.iter().fold(0u8, |a, &x| a.wrapping_add(x)) != 0 {
        return Err("RSDP checksum wrong");
    }
    let revision = *b.get(15).ok_or("RSDP truncated")?;
    let rsdt = le32(b, 16).ok_or("RSDP truncated")?;
    let mut xsdt = 0;
    if revision >= 2 {
        let len = le32(b, 20).ok_or("RSDP truncated")? as usize;
        let whole = b.get(..len.clamp(36, 64)).ok_or("RSDP truncated")?;
        if whole.iter().fold(0u8, |a, &x| a.wrapping_add(x)) != 0 {
            return Err("RSDP extended checksum wrong");
        }
        xsdt = le64(b, 24).ok_or("RSDP truncated")?;
    }
    Ok(Rsdp { revision, rsdt, xsdt })
}

// ---- ACPI tables --------------------------------------------------------------------------------------------------------------

/// Sum of `len` bytes at `pa` read in small chunks (so no table-sized buffer is ever on the stack).
fn checksum<P: Phys>(p: &mut P, pa: u64, len: u32) -> Option<u8> {
    let mut sum = 0u8;
    let mut done = 0u32;
    let mut buf = [0u8; 128];
    while done < len {
        let n = (len - done).min(128) as usize;
        let chunk = buf.get_mut(..n)?;
        if !p.read(pa.checked_add(u64::from(done))?, chunk) {
            return None;
        }
        sum = chunk.iter().fold(sum, |a, &x| a.wrapping_add(x));
        done += n as u32;
    }
    Some(sum)
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Tables {
    pub root_is_xsdt: bool,
    pub root_addr: u64,
    /// `None`: the root table was read and checked, so the listed signatures are the whole story. `Some`: it was not.
    pub err: Option<&'static str>,
    pub count: u8,
    pub sigs: [[u8; 4]; SIG_LIMIT],
    pub dmar: Option<u64>,
    pub ivrs: Option<u64>,
    pub more_than_listed: bool,
}

fn header<P: Phys>(p: &mut P, pa: u64) -> Option<([u8; 4], u32)> {
    let mut h = [0u8; 36];
    if !p.read(pa, &mut h) {
        return None;
    }
    Some(([h[0], h[1], h[2], h[3]], le32(&h, 4)?))
}

pub fn walk_acpi<P: Phys>(p: &mut P, rsdp: &Rsdp) -> Tables {
    let mut t = Tables::default();
    let (root, entry, want) = if rsdp.revision >= 2 && rsdp.xsdt != 0 {
        t.root_is_xsdt = true;
        (rsdp.xsdt, 8usize, *b"XSDT")
    } else {
        (u64::from(rsdp.rsdt), 4usize, *b"RSDT")
    };
    t.root_addr = root;
    let Some((sig, len)) = header(p, root) else {
        t.err = Some("root table unreadable");
        return t;
    };
    if sig != want || len < 36 || len > ROOT_LIMIT {
        t.err = Some("root table malformed");
        return t;
    }
    if checksum(p, root, len) != Some(0) {
        t.err = Some("root table checksum wrong");
        return t;
    }
    let listed = ((len - 36) as usize).checked_div(entry).unwrap_or(0);
    let n = listed.min(ENTRY_LIMIT);
    t.more_than_listed = listed > ENTRY_LIMIT;
    for i in 0..n {
        let mut e = [0u8; 8];
        let Some(slot) = e.get_mut(..entry) else { break };
        if !p.read(root.saturating_add(36).saturating_add((i * entry) as u64), slot) {
            t.err = Some("root table entry unreadable");
            return t;
        }
        let addr = if entry == 8 { le64(&e, 0).unwrap_or(0) } else { u64::from(le32(&e, 0).unwrap_or(0)) };
        let Some((sig, _)) = header(p, addr) else {
            // an unreadable table is not an absent one
            t.err = Some("a listed table is unreadable");
            continue;
        };
        if let Some(s) = t.sigs.get_mut(usize::from(t.count)) {
            *s = sig;
            t.count += 1;
        } else {
            t.more_than_listed = true;
        }
        match &sig {
            b"DMAR" if t.dmar.is_none() => t.dmar = Some(addr),
            b"IVRS" if t.ivrs.is_none() => t.ivrs = Some(addr),
            _ => {}
        }
    }
    t
}

// ---- DMAR (Intel) and IVRS (AMD) ---------------------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Cover {
    #[default]
    No,
    /// A device scope of this unit names the xHCI exactly.
    Specific,
    /// The unit has INCLUDE_PCI_ALL and no other unit names the xHCI.
    PciAll,
    /// A scope the survey does not resolve (a bridge or a nested path) could contain the xHCI.
    Unknown,
}
impl Cover {
    pub fn name(self) -> &'static str {
        match self {
            Self::No => "no",
            Self::Specific => "yes (named)",
            Self::PciAll => "yes (all devices)",
            Self::Unknown => "unknown",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Unit {
    pub base: u64,
    pub flags: u8,
    pub segment: u16,
    pub cover: Cover,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Dmar {
    pub host_width: u8,
    pub flags: u8,
    pub units: [Unit; UNIT_LIMIT],
    pub n_units: u8,
    pub units_total: u8,
    pub rmrr: u8,
    /// An RMRR (memory the BIOS keeps using for a device) names the xHCI: firmware DMA the OS must not break.
    pub rmrr_names_xhci: bool,
    pub err: Option<&'static str>,
}

/// Where the xHCI is, for scope matching: (bus, device, function).
pub type Target = (u8, u8, u8);

fn read_at<P: Phys>(p: &mut P, pa: u64, buf: &mut [u8]) -> bool {
    p.read(pa, buf)
}

/// Device scopes of one DMAR remapping structure: does one name the target exactly, and is there one the survey cannot resolve?
fn scopes<P: Phys>(p: &mut P, mut at: u64, end: u64, target: Target) -> (bool, bool) {
    let (mut named, mut unknown) = (false, false);
    for _ in 0..16 {
        if at.saturating_add(6) > end {
            break;
        }
        let mut h = [0u8; 6];
        if !read_at(p, at, &mut h) {
            return (named, true);
        }
        let (ty, len, bus) = (h[0], u64::from(h[1]), h[5]);
        if len < 6 || at.saturating_add(len) > end {
            return (named, true);
        }
        let pairs = (len - 6) / 2;
        match (ty, pairs) {
            (1, 1) => {
                let mut path = [0u8; 2];
                if read_at(p, at.saturating_add(6), &mut path) {
                    named |= bus == target.0 && path[0] == target.1 && path[1] == target.2;
                } else {
                    unknown = true;
                }
            }
            (1, _) | (2, _) => unknown = true, // a bridge, or an endpoint behind one
            _ => {}
        }
        at = at.saturating_add(len);
    }
    (named, unknown)
}

pub fn parse_dmar<P: Phys>(p: &mut P, pa: u64, target: Target) -> Dmar {
    let mut d = Dmar::default();
    let Some((sig, len)) = header(p, pa) else {
        d.err = Some("DMAR unreadable");
        return d;
    };
    if &sig != b"DMAR" || !(48..=TABLE_LIMIT).contains(&len) {
        d.err = Some("DMAR malformed");
        return d;
    }
    if checksum(p, pa, len) != Some(0) {
        d.err = Some("DMAR checksum wrong");
        return d;
    }
    let mut hw = [0u8; 2];
    if !p.read(pa.saturating_add(36), &mut hw) {
        d.err = Some("DMAR unreadable");
        return d;
    }
    d.host_width = hw[0].wrapping_add(1);
    d.flags = hw[1];
    let end = pa.saturating_add(u64::from(len));
    let mut at = pa.saturating_add(48);
    let mut named_unit = None;
    for _ in 0..64 {
        if at.saturating_add(4) > end {
            break;
        }
        let mut h = [0u8; 4];
        if !read_at(p, at, &mut h) {
            d.err = Some("DMAR unreadable");
            return d;
        }
        let (ty, slen) = (u16::from_le_bytes([h[0], h[1]]), u64::from(u16::from_le_bytes([h[2], h[3]])));
        if slen < 4 || at.saturating_add(slen) > end {
            d.err = Some("DMAR structure length invalid");
            return d;
        }
        match ty {
            0 if slen >= 16 => {
                let mut b = [0u8; 12];
                if !read_at(p, at.saturating_add(4), &mut b) {
                    d.err = Some("DMAR unreadable");
                    return d;
                }
                let flags = b[0];
                let segment = u16::from_le_bytes([b[2], b[3]]);
                let base = le64(&b, 4).unwrap_or(0);
                let (named, unknown) = scopes(p, at.saturating_add(16), at.saturating_add(slen), target);
                let all = flags & 1 != 0;
                let cover = if named {
                    Cover::Specific
                } else if all {
                    Cover::PciAll
                } else if unknown {
                    Cover::Unknown
                } else {
                    Cover::No
                };
                if named && named_unit.is_none() {
                    named_unit = Some(d.units_total);
                }
                if let Some(u) = d.units.get_mut(usize::from(d.n_units)) {
                    *u = Unit { base, flags, segment, cover };
                    d.n_units += 1;
                }
                d.units_total = d.units_total.saturating_add(1);
            }
            1 if slen >= 24 => {
                d.rmrr = d.rmrr.saturating_add(1);
                let (named, _) = scopes(p, at.saturating_add(24), at.saturating_add(slen), target);
                d.rmrr_names_xhci |= named;
            }
            _ => {}
        }
        at = at.saturating_add(slen);
    }
    // A unit with INCLUDE_PCI_ALL covers every device no other unit names; a named unit takes precedence.
    if named_unit.is_some() {
        for u in d.units.iter_mut() {
            if u.cover == Cover::PciAll {
                u.cover = Cover::No;
            }
        }
    }
    d
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Ivrs {
    pub n_units: u8,
    pub bases: [u64; UNIT_LIMIT],
    pub segments: [u16; UNIT_LIMIT],
    pub units_total: u8,
    pub ivmd: u8,
    pub err: Option<&'static str>,
}

pub fn parse_ivrs<P: Phys>(p: &mut P, pa: u64) -> Ivrs {
    let mut v = Ivrs::default();
    let Some((sig, len)) = header(p, pa) else {
        v.err = Some("IVRS unreadable");
        return v;
    };
    if &sig != b"IVRS" || !(48..=TABLE_LIMIT).contains(&len) {
        v.err = Some("IVRS malformed");
        return v;
    }
    if checksum(p, pa, len) != Some(0) {
        v.err = Some("IVRS checksum wrong");
        return v;
    }
    let end = pa.saturating_add(u64::from(len));
    let mut at = pa.saturating_add(48);
    for _ in 0..64 {
        if at.saturating_add(4) > end {
            break;
        }
        let mut h = [0u8; 4];
        if !read_at(p, at, &mut h) {
            v.err = Some("IVRS unreadable");
            return v;
        }
        let (ty, slen) = (h[0], u64::from(u16::from_le_bytes([h[2], h[3]])));
        if slen < 4 || at.saturating_add(slen) > end {
            v.err = Some("IVRS structure length invalid");
            return v;
        }
        match ty {
            0x10 | 0x11 | 0x40 if slen >= 24 => {
                let mut b = [0u8; 12];
                if read_at(p, at.saturating_add(8), &mut b) {
                    let base = le64(&b, 0).unwrap_or(0);
                    let seg = u16::from_le_bytes([b[8], b[9]]);
                    if let (Some(bs), Some(sg)) = (v.bases.get_mut(usize::from(v.n_units)), v.segments.get_mut(usize::from(v.n_units))) {
                        *bs = base;
                        *sg = seg;
                        v.n_units += 1;
                    }
                    v.units_total = v.units_total.saturating_add(1);
                }
            }
            0x20..=0x22 => v.ivmd = v.ivmd.saturating_add(1),
            _ => {}
        }
        at = at.saturating_add(slen);
    }
    v
}

/// A remapping unit's registers, read once.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UnitRegs {
    pub responds: bool,
    pub version: u32,
    pub cap: u64,
    pub ecap: u64,
    /// Intel GSTS (bit 31 TES), or AMD control register low dword (bit 0 IommuEn).
    pub status: u32,
}
impl UnitRegs {
    pub fn intel_translation(&self) -> bool {
        self.responds && self.status >> 31 & 1 != 0
    }
    pub fn amd_enabled(&self) -> bool {
        self.responds && self.status & 1 != 0
    }
}

pub fn unit_base_ok(base: u64) -> bool {
    base % 0x1000 == 0 && base >= UNIT_WINDOW.0 && base <= UNIT_WINDOW.1
}

/// Intel VT-d: VER (0x00), CAP (0x08), ECAP (0x10), GSTS (0x1c). Only reads.
pub fn read_intel_unit<M: Mmio>(m: &mut M) -> UnitRegs {
    let mut u = UnitRegs::default();
    let (Some(ver), Some(c0), Some(c1), Some(e0), Some(e1), Some(gs)) =
        (m.read32(0), m.read32(8), m.read32(0xc), m.read32(0x10), m.read32(0x14), m.read32(0x1c))
    else {
        return u;
    };
    u.version = ver;
    u.cap = u64::from(c1) << 32 | u64::from(c0);
    u.ecap = u64::from(e1) << 32 | u64::from(e0);
    u.status = gs;
    u.responds = ver != 0 && ver != 0xffff_ffff;
    u
}

/// AMD IOMMU: the control register (0x18, low dword: bit 0 IommuEn) and the extended feature register (0x30). Only reads.
pub fn read_amd_unit<M: Mmio>(m: &mut M) -> UnitRegs {
    let mut u = UnitRegs::default();
    let (Some(ctl), Some(f0), Some(f1)) = (m.read32(0x18), m.read32(0x30), m.read32(0x34)) else { return u };
    u.status = ctl;
    u.cap = u64::from(f1) << 32 | u64::from(f0);
    u.responds = ctl != 0xffff_ffff;
    u.version = 1;
    u
}

// ---- the four facts, kept apart --------------------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Tri {
    Yes,
    No,
    #[default]
    Unknown,
}
impl Tri {
    pub fn name(self) -> &'static str {
        match self {
            Self::Yes => "yes",
            Self::No => "no",
            Self::Unknown => "unknown",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DmaFacts {
    /// 1: DMAR/IVRS present. `No` only when the root table was read and checked and lists neither.
    pub table_present: Tri,
    /// 2: at least one unit's registers answered; `No` when units were found and none answered; `Unknown` when none were read.
    pub hardware_responds: Tri,
    /// 3: translation enabled in at least one answering unit; `No` only when every found unit answered with it off.
    pub translation_enabled: Tri,
    /// 4: an enabled unit covers the xHCI.
    pub xhci_translated: Tri,
}

/// Combine what was found. Absence of evidence stays `Unknown`: no table does not prove no hardware, an unread unit does
/// not prove translation is off, and an enabled unit that does not cover the xHCI proves nothing for the xHCI.
pub fn dma_facts(tables: &Tables, units: &[(Cover, UnitRegs, bool)], found_units: u8) -> DmaFacts {
    let mut f = DmaFacts::default();
    f.table_present = if tables.dmar.is_some() || tables.ivrs.is_some() {
        Tri::Yes
    } else if tables.err.is_none() && !tables.more_than_listed {
        Tri::No
    } else {
        Tri::Unknown
    };
    if found_units == 0 {
        return f;
    }
    let read = units.len();
    let answering = units.iter().filter(|(_, r, _)| r.responds).count();
    f.hardware_responds = if answering > 0 {
        Tri::Yes
    } else if read == usize::from(found_units) {
        Tri::No
    } else {
        Tri::Unknown
    };
    let enabled = units.iter().filter(|(_, r, amd)| if *amd { r.amd_enabled() } else { r.intel_translation() }).count();
    f.translation_enabled = if enabled > 0 {
        Tri::Yes
    } else if answering == usize::from(found_units) {
        Tri::No
    } else {
        Tri::Unknown
    };
    let covered_enabled = units
        .iter()
        .filter(|(c, r, amd)| matches!(c, Cover::Specific | Cover::PciAll) && if *amd { r.amd_enabled() } else { r.intel_translation() })
        .count();
    let maybe = units.iter().any(|(c, _, _)| *c == Cover::Unknown);
    f.xhci_translated = if covered_enabled > 0 {
        Tri::Yes
    } else if answering == usize::from(found_units) && !maybe && units.iter().all(|(c, r, amd)| *c == Cover::No || !(if *amd { r.amd_enabled() } else { r.intel_translation() })) {
        Tri::No
    } else {
        Tri::Unknown
    };
    f
}

// ---- rendering ---------------------------------------------------------------------------------------------------------------
// Every line must fit `Line` (118 visible characters) because a longer one is silently cut: long facts are split over lines.

pub fn render_acpi(rsdp: &Result<Rsdp, &'static str>, t: Option<&Tables>, out: &mut dyn Out) {
    let mut l = Line::new();
    match rsdp {
        Err(why) => {
            l.s("usb: acpi: ").s(why).s(" (DMAR/IVRS not looked for)");
        }
        Ok(r) => {
            l.s("usb: acpi rsdp revision ").d(u64::from(r.revision)).s(" rsdt ").x(u64::from(r.rsdt), 8).s(" xsdt ").x(r.xsdt, 8);
            if let Some(t) = t {
                l.s(" root ").s(if t.root_is_xsdt { "xsdt" } else { "rsdt" }).s(" tables ").d(u64::from(t.count));
                if t.more_than_listed {
                    l.s("+");
                }
            }
        }
    }
    l.flush(out);
    if let Some(t) = t {
        if let Some(why) = t.err {
            l.s("usb:   acpi tables not fully read: ").s(why);
            l.flush(out);
        }
        for (i, s) in t.sigs.iter().take(usize::from(t.count)).enumerate() {
            if i % 12 == 0 {
                if i != 0 {
                    l.flush(out);
                }
                l.s("usb:   signatures");
            }
            l.s(" ");
            for &c in s {
                l.c(c);
            }
        }
        if t.count == 0 {
            l.s("usb:   signatures none");
        }
        l.flush(out);
    }
}

pub fn render_dmar(d: &Dmar, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: dmar host-width ").d(u64::from(d.host_width)).s(" flags ").x(u64::from(d.flags), 2).s(" units ").d(u64::from(d.units_total)).s(" rmrr ").d(u64::from(d.rmrr));
    if d.rmrr_names_xhci {
        l.s(" (an RMRR names the xhci: firmware DMA)");
    }
    l.flush(out);
    if let Some(why) = d.err {
        l.s("usb:   DMAR not fully read: ").s(why);
        l.flush(out);
    }
}

pub fn render_unit(i: usize, kind: &str, base: u64, flags: u8, cover: Option<Cover>, r: Option<&UnitRegs>, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb:   ").s(kind).s(" unit ").d(i as u64).s(" base ").x(base, 8).s(" flags ").x(u64::from(flags), 2);
    if let Some(c) = cover {
        l.s(" covers xhci: ").s(c.name());
    }
    l.flush(out);
    match r {
        None => {
            l.s("usb:     registers NOT read (outside the chipset window or unreadable)");
        }
        Some(r) if !r.responds => {
            l.s("usb:     registers do not answer");
        }
        Some(r) => {
            l.s("usb:     registers ver ").x(u64::from(r.version), 2).s(" status ").x(u64::from(r.status), 8);
            l.s(if kind == "amd" { if r.amd_enabled() { " translation ENABLED" } else { " translation off" } } else if r.intel_translation() { " translation ENABLED" } else { " translation off" });
        }
    }
    l.flush(out);
}

pub fn render_dma(f: &DmaFacts, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: dma-protection facts (separate claims): table present ").s(f.table_present.name()).s(", hardware answers ").s(f.hardware_responds.name());
    l.flush(out);
    l.s("usb:   translation enabled ").s(f.translation_enabled.name()).s(", xhci translated ").s(f.xhci_translated.name());
    l.flush(out);
    l.s("usb:   DMA isolation NOT ESTABLISHED: translation state is not a proof (translation tables are not read)");
    l.flush(out);
}
