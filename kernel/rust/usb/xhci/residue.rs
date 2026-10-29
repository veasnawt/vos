//! USB.2-C3a firmware DMA residue model. Pure decisions over controller
//! register values and a platform that classifies frames, reserves free ones
//! and reads memory only inside frames it has reserved for this guard.
//!
//! Protect first, inspect second: every frame a firmware structure occupies is
//! claimed before any of its contents is read. Frames the boot already
//! excluded from the allocator as firmware memory (USB.2-C3a.3) are protected
//! for the whole boot and may be read without reserving them. Structures in
//! memory the guard cannot read (map-protected or untracked) are reported as
//! uninspected, never as safe. Two results are kept apart: reuse protection
//! (every discovered frame is unallocatable) and inspection completeness
//! (every structure was read and validated); neither ever makes firmware
//! memory releasable. Device contexts and scratchpads are protected but their transfer
//! rings and buffers are not bounded, so they leave the assessment incomplete.
use super::Error;

/// Firmware-residue reservations for the whole boot (registry capacity).
pub const RESERVE_LIMIT: usize = 16;
pub const INTERRUPTER_LIMIT: u8 = 16;
/// Command ring walk bound, in pages, and at most this many segments.
const RING_PAGES: u32 = 8;
const RING_SEGMENTS: u32 = 4;
const PAGE: u64 = 4096;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Role {
    Dcbaa = 1,
    CommandRing = 2,
    EventTable = 3,
    EventSegment = 4,
    DeviceContext = 5,
    Scratchpad = 6,
}

/// Platform classification of one page-aligned frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Page {
    /// Allocatable and not allocated: must be reserved before reuse.
    Free,
    /// Already reserved by this guard (duplicate claims are harmless).
    Owned,
    /// Already allocated by the kernel: a hazard, never reserved silently.
    Allocated,
    /// Never allocatable (map-protected): safe from reuse, not readable here.
    Protected,
    /// Beyond the allocator's range: safe from reuse, not readable here.
    Untracked,
    /// Excluded from the allocator at boot as firmware memory and verified to
    /// be RAM: safe from reuse for the whole boot and readable, never reserved.
    Excluded,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Interrupter {
    pub iman: u32,
    pub erstsz: u32,
    pub erstba: u64,
    pub erdp: u64,
}

pub trait Memory {
    fn page(&mut self, frame: u64) -> Page;
    /// Reserve a Free frame for the rest of the boot. `Err(Budget)` when the
    /// registry is full; any other error means the frame was not reserved.
    fn reserve(&mut self, frame: u64, role: Role) -> Result<(), Error>;
    /// Eight aligned bytes inside an Owned or Excluded frame.
    fn read64(&mut self, phys: u64) -> Result<u64, Error>;
    /// Runtime registers of interrupter `index` (< the inspected count).
    fn interrupter(&mut self, index: u8) -> Result<Interrupter, Error>;
}

/// Controller registers read once by the platform (no writes).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Registers {
    pub usbcmd: u32,
    pub usbsts: u32,
    pub crcr: u64,
    pub dcbaap: u64,
    pub config: u32,
    pub hcs1: u32,
    pub hcs2: u32,
    pub hcc1: u32,
    pub interrupters: u8,
    pub physical_bits: u8,
}

pub const RUNNING: u16 = 1 << 0;
pub const UNINSPECTED: u16 = 1 << 1;
pub const HAZARD: u16 = 1 << 2;
pub const EXHAUSTED: u16 = 1 << 3;
pub const MALFORMED: u16 = 1 << 4;
pub const DEVICES: u16 = 1 << 5;
pub const SCRATCHPADS: u16 = 1 << 6;
pub const RING_OPEN: u16 = 1 << 7;
pub const ERDP_OUTSIDE: u16 = 1 << 8;
pub const READ_FAILED: u16 = 1 << 9;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Outcome {
    pub findings: u16,
    pub reserved: u8,
    pub owned_again: u8,
    pub protected: u8,
    pub hazards: u8,
    pub pages: u8,
    /// Frames read in place because the boot excluded them (never reserved).
    pub excluded: u8,
    /// Bit i set: interrupter i has IMAN.IE set (input to the lifecycle plan).
    pub interrupter_enables: u16,
    /// Bit i set: interrupter i has IMAN.IP set (a pending interrupt; USB.2-C3b.1).
    pub interrupter_pending: u16,
    /// Order-sensitive digest of every memory word and interrupter register the
    /// walk read, so a revalidation can detect any change (USB.2-C3b.1).
    pub digest: u64,
}
impl Outcome {
    /// Every firmware structure bounded, protected and inspected.
    pub fn complete(&self) -> bool {
        self.findings == 0
    }
    pub fn halted(&self) -> bool {
        self.findings & RUNNING == 0
    }
    /// Every discovered frame is unallocatable: reserved by the guard,
    /// excluded at boot, map-protected or untracked. Says nothing about frames
    /// an incomplete walk could not discover, and never permits release.
    pub fn reuse_protected(&self) -> bool {
        self.findings & (HAZARD | EXHAUSTED) == 0
    }
    /// Every firmware structure was bounded, read and validated (controller
    /// state is reported separately by `halted`).
    pub fn inspection_complete(&self) -> bool {
        self.findings & !RUNNING == 0
    }
}

struct Walk<'a, M: Memory> {
    m: &'a mut M,
    out: Outcome,
    limit: u64,
}

/// FNV-1a style fold; detects drift between two reads, not an adversary.
pub fn fold(digest: u64, value: u64) -> u64 {
    (digest ^ value).wrapping_mul(0x0000_0100_0000_01b3).rotate_left(23)
}

impl<M: Memory> Walk<'_, M> {
    fn flag(&mut self, bit: u16) {
        self.out.findings |= bit;
    }
    /// Claim every frame of [start, start + len). True only if all of them
    /// are now owned by the guard and therefore safe to read.
    fn claim(&mut self, start: u64, len: u64, role: Role) -> bool {
        let Some(end) = start.checked_add(len) else {
            self.flag(MALFORMED);
            return false;
        };
        if len == 0 || end > self.limit {
            self.flag(MALFORMED);
            return false;
        }
        let mut readable = true;
        let mut frame = start & !(PAGE - 1);
        while frame < end {
            self.out.pages = self.out.pages.saturating_add(1);
            match self.m.page(frame) {
                Page::Owned => self.out.owned_again = self.out.owned_again.saturating_add(1),
                Page::Excluded => self.out.excluded = self.out.excluded.saturating_add(1),
                Page::Free => match self.m.reserve(frame, role) {
                    Ok(()) => self.out.reserved += 1,
                    Err(Error::Budget) => {
                        self.flag(EXHAUSTED);
                        readable = false;
                    }
                    Err(_) => {
                        self.flag(HAZARD);
                        self.out.hazards = self.out.hazards.saturating_add(1);
                        readable = false;
                    }
                },
                Page::Allocated => {
                    self.flag(HAZARD);
                    self.out.hazards = self.out.hazards.saturating_add(1);
                    readable = false;
                }
                Page::Protected | Page::Untracked => {
                    self.flag(UNINSPECTED);
                    self.out.protected = self.out.protected.saturating_add(1);
                    readable = false;
                }
            }
            frame += PAGE;
        }
        readable
    }
    fn read(&mut self, phys: u64) -> Option<u64> {
        match self.m.read64(phys) {
            Ok(v) => {
                self.out.digest = fold(self.out.digest, v ^ phys);
                Some(v)
            }
            Err(_) => {
                self.flag(READ_FAILED);
                None
            }
        }
    }

    fn dcbaa(&mut self, r: &Registers) {
        if r.dcbaap == 0 {
            return;
        }
        let slots = u64::from(r.config & 0xff);
        if r.dcbaap & 0x3f != 0 || slots > u64::from(r.hcs1 & 0xff) {
            self.flag(MALFORMED);
            return;
        }
        if !self.claim(r.dcbaap, (slots + 1) * 8, Role::Dcbaa) {
            return;
        }
        let scratchpads = u64::from(((r.hcs2 >> 27) & 31) | (((r.hcs2 >> 21) & 31) << 5));
        let Some(first) = self.read(r.dcbaap) else { return };
        if scratchpads != 0 {
            // Protect the array; the buffers it lists are not bounded here.
            self.flag(SCRATCHPADS);
            if first != 0 {
                self.claim(first & !0x3f, scratchpads * 8, Role::Scratchpad);
            }
        } else if first != 0 {
            self.flag(MALFORMED);
        }
        let context = if r.hcc1 & (1 << 2) != 0 { 2048 } else { 1024 };
        for slot in 1..=slots {
            let Some(entry) = self.read(r.dcbaap + slot * 8) else { return };
            if entry != 0 {
                // Transfer rings and data buffers behind it remain unbounded.
                self.flag(DEVICES);
                if entry & 0x3f != 0 {
                    self.flag(MALFORMED);
                } else {
                    self.claim(entry, context, Role::DeviceContext);
                }
            }
        }
    }

    fn command_ring(&mut self, r: &Registers) {
        let start = r.crcr & !0x3f;
        if start == 0 {
            if r.dcbaap != 0 {
                self.flag(RING_OPEN); // programmed controller, ring location unknown
            }
            return;
        }
        let (mut cursor, mut pages, mut segments) = (start, 0u32, 1u32);
        loop {
            if pages == RING_PAGES {
                self.flag(RING_OPEN); // not closed by a Link TRB within the bound
                return;
            }
            if !self.claim(cursor & !(PAGE - 1), PAGE, Role::CommandRing) {
                return; // claim already recorded why the page is unreadable
            }
            pages += 1;
            let page_end = (cursor & !(PAGE - 1)) + PAGE;
            while cursor < page_end {
                let Some(control) = self.read(cursor + 8).map(|v| (v >> 32) as u32) else { return };
                if (control >> 10) & 0x3f == 6 {
                    // Link TRB: closes the ring at its start or opens a segment.
                    // Bits 3:0 are reserved-zero: a nonzero value is malformed,
                    // never masked into an apparently valid pointer.
                    let Some(target) = self.read(cursor) else { return };
                    if target == start {
                        return;
                    }
                    segments += 1;
                    if target == 0 || target & 0xf != 0 || segments > RING_SEGMENTS {
                        self.flag(RING_OPEN);
                        return;
                    }
                    cursor = target;
                    break;
                }
                cursor += 16;
            }
        }
    }

    fn interrupters(&mut self, r: &Registers) {
        let count = r.interrupters.min(INTERRUPTER_LIMIT);
        let table_max = 1u64 << ((r.hcs2 >> 4) & 0xf);
        for i in 0..count {
            let Ok(it) = self.m.interrupter(i) else {
                self.flag(READ_FAILED);
                return;
            };
            if it.iman & 2 != 0 {
                self.out.interrupter_enables |= 1 << i;
            }
            if it.iman & 1 != 0 {
                self.out.interrupter_pending |= 1 << i;
            }
            for v in [u64::from(it.iman), u64::from(it.erstsz), it.erstba, it.erdp] {
                self.out.digest = fold(self.out.digest, v);
            }
            let size = u64::from(it.erstsz & 0xffff);
            if size == 0 {
                continue;
            }
            if size > table_max || it.erstba & 0x3f != 0 {
                self.flag(MALFORMED);
                continue;
            }
            if !self.claim(it.erstba, size * 16, Role::EventTable) {
                continue;
            }
            let erdp = it.erdp & !0xf;
            let mut inside = erdp == 0;
            for e in 0..size {
                let (Some(base), Some(word)) = (self.read(it.erstba + e * 16), self.read(it.erstba + e * 16 + 8))
                else {
                    return;
                };
                let trbs = word & 0xffff;
                if base & 0x3f != 0 || !(16..=4096).contains(&trbs) {
                    self.flag(MALFORMED);
                    continue;
                }
                self.claim(base, trbs * 16, Role::EventSegment);
                inside |= erdp >= base && erdp < base + trbs * 16;
            }
            if !inside {
                self.flag(ERDP_OUTSIDE);
            }
        }
    }
}

/// Assess and protect the firmware residue described by `r`. Never writes a
/// device register; the only effects are reservations through `m`.
pub fn assess<M: Memory>(m: &mut M, r: &Registers) -> Outcome {
    let bits = u32::from(r.physical_bits).clamp(32, 52);
    let mut w = Walk { m, out: Outcome::default(), limit: 1u64 << bits };
    if r.usbsts & 1 == 0 || r.usbcmd & 1 != 0 {
        w.flag(RUNNING); // live DMA cannot be bounded by a snapshot
    }
    w.dcbaa(r);
    w.command_ring(r);
    w.interrupters(r);
    w.out
}
