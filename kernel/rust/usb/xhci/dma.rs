//! USB.2-C2 ownership model for DMA-designated pages. Portable decisions only:
//! the platform supplies frames, zeroing and release through `Frames`.
//!
//! Address spaces are distinct. A *CPU physical* frame number is what the PMM
//! returns and what `Frames` consumes; a *CPU virtual* view exists only inside
//! the platform; a *device-visible DMA address* is not established in C2 (no
//! IOMMU/DMAR qualification, unknown coherency), so no API produces one. Safe
//! callers never receive a raw frame: `Page` exposes kind and token only.
//!
//! Lifecycle: Free -> Allocated -> Zeroed -> Prepared. Unpublished pages
//! (Allocated, Zeroed, Prepared) may be released after token and state checks.
//! Published is designed but unreachable: its entry needs a `PublishProof` and
//! its release a `QuiescenceProof`, both uninhabited types in C2. A page whose
//! safety cannot be established is Quarantined (bounded) or, when the
//! quarantine is exhausted, Retained with the pool closed. Neither is ever
//! freed or reused; no timer, cancellation or flag releases them.
use super::resources::{Kind, Lease};
use super::{Error, PAGE};

/// Fixed record table: four live DMA pages plus four quarantine places.
pub const RECORDS: usize = 8;
/// The USB.2 design's DMA budget: command, event, DCBAA and ERST pages.
pub const ACTIVE_LIMIT: u8 = 4;
pub const QUARANTINE_LIMIT: u8 = 4;
/// Verified PMM range: individual frames in [1 MiB, 1 GiB).
pub const FRAME_LOW: u64 = 0x10_0000;
pub const FRAME_HIGH: u64 = 1 << 30;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Free,
    Allocated,
    Zeroed,
    Prepared,
    /// Future C3+: the device may access the frame. Invariants: no CPU writes
    /// except through the ring protocol; never freed, reused or zeroed until a
    /// quiescence protocol proves the device stopped (halt, reset complete,
    /// bus mastering off, ordering proof). Unreachable in C2.
    Published,
    Quarantined,
    Retained,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reason {
    None,
    Deliberate,
    ReleaseFailed,
    ZeroFailed,
    OutOfRange,
    Cancelled,
}

/// Platform frame services over the kernel PMM. Frames are CPU physical.
pub trait Frames {
    /// One frame owned by the caller, or an error with nothing acquired.
    fn alloc(&mut self) -> Result<u64, Error>;
    /// CPU-write zeros through the kernel RAM view and verify them.
    fn zero(&mut self, frame: u64) -> Result<(), Error>;
    /// Return a frame acquired by `alloc`; an error leaves it allocated.
    fn free(&mut self, frame: u64) -> Result<(), Error>;
}

/// Uninhabited in C2: no caller can publish a page to a device.
pub enum PublishProof {}
/// Uninhabited in C2: no protocol yet proves a device stopped DMA.
pub enum QuiescenceProof {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Record {
    frame: u64,
    generation: u64,
    state: State,
    kind: Kind,
    reason: Reason,
}
const FREE: Record = Record {
    frame: 0,
    generation: 1,
    state: State::Free,
    kind: Kind::Command,
    reason: Reason::None,
};

/// Exclusive handle to one live record. Non-Copy, !Send/!Sync through the
/// canonical Lease. The frame stays private; release consumes the handle.
#[derive(Debug)]
pub struct Page {
    lease: Lease,
    slot: u8,
}
impl Page {
    pub fn kind(&self) -> Kind {
        self.lease.kind()
    }
    pub fn token(&self) -> u64 {
        self.lease.token()
    }
}

/// A refused operation hands the page back unchanged; `None` means the pool
/// kept it (quarantined or retained) and the caller no longer owns it.
#[derive(Debug)]
pub struct Refused {
    pub page: Option<Page>,
    pub error: Error,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Accounting {
    pub allocated: u32,
    pub prepared: u32,
    pub released: u32,
    pub quarantined: u32,
    pub active_now: u8,
    pub retained_now: u8,
    pub allocation_failures: u32,
    pub rollback_failures: u32,
    pub violations: u32,
}

/// Diagnostic copy of one record: data for identifying retained frames,
/// never an ownership grant.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Entry {
    pub frame: u64,
    pub generation: u64,
    pub state: State,
    pub kind: Kind,
    pub reason: Reason,
}

pub struct Pool {
    records: [Record; RECORDS],
    acct: Accounting,
    closed: bool,
    limit: u64,
}

fn token(slot: usize, generation: u64) -> u64 {
    (generation << 3) | slot as u64
}

impl Pool {
    /// `device_limit` is the controller's addressable bound (e.g. 1 << 32 for
    /// a 32-bit controller); frames must also lie inside the PMM range.
    pub const fn new(device_limit: u64) -> Self {
        let limit = if device_limit < FRAME_HIGH { device_limit } else { FRAME_HIGH };
        Self {
            records: [FREE; RECORDS],
            acct: Accounting {
                allocated: 0,
                prepared: 0,
                released: 0,
                quarantined: 0,
                active_now: 0,
                retained_now: 0,
                allocation_failures: 0,
                rollback_failures: 0,
                violations: 0,
            },
            closed: false,
            limit,
        }
    }
    pub fn accounting(&self) -> Accounting {
        self.acct
    }
    pub fn closed(&self) -> bool {
        self.closed
    }
    pub fn entry(&self, slot: usize) -> Option<Entry> {
        self.records.get(slot).map(|r| Entry {
            frame: r.frame,
            generation: r.generation,
            state: r.state,
            kind: r.kind,
            reason: r.reason,
        })
    }

    fn violation(&mut self, error: Error) -> Error {
        self.acct.violations = self.acct.violations.saturating_add(1);
        error
    }

    /// Token, generation and frame must all match a live, unpublished record.
    fn check(&mut self, page: &Page) -> Result<usize, Error> {
        let slot = usize::from(page.slot);
        let ok = match self.records.get(slot) {
            Some(r) => {
                r.state != State::Free
                    && token(slot, r.generation) == page.lease.token()
                    && r.frame == page.lease.address()
                    && r.kind == page.lease.kind()
            }
            None => false,
        };
        if ok { Ok(slot) } else { Err(self.violation(Error::Stale)) }
    }

    fn valid_frame(&self, frame: u64) -> bool {
        frame % PAGE == 0
            && frame >= FRAME_LOW
            && frame.checked_add(PAGE).is_some_and(|end| end <= self.limit)
            && !self.records.iter().any(|r| r.state != State::Free && r.frame == frame)
    }

    /// Move a record out of the live set. Exhaustion fails closed: the record
    /// stays owned as Retained and the pool refuses further allocations.
    fn retain(&mut self, slot: usize, reason: Reason) -> Error {
        let r = &mut self.records[slot];
        if matches!(r.state, State::Allocated | State::Zeroed | State::Prepared | State::Published) {
            self.acct.active_now -= 1;
        }
        r.reason = reason;
        self.acct.retained_now += 1;
        self.acct.quarantined = self.acct.quarantined.saturating_add(1);
        let quarantined = self.records.iter().filter(|r| r.state == State::Quarantined).count() as u8;
        if quarantined < QUARANTINE_LIMIT && !self.closed {
            self.records[slot].state = State::Quarantined;
            Error::Quarantined
        } else {
            self.records[slot].state = State::Retained;
            self.closed = true;
            Error::Budget
        }
    }

    pub fn allocate<F: Frames>(&mut self, f: &mut F, kind: Kind) -> Result<Page, Error> {
        if !kind.dma() {
            return Err(self.violation(Error::Unsupported));
        }
        if self.closed {
            self.acct.allocation_failures = self.acct.allocation_failures.saturating_add(1);
            return Err(Error::Quarantined);
        }
        let slot = match self.records.iter().position(|r| r.state == State::Free) {
            Some(slot) if self.acct.active_now < ACTIVE_LIMIT => slot,
            _ => {
                self.acct.allocation_failures = self.acct.allocation_failures.saturating_add(1);
                return Err(Error::Budget);
            }
        };
        let frame = match f.alloc() {
            Ok(frame) => frame,
            Err(_) => {
                self.acct.allocation_failures = self.acct.allocation_failures.saturating_add(1);
                return Err(Error::Allocation);
            }
        };
        let generation = self.records[slot].generation;
        let lease = if self.valid_frame(frame) {
            Lease::new(kind, token(slot, generation), frame).ok()
        } else {
            None
        };
        let Some(lease) = lease else {
            // Never keep an unqualified frame; if it cannot be returned, it is
            // retained (a frame of unknown provenance is never reused).
            self.acct.allocation_failures = self.acct.allocation_failures.saturating_add(1);
            if f.free(frame).is_err() {
                self.acct.rollback_failures = self.acct.rollback_failures.saturating_add(1);
                self.records[slot] = Record { frame, generation, state: State::Allocated, kind, reason: Reason::None };
                self.acct.active_now += 1;
                self.retain(slot, Reason::OutOfRange);
            }
            return Err(Error::Bounds);
        };
        self.records[slot] = Record { frame, generation, state: State::Allocated, kind, reason: Reason::None };
        self.acct.active_now += 1;
        self.acct.allocated = self.acct.allocated.saturating_add(1);
        Ok(Page { lease, slot: slot as u8 })
    }

    /// Allocated -> Zeroed. A zeroing failure means the frame's content is
    /// unknown: the page is retained, never freed back for reuse.
    pub fn zero<F: Frames>(&mut self, f: &mut F, page: Page) -> Result<Page, Refused> {
        let slot = match self.check(&page) {
            Ok(slot) => slot,
            Err(error) => return Err(Refused { page: Some(page), error }),
        };
        if self.records[slot].state != State::Allocated {
            let error = self.violation(Error::IllegalState);
            return Err(Refused { page: Some(page), error });
        }
        match f.zero(self.records[slot].frame) {
            Ok(()) => {
                self.records[slot].state = State::Zeroed;
                Ok(page)
            }
            Err(_) => {
                let error = self.retain(slot, Reason::ZeroFailed);
                Err(Refused { page: None, error })
            }
        }
    }

    /// Zeroed -> Prepared: ready for a future publication step, not published.
    pub fn prepare(&mut self, page: &Page) -> Result<(), Error> {
        let slot = self.check(page)?;
        if self.records[slot].state != State::Zeroed {
            return Err(self.violation(Error::IllegalState));
        }
        self.records[slot].state = State::Prepared;
        self.acct.prepared = self.acct.prepared.saturating_add(1);
        Ok(())
    }

    /// Unreachable in C2: `PublishProof` has no values.
    pub fn publish(&mut self, _page: &Page, proof: PublishProof) -> ! {
        match proof {}
    }
    /// Unreachable in C2: `QuiescenceProof` has no values.
    pub fn release_published<F: Frames>(&mut self, _f: &mut F, _page: Page, proof: QuiescenceProof) -> ! {
        match proof {}
    }

    /// Release an unpublished page. A failed free retains it (never reused).
    pub fn release<F: Frames>(&mut self, f: &mut F, page: Page) -> Result<(), Refused> {
        let slot = match self.check(&page) {
            Ok(slot) => slot,
            Err(error) => return Err(Refused { page: Some(page), error }),
        };
        if !matches!(self.records[slot].state, State::Allocated | State::Zeroed | State::Prepared) {
            let error = self.violation(Error::IllegalState);
            return Err(Refused { page: Some(page), error });
        }
        match f.free(self.records[slot].frame) {
            Ok(()) => {
                let generation = self.records[slot].generation.wrapping_add(1).max(1);
                self.records[slot] = Record { generation, ..FREE };
                self.acct.active_now -= 1;
                self.acct.released = self.acct.released.saturating_add(1);
                Ok(())
            }
            Err(_) => {
                self.acct.rollback_failures = self.acct.rollback_failures.saturating_add(1);
                let error = self.retain(slot, Reason::ReleaseFailed);
                Err(Refused { page: None, error })
            }
        }
    }

    /// Hand a page to the bounded quarantine for the rest of the boot.
    pub fn quarantine(&mut self, page: Page, reason: Reason) -> Result<(), Refused> {
        let slot = match self.check(&page) {
            Ok(slot) => slot,
            Err(error) => return Err(Refused { page: Some(page), error }),
        };
        if !matches!(self.records[slot].state, State::Allocated | State::Zeroed | State::Prepared) {
            let error = self.violation(Error::IllegalState);
            return Err(Refused { page: Some(page), error });
        }
        match self.retain(slot, reason) {
            Error::Quarantined => Ok(()),
            error => Err(Refused { page: None, error }),
        }
    }

    /// Transactionally allocate, zero and prepare one page per kind. On any
    /// failure every page acquired so far is released in reverse order (or
    /// retained if its release fails) and `out` is left empty.
    pub fn allocate_set<F: Frames>(
        &mut self,
        f: &mut F,
        kinds: &[Kind],
        out: &mut [Option<Page>],
    ) -> Result<(), Error> {
        if kinds.len() > out.len() || out.iter().any(Option::is_some) {
            return Err(self.violation(Error::Bounds));
        }
        for (i, &kind) in kinds.iter().enumerate() {
            let step = self
                .allocate(f, kind)
                .map_err(|error| Refused { page: None, error })
                .and_then(|page| self.zero(f, page))
                .and_then(|page| match self.prepare(&page) {
                    Ok(()) => Ok(page),
                    Err(error) => Err(Refused { page: Some(page), error }),
                });
            match step {
                Ok(page) => out[i] = Some(page),
                Err(refused) => {
                    if let Some(page) = refused.page {
                        let _ = self.release(f, page);
                    }
                    self.cancel(f, out);
                    return Err(refused.error);
                }
            }
        }
        Ok(())
    }

    /// Release every unpublished page the caller holds; failures retain.
    /// Quarantined and retained pages are never touched. Returns the number
    /// of pages that could not be returned to the PMM.
    pub fn cancel<F: Frames>(&mut self, f: &mut F, pages: &mut [Option<Page>]) -> u32 {
        let mut kept = 0;
        for slot in pages.iter_mut().rev() {
            if let Some(page) = slot.take()
                && let Err(refused) = self.release(f, page)
            {
                kept += 1;
                if let Some(page) = refused.page {
                    // Not releasable (stale or wrong state): never freed.
                    let _ = self.quarantine(page, Reason::Cancelled);
                }
            }
        }
        kept
    }
}
