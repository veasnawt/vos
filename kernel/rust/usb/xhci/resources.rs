//! Logical budgets only. These types allocate no PMM, MMIO, DMA or thread resource.
use super::{BAR_BYTES, Error, PAGE};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Usage {
    pub dma_pages: u8,
    pub control_pages: u8,
    pub table_pages: u8,
    pub mappings: u8,
    pub workers: u8,
    pub permanent_bytes: u16,
}
impl Usage {
    pub fn physical_bytes(self) -> u64 {
        (u64::from(self.dma_pages) + u64::from(self.control_pages) + u64::from(self.table_pages))
            * PAGE
    }
    pub fn validate(self) -> Result<(), Error> {
        if self.dma_pages > 4
            || self.control_pages > 1
            || self.table_pages > 2
            || self.mappings > 1
            || self.workers > 1
            || self.permanent_bytes > 512
            || self.physical_bytes() > 28 * 1024
        {
            Err(Error::Budget)
        } else {
            Ok(())
        }
    }
    pub fn add(self, b: Self) -> Result<Self, Error> {
        let n = Self {
            dma_pages: self
                .dma_pages
                .checked_add(b.dma_pages)
                .ok_or(Error::Overflow)?,
            control_pages: self
                .control_pages
                .checked_add(b.control_pages)
                .ok_or(Error::Overflow)?,
            table_pages: self
                .table_pages
                .checked_add(b.table_pages)
                .ok_or(Error::Overflow)?,
            mappings: self
                .mappings
                .checked_add(b.mappings)
                .ok_or(Error::Overflow)?,
            workers: self.workers.checked_add(b.workers).ok_or(Error::Overflow)?,
            permanent_bytes: self
                .permanent_bytes
                .checked_add(b.permanent_bytes)
                .ok_or(Error::Overflow)?,
        };
        n.validate()?;
        Ok(n)
    }
    pub fn subtract(self, b: Self) -> Result<Self, Error> {
        self.validate()?;
        let n = Self {
            dma_pages: self
                .dma_pages
                .checked_sub(b.dma_pages)
                .ok_or(Error::Budget)?,
            control_pages: self
                .control_pages
                .checked_sub(b.control_pages)
                .ok_or(Error::Budget)?,
            table_pages: self
                .table_pages
                .checked_sub(b.table_pages)
                .ok_or(Error::Budget)?,
            mappings: self.mappings.checked_sub(b.mappings).ok_or(Error::Budget)?,
            workers: self.workers.checked_sub(b.workers).ok_or(Error::Budget)?,
            permanent_bytes: self
                .permanent_bytes
                .checked_sub(b.permanent_bytes)
                .ok_or(Error::Budget)?,
        };
        n.validate()?;
        Ok(n)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Kind {
    Worker,
    Control,
    Mapping,
    Command,
    Event,
    Dcbaa,
    Erst,
}
pub const KINDS: [Kind; 7] = [
    Kind::Worker,
    Kind::Control,
    Kind::Mapping,
    Kind::Command,
    Kind::Event,
    Kind::Dcbaa,
    Kind::Erst,
];
impl Kind {
    pub fn charge(self) -> Usage {
        match self {
            Self::Worker => Usage {
                workers: 1,
                ..Usage::default()
            },
            Self::Control => Usage {
                control_pages: 1,
                ..Usage::default()
            },
            // Conservative reservation; real mapping costs remain a later adapter contract.
            Self::Mapping => Usage {
                mappings: 1,
                table_pages: 2,
                ..Usage::default()
            },
            _ => Usage {
                dma_pages: 1,
                ..Usage::default()
            },
        }
    }
    pub fn dma(self) -> bool {
        matches!(self, Self::Command | Self::Event | Self::Dcbaa | Self::Erst)
    }
}

/// Non-Copy ownership record. No automatic free; backend owns the actual effect.
#[derive(Debug)]
pub struct Lease {
    // Zero-sized ownership marker, not an address or a dereferenceable pointer.
    _local: core::marker::PhantomData<*mut ()>,
    kind: Kind,
    token: u64,
    address: u64,
}
impl Lease {
    pub fn new(kind: Kind, token: u64, address: u64) -> Result<Self, Error> {
        if token == 0 {
            return Err(Error::Stale);
        }
        if kind == Kind::Worker {
            if address != 0 {
                return Err(Error::Bounds);
            }
        } else if address < 0x100000 || address % PAGE != 0 {
            return Err(Error::Alignment);
        }
        if kind == Kind::Mapping {
            address
                .checked_add(u64::from(BAR_BYTES))
                .ok_or(Error::Overflow)?;
        }
        if kind != Kind::Worker
            && kind != Kind::Mapping
            && address.checked_add(PAGE).ok_or(Error::Overflow)? > 1 << 30
        {
            return Err(Error::Bounds);
        }
        Ok(Self {
            _local: core::marker::PhantomData,
            kind,
            token,
            address,
        })
    }
    pub fn kind(&self) -> Kind {
        self.kind
    }
    pub fn token(&self) -> u64 {
        self.token
    }
    pub fn address(&self) -> u64 {
        self.address
    }
    pub fn overlaps(&self, other: &Self) -> bool {
        if self.kind == Kind::Worker || other.kind == Kind::Worker {
            return false;
        }
        let len = if self.kind == Kind::Mapping {
            u64::from(BAR_BYTES)
        } else {
            PAGE
        };
        let other_len = if other.kind == Kind::Mapping {
            u64::from(BAR_BYTES)
        } else {
            PAGE
        };
        // Constructors prove both end additions representable.
        self.address < other.address + other_len && other.address < self.address + len
    }
}
