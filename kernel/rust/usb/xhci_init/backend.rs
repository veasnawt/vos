//! USB.2-C4.2a hosted backend contract: typed register reads with explicit identity and width, backend
//! operation sequence numbers, identity/topology/platform reads, write tickets and receipts. Pure types and
//! one trait. Nothing here performs I/O; a future native backend would implement `Backend`, and the hosted tests
//! implement it with a fake. A sequence number orders operations the backend performed; it is NOT proof that
//! hardware or firmware did not change state asynchronously.

use crate::types::{EventTrb, ModelEvent, Profile, Tri};

macro_rules! named {
    ($(#[$m:meta])* $v:vis enum $n:ident { $($(#[$vm:meta])* $var:ident => $s:expr),+ $(,)? }) => {
        $(#[$m])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        $v enum $n { $($(#[$vm])* $var),+ }
        impl $n {
            pub const ALL: &'static [$n] = &[$($n::$var),+];
            pub fn name(self) -> &'static str {
                match self { $($n::$var => $s),+ }
            }
        }
    };
}
pub(crate) use named;

named! {
    /// Access width of a register read or write.
    pub enum Width {
        W8 => "w8",
        W16 => "w16",
        W32 => "w32",
        W64 => "w64",
    }
}
impl Width {
    pub fn bits(self) -> u8 {
        match self {
            Self::W8 => 8,
            Self::W16 => 16,
            Self::W32 => 32,
            Self::W64 => 64,
        }
    }
    /// All-ones value of this width (what a read of absent or powered-off hardware returns).
    pub fn ones(self) -> u64 {
        if self == Self::W64 { u64::MAX } else { (1u64 << self.bits()) - 1 }
    }
}

named! {
    /// Register identity. Every observation names the register it claims to be.
    pub enum Reg {
        PciCommand => "pci.command",
        PciMsiControl => "pci.msi-control",
        PciMsixControl => "pci.msix-control",
        PcieDevCtl => "pcie.device-control",
        Hciversion => "hciversion",
        Hcsparams1 => "hcsparams1",
        Hcsparams2 => "hcsparams2",
        Hccparams1 => "hccparams1",
        Pagesize => "pagesize",
        Usbcmd => "usbcmd",
        Usbsts => "usbsts",
        Config => "config",
        Crcr => "crcr",
        Dcbaap => "dcbaap",
        Erstsz => "erstsz",
        Erstba => "erstba",
        Erdp => "erdp",
        Usblegsup => "usblegsup",
        DbcCtrl => "dbc.dcctrl",
        Portsc => "portsc",
    }
}
impl Reg {
    /// Architectural access width (xHCI 1.2 section 5; PCI configuration words are 16-bit).
    pub fn width(self) -> Width {
        match self {
            Self::PciCommand | Self::PciMsiControl | Self::PciMsixControl | Self::PcieDevCtl | Self::Hciversion => Width::W16,
            Self::Crcr | Self::Dcbaap | Self::Erstba | Self::Erdp => Width::W64,
            _ => Width::W32,
        }
    }
}

/// Backend operation stamp: `seq` increases by one for every backend operation (read, write, failed attempt).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stamp {
    pub seq: u64,
    pub tick: u64,
}

/// One typed register read: which register, which port (for `Portsc`), what width, what value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RegRead {
    pub reg: Reg,
    pub index: u8,
    pub width: Width,
    pub value: u64,
    pub stamp: Stamp,
    pub session: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stamped<T> {
    pub value: T,
    pub stamp: Stamp,
    pub session: u32,
}

named! {
    pub enum ReadFault {
        AllOnes => "all-ones",
        Width => "width",
        Misaligned => "misaligned",
        OutOfExtent => "out-of-extent",
        Torn => "torn",
        Partial => "partial",
        Budget => "budget",
        LeaseLost => "lease-lost",
        /// The optional register/capability does not exist (legitimate, not a failure).
        NotPresent => "not-present",
        Other => "other",
    }
}

/// A failed read still consumed a backend sequence number.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReadErr {
    pub kind: ReadFault,
    pub stamp: Stamp,
    pub session: u32,
}

named! {
    pub enum WriteFault {
        /// Definitely not performed.
        Refused => "refused",
        BadTicket => "bad-ticket",
        /// Outcome unknown: the store may or may not have happened.
        Unverified => "unverified",
        Fault => "fault",
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WriteErr {
    pub kind: WriteFault,
    pub stamp: Stamp,
    pub session: u32,
}

/// Read-back of the register a write targeted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Readback {
    pub reg: Reg,
    pub index: u8,
    pub width: Width,
    pub value: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Receipt {
    pub stamp: Stamp,
    pub session: u32,
    pub readback: Option<Readback>,
}

/// Authority for exactly one accepted write. Only the session can mint one; a ticket is single-use.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WriteTicket {
    nonce: u64,
    id: &'static str,
}
impl WriteTicket {
    pub(crate) fn new(nonce: u64, id: &'static str) -> Self {
        Self { nonce, id }
    }
    pub fn nonce(&self) -> u64 {
        self.nonce
    }
    pub fn id(&self) -> &'static str {
        self.id
    }
}

/// Controller identity as read from configuration space and the capability registers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Identity {
    pub bdf: u32,
    pub vendor: u16,
    pub device: u16,
    pub revision: u8,
    pub class: u32,
    pub subsystem: u32,
    pub bar0: u64,
    pub hciversion: u16,
    pub hcs1: u32,
    pub hcs2: u32,
    pub hcs3: u32,
    pub hcc1: u32,
    pub dboff: u32,
    pub rtsoff: u32,
}

/// What else lives in the scanned configuration space.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Topology {
    /// xHCI-class functions found by a complete bounded scan.
    pub xhci_functions: u8,
    /// The scan visited every function it was supposed to.
    pub scan_complete: bool,
    pub behind_bridge: bool,
}

/// Platform facts that are not registers (ACPI-derived and environment).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlatformFacts {
    pub profile: Profile,
    pub iommu: Tri,
    pub identity_dma: Tri,
    pub dma_limit: u64,
}

pub type ReadResult<T> = Result<Stamped<T>, ReadErr>;

/// The contract a native backend must satisfy. Every method that touches the controller or platform
/// returns a stamp whose `seq` is exactly one more than the previous operation's. The backend never
/// retries, caches or reorders, and refuses everything but the quarantine tail after `latch_terminal`.
pub trait Backend {
    fn read(&mut self, reg: Reg, index: u8) -> Result<RegRead, ReadErr>;
    fn read_identity(&mut self) -> ReadResult<Identity>;
    fn read_topology(&mut self) -> ReadResult<Topology>;
    fn read_platform(&mut self) -> ReadResult<PlatformFacts>;
    /// Event ring slot `index` read with consumer cycle `ccs` (cycle first, then the rest).
    fn read_event(&mut self, index: u16, ccs: bool) -> ReadResult<Option<EventTrb>>;
    /// Perform exactly the accepted write named by `ev`, against `ticket`. Not performed without a valid ticket.
    fn perform(&mut self, ev: &ModelEvent, ticket: WriteTicket) -> Result<Receipt, WriteErr>;
    fn latch_terminal(&mut self);
    /// Tick counter for deadlines (not a freshness proof).
    fn now(&self) -> u64;
}
