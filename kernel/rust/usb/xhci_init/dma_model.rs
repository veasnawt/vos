//! USB.2-C4.2a model-only DMA ownership ledger. No memory, no device and no address is touched: it records
//! which logical pages exist, who may touch them and when they may be given back.
//!
//! Distinction the type system encodes: this ledger is *software ownership*, a record that keeps VOS from
//! reusing, freeing or writing pages a device may be using. It is not hardware isolation: with bus mastering
//! on and no IOMMU a device can still reach any physical address regardless of this ledger.
//!
//! Scratchpad policy (resolved against xHCI 1.2, May 2019, in the Increment 1 report): the normative definition is
//! HCSPARAMS2 "Max Scratchpad Buffers" = Hi:Lo, "valid values 0-1023" (section 5.3.4), and the Scratchpad Buffer
//! Array "size is defined by the Max Scratchpad Buffers field" (section 6.6). Table 6-1's 248-byte maximum (31
//! entries) is the pre-1.1 five-bit limit that the Hi field superseded; 31 is therefore NOT a hardware limit.
//! Two separate limits remain, and they are not the same thing:
//!  - `SPEC_MAX_SCRATCHPADS` (1,023): a larger count cannot be encoded and is invalid.
//!  - `SCRATCHPAD_BUDGET` (64): a VOS resource decision. A controller that asks for more is refused with its own
//!    reason (`scratchpads-exceed-budget`), not treated as broken; raising the budget is a reviewed change.

use crate::backend::named;
use crate::types::{PageKind, ReleasePolicy};

/// Largest Max Scratchpad Buffers value xHCI 1.2 section 5.3.4 allows (Hi:Lo, ten bits).
pub const SPEC_MAX_SCRATCHPADS: u16 = 1023;
/// VOS resource budget for scratchpad buffers. 64 entries are 512 bytes, so the array fits one 4 KiB page, which
/// also satisfies the PAGESIZE boundary Table 6-1 gives the array.
pub const SCRATCHPAD_BUDGET: u16 = 64;
pub const DMA_PAGE: u64 = 4096;
pub const DMA_MIN_ADDR: u64 = 0x10_0000;
const CORE: usize = 4;
const ARRAY: usize = 4;
const BUF0: usize = 5;
pub const MAX_RECORDS: usize = BUF0 + SCRATCHPAD_BUDGET as usize;

/// Authority to publish a page. Only a session whose machine holds the page as published can mint one.
#[derive(Debug)]
pub struct PublishProof(());
impl PublishProof {
    pub(crate) fn new() -> Self {
        Self(())
    }
}
/// Authority to treat published pages as quiesced. Only a session whose machine reached `Quiesced` can mint one.
#[derive(Debug)]
pub struct QuiescenceProof(());
impl QuiescenceProof {
    pub(crate) fn new() -> Self {
        Self(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Item {
    Core(PageKind),
    Array,
    Buffer(u8),
}

named! {
    pub enum DState {
        Free => "free",
        Allocated => "allocated",
        Zeroed => "zeroed",
        Prepared => "prepared",
        Published => "published",
        /// Published scratchpad pages: the device owns them; software must not read or write them (spec 4.20).
        DeviceOwned => "device-owned",
        Quiesced => "quiesced",
        Quarantined => "quarantined",
        Retained => "retained",
    }
}

named! {
    pub enum DReason {
        ScratchpadsInvalid => "scratchpads-invalid",
        ScratchpadsExceedBudget => "scratchpads-exceed-budget",
        BadLimit => "bad-limit",
        NoSuchItem => "no-such-item",
        AlreadyOwned => "already-owned",
        NotOwned => "not-owned",
        BadState => "bad-state",
        Misaligned => "misaligned",
        AddressRange => "address-range",
        Overlap => "overlap",
        DependencyNotReady => "dependency-not-ready",
        ScratchpadIncomplete => "scratchpad-incomplete",
        CpuTouchOfDeviceOwned => "cpu-touch-of-device-owned",
        StillPublished => "still-published",
        ReleaseForbiddenByPolicy => "release-forbidden-by-policy",
        Quarantined => "quarantined",
        QuarantineExhausted => "quarantine-exhausted",
        PoolClosed => "pool-closed",
        NothingToQuiesce => "nothing-to-quiesce",
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PoolSpec {
    /// Exclusive upper bound of device-addressable memory.
    pub device_limit: u64,
    /// The controller implements 64-bit addresses; otherwise pages must end below 4 GiB.
    pub ac64: bool,
    pub scratchpads: u16,
    /// How many records the quarantine can hold before the pool closes.
    pub quarantine_cap: u8,
    pub release: ReleasePolicy,
}

/// States and addresses live in two arrays so that the ledger stays near 600 bytes (a `(state, addr)` pair would pad
/// to sixteen bytes per record and double it); the ledger is embedded in the session and copied by value at creation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Ledger {
    spec: PoolSpec,
    st: [DState; MAX_RECORDS],
    ad: [u64; MAX_RECORDS],
    quarantined: u8,
    closed: bool,
}

impl Ledger {
    pub fn new(spec: PoolSpec) -> Result<Self, DReason> {
        if spec.scratchpads > SPEC_MAX_SCRATCHPADS {
            return Err(DReason::ScratchpadsInvalid);
        }
        if spec.scratchpads > SCRATCHPAD_BUDGET {
            return Err(DReason::ScratchpadsExceedBudget);
        }
        if spec.device_limit < 2 * DMA_MIN_ADDR || spec.device_limit > 1 << 48 {
            return Err(DReason::BadLimit);
        }
        Ok(Self { spec, st: [DState::Free; MAX_RECORDS], ad: [0; MAX_RECORDS], quarantined: 0, closed: false })
    }

    pub fn spec(&self) -> PoolSpec {
        self.spec
    }
    pub fn closed(&self) -> bool {
        self.closed
    }
    pub fn state(&self, item: Item) -> Option<DState> {
        self.slot(item).ok().map(|i| self.st[i])
    }
    pub fn address(&self, item: Item) -> Option<u64> {
        self.slot(item).ok().map(|i| self.ad[i])
    }
    /// Does a live scratchpad page (the array or a buffer) overlap the page at `addr`? The machine tracks only the
    /// four core pages, so it cannot see this.
    pub fn scratch_overlaps(&self, addr: u64) -> bool {
        (ARRAY..MAX_RECORDS).any(|j| self.st[j] != DState::Free && self.ad[j].abs_diff(addr) < DMA_PAGE)
    }
    pub fn live(&self) -> usize {
        self.st.iter().filter(|&&s| s != DState::Free).count()
    }
    /// Pages the plan needs: 4 core + (array + buffers) when scratchpads are required.
    pub fn planned_pages(&self) -> usize {
        CORE + if self.spec.scratchpads > 0 { 1 + usize::from(self.spec.scratchpads) } else { 0 }
    }

    fn slot(&self, item: Item) -> Result<usize, DReason> {
        match item {
            Item::Core(k) => Ok(k as usize),
            Item::Array if self.spec.scratchpads > 0 => Ok(ARRAY),
            Item::Buffer(i) if u16::from(i) < self.spec.scratchpads => Ok(BUF0 + usize::from(i)),
            _ => Err(DReason::NoSuchItem),
        }
    }

    pub fn allocate(&mut self, item: Item, addr: u64) -> Result<(), DReason> {
        let i = self.slot(item)?;
        if self.closed {
            return Err(DReason::PoolClosed);
        }
        if self.st[i] != DState::Free {
            return Err(DReason::AlreadyOwned);
        }
        if addr % DMA_PAGE != 0 {
            return Err(DReason::Misaligned);
        }
        let limit = if self.spec.ac64 { self.spec.device_limit } else { self.spec.device_limit.min(1 << 32) };
        match addr.checked_add(DMA_PAGE) {
            Some(end) if addr >= DMA_MIN_ADDR && end <= limit => {}
            _ => return Err(DReason::AddressRange),
        }
        if (0..MAX_RECORDS).any(|j| self.st[j] != DState::Free && self.ad[j].abs_diff(addr) < DMA_PAGE) {
            return Err(DReason::Overlap);
        }
        self.st[i] = DState::Allocated;
        self.ad[i] = addr;
        Ok(())
    }

    fn expect(&self, item: Item, want: DState) -> Result<usize, DReason> {
        let i = self.slot(item)?;
        match self.st[i] {
            DState::Free => Err(DReason::NotOwned),
            DState::Quarantined | DState::Retained => Err(DReason::Quarantined),
            s if s == want => Ok(i),
            _ => Err(DReason::BadState),
        }
    }

    pub fn zero(&mut self, item: Item) -> Result<(), DReason> {
        let i = self.expect(item, DState::Allocated)?;
        self.st[i] = DState::Zeroed;
        Ok(())
    }

    fn all_buffers(&self, st: DState) -> bool {
        (0..usize::from(self.spec.scratchpads)).all(|b| self.st[BUF0 + b] == st)
    }

    pub fn prepare(&mut self, item: Item) -> Result<(), DReason> {
        let i = self.expect(item, DState::Zeroed)?;
        let ready = match item {
            Item::Core(PageKind::Erst) => matches!(self.st[PageKind::Event as usize], DState::Prepared | DState::Published),
            // The array is filled with the buffer addresses, so every buffer exists first.
            Item::Array => self.all_buffers(DState::Prepared),
            // DCBAA entry 0 points at the array.
            Item::Core(PageKind::Dcbaa) if self.spec.scratchpads > 0 => self.st[ARRAY] == DState::Prepared,
            _ => true,
        };
        if !ready {
            return Err(DReason::DependencyNotReady);
        }
        self.st[i] = DState::Prepared;
        Ok(())
    }

    /// Publish a core page other than the DCBAA (the DCBAA publishes the scratchpad objects too).
    pub fn publish(&mut self, kind: PageKind, _proof: &PublishProof) -> Result<(), DReason> {
        if kind == PageKind::Dcbaa {
            return Err(DReason::BadState);
        }
        let i = self.expect(Item::Core(kind), DState::Prepared)?;
        self.st[i] = DState::Published;
        Ok(())
    }

    /// Publish the DCBAA and, atomically, the scratchpad array and every scratchpad buffer it names.
    pub fn publish_dcbaa(&mut self, _proof: &PublishProof) -> Result<(), DReason> {
        let d = self.expect(Item::Core(PageKind::Dcbaa), DState::Prepared)?;
        if self.spec.scratchpads > 0 {
            if self.st[ARRAY] != DState::Prepared || !self.all_buffers(DState::Prepared) {
                return Err(DReason::ScratchpadIncomplete);
            }
            self.st[ARRAY] = DState::DeviceOwned;
            for b in 0..usize::from(self.spec.scratchpads) {
                self.st[BUF0 + b] = DState::DeviceOwned;
            }
        }
        self.st[d] = DState::Published;
        Ok(())
    }

    /// May the CPU touch this page now? Ring pages stay accessible by the ring protocol; scratchpad pages
    /// never are once published.
    pub fn cpu_access(&self, item: Item) -> Result<(), DReason> {
        let i = self.slot(item)?;
        match self.st[i] {
            DState::Free => Err(DReason::NotOwned),
            DState::DeviceOwned => Err(DReason::CpuTouchOfDeviceOwned),
            DState::Quarantined | DState::Retained => Err(DReason::Quarantined),
            DState::Quiesced => Err(DReason::BadState),
            _ => Ok(()),
        }
    }

    /// Published and device-owned pages become quiesced (the device is verified stopped).
    pub fn quiesce(&mut self, _proof: &QuiescenceProof) -> Result<(), DReason> {
        let mut any = false;
        for s in self.st.iter_mut() {
            if matches!(*s, DState::Published | DState::DeviceOwned) {
                *s = DState::Quiesced;
                any = true;
            }
        }
        if any { Ok(()) } else { Err(DReason::NothingToQuiesce) }
    }

    pub fn release(&mut self, item: Item) -> Result<(), DReason> {
        let i = self.slot(item)?;
        match self.st[i] {
            DState::Free => Err(DReason::NotOwned),
            DState::Quarantined | DState::Retained => Err(DReason::Quarantined),
            DState::Published | DState::DeviceOwned => Err(DReason::StillPublished),
            DState::Quiesced if self.spec.release == ReleasePolicy::Never => Err(DReason::ReleaseForbiddenByPolicy),
            _ => {
                self.st[i] = DState::Free;
                self.ad[i] = 0;
                Ok(())
            }
        }
    }

    /// Keep a page forever. When the quarantine is full the page is retained and the pool closes.
    pub fn quarantine(&mut self, item: Item) -> Result<(), DReason> {
        let i = self.slot(item)?;
        self.quarantine_at(i)
    }

    fn quarantine_at(&mut self, i: usize) -> Result<(), DReason> {
        match self.st[i] {
            DState::Free => Err(DReason::NotOwned),
            DState::Quarantined | DState::Retained => Err(DReason::Quarantined),
            _ if self.quarantined < self.spec.quarantine_cap => {
                self.st[i] = DState::Quarantined;
                self.quarantined += 1;
                Ok(())
            }
            _ => {
                self.st[i] = DState::Retained;
                self.closed = true;
                Err(DReason::QuarantineExhausted)
            }
        }
    }

    /// Give up on every live page: each is kept forever (bounded; exhaustion retains the page and closes the pool).
    /// Returns how many pages were newly quarantined or retained.
    pub fn quarantine_all(&mut self) -> usize {
        let mut n = 0;
        for i in 0..MAX_RECORDS {
            if !matches!(self.st[i], DState::Free | DState::Quarantined | DState::Retained) {
                let _ = self.quarantine_at(i);
                n += 1;
            }
        }
        n
    }
}
