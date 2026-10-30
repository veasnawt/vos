//! USB input bring-up, Increment 1: a read-only survey of the PCI bus tree and every xHCI controller on it.
//!
//! The qualifier (`qualify`) accepts one QEMU profile and rejects everything else; a physical computer needs the
//! opposite first: a report of what is actually there. This module only *reads* and only *describes*. Its traits have
//! no write operation, it never rejects a controller for being unusual (each failure becomes a short reason in the
//! report), every loop and every read is bounded, and a malformed or hostile register file can make the report say
//! less, never make the survey do more. It grants no authority: a controller that is listed here is not thereby
//! qualified, owned or safe to program.
//!
//! Limits recorded in the report text rather than hidden: PCI bridges are followed through their firmware-assigned
//! secondary bus numbers (a host bridge on another root bus is not found); the register window is 64 KiB at the BAR
//! base because the BAR size cannot be learned without writing the BAR; ACPI IOMMU tables (DMAR, IVRS) are not read.

/// Bounded work: functions visited, buses queued, controllers recorded in detail, register reads per controller.
pub const FUNCTION_LIMIT: u16 = 4096;
pub const BUS_LIMIT: usize = 32;
pub const MAX_USB: usize = 12;
pub const READ_LIMIT: u16 = 400;
/// The register window the platform leases at the BAR base.
pub const WINDOW: u32 = 64 * 1024;
pub const PORT_LIMIT: u8 = 64;
pub const CAP_LIMIT: usize = 32;

/// Configuration reads (mechanism #1 or MMCONFIG); `0xFFFF_FFFF` where nothing answers. No write operation exists.
pub trait Pci {
    fn read32(&mut self, bus: u8, dev: u8, func: u8, off: u8) -> u32;
}
/// Aligned 32-bit reads of one leased BAR window; `None` when the offset is outside it or the platform refuses.
pub trait Mmio {
    fn read32(&mut self, off: u32) -> Option<u32>;
}
/// Text output (serial and screen).
pub trait Out {
    fn put(&mut self, text: &[u8]);
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UsbFn {
    pub bus: u8,
    pub dev: u8,
    pub func: u8,
    pub header: u8,
    pub vendor: u16,
    pub device: u16,
    pub class_rev: u32,
    pub subsystem: u32,
    pub command: u16,
    pub status: u16,
    pub bar0: u32,
    pub bar1: u32,
    pub irq_line: u8,
    pub irq_pin: u8,
}
impl UsbFn {
    pub fn prog_if(&self) -> u8 {
        (self.class_rev >> 8) as u8
    }
    pub fn is_xhci(&self) -> bool {
        self.class_rev >> 8 == 0x000c_0330
    }
    pub fn kind(&self) -> &'static str {
        match self.prog_if() {
            0x00 => "uhci",
            0x10 => "ohci",
            0x20 => "ehci",
            0x30 => "xhci",
            0x40 => "usb4",
            0x80 => "other",
            0xfe => "device",
            _ => "unknown",
        }
    }
    pub fn is_ehci(&self) -> bool {
        self.class_rev >> 8 == 0x000c_0320
    }
    /// Base of memory BAR 0 of an xHCI (a 4 KiB-aligned BAR): `Err` says why there is none usable.
    pub fn bar_base(&self) -> Result<u64, &'static str> {
        self.bar_base_aligned(4096)
    }
    /// An EHCI's BAR is only 1 KiB, so it may sit anywhere in a page at a 1 KiB boundary.
    pub fn ehci_base(&self) -> Result<u64, &'static str> {
        self.bar_base_aligned(1024)
    }
    fn bar_base_aligned(&self, align: u64) -> Result<u64, &'static str> {
        if self.header & 0x7f != 0 {
            return Err("not a type-0 header");
        }
        if self.command & 2 == 0 {
            return Err("memory decoding off");
        }
        let low = self.bar0;
        if low & 1 != 0 {
            return Err("I/O BAR");
        }
        let base = match (low >> 1) & 3 {
            0 => u64::from(low & !0xf),
            2 => (u64::from(self.bar1) << 32) | u64::from(low & !0xf),
            _ => return Err("BAR type"),
        };
        if base == 0 {
            return Err("BAR unassigned");
        }
        if base < 0x10_0000 {
            return Err("BAR below 1 MiB");
        }
        if base % align != 0 {
            return Err("BAR unaligned");
        }
        Ok(base)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Scan {
    pub usb: [UsbFn; MAX_USB],
    /// Recorded entries in `usb`, and the total found (more than `MAX_USB` is reported, not hidden).
    pub recorded: u8,
    pub usb_total: u16,
    pub seen: u16,
    pub buses: u8,
    pub bridges: u8,
    /// Bridges whose secondary bus is unassigned, reversed or not behind them: not followed.
    pub skipped_bridges: u8,
    /// Functions with class 08/06 (system IOMMU); only AMD's is a PCI function, Intel's lives in ACPI DMAR.
    pub iommu_functions: u8,
    pub limit_hit: bool,
}

fn word(p: &mut impl Pci, b: u8, d: u8, f: u8, off: u8) -> u32 {
    p.read32(b, d, f, off)
}

/// Does something answer with this vendor/device word? All ones is a master abort; vendor 0 is not a vendor.
fn present(id: u32) -> bool {
    id as u16 != 0xffff && id as u16 != 0
}

/// Walk bus 0 and every bus a bridge reports behind it. Bounded and cycle-free (a bus is queued once, only upward).
pub fn scan<P: Pci>(p: &mut P) -> Scan {
    let mut s = Scan::default();
    let mut queue = [0u8; BUS_LIMIT];
    let mut queued = [false; 256];
    let (mut head, mut tail) = (0usize, 1usize);
    queued[0] = true;
    while head < tail {
        let bus = queue.get(head).copied().unwrap_or(0);
        head += 1;
        s.buses += 1;
        for dev in 0..32u8 {
            let id0 = word(p, bus, dev, 0, 0);
            if !present(id0) {
                continue;
            }
            let multi = (word(p, bus, dev, 0, 0x0c) >> 16) as u8 & 0x80 != 0;
            for func in 0..if multi { 8u8 } else { 1 } {
                if s.seen >= FUNCTION_LIMIT {
                    s.limit_hit = true;
                    return s;
                }
                let id = if func == 0 { id0 } else { word(p, bus, dev, func, 0) };
                if !present(id) {
                    continue;
                }
                s.seen += 1;
                let class_rev = word(p, bus, dev, func, 8);
                let header = (word(p, bus, dev, func, 0x0c) >> 16) as u8;
                match class_rev >> 16 {
                    0x0c03 => {
                        s.usb_total = s.usb_total.saturating_add(1);
                        if let Some(slot) = s.usb.get_mut(usize::from(s.recorded)) {
                            let cs = word(p, bus, dev, func, 4);
                            let irq = word(p, bus, dev, func, 0x3c);
                            *slot = UsbFn {
                                bus,
                                dev,
                                func,
                                header,
                                vendor: id as u16,
                                device: (id >> 16) as u16,
                                class_rev,
                                subsystem: word(p, bus, dev, func, 0x2c),
                                command: cs as u16,
                                status: (cs >> 16) as u16,
                                bar0: word(p, bus, dev, func, 0x10),
                                bar1: word(p, bus, dev, func, 0x14),
                                irq_line: irq as u8,
                                irq_pin: (irq >> 8) as u8,
                            };
                            s.recorded += 1;
                        }
                    }
                    0x0806 => s.iommu_functions = s.iommu_functions.saturating_add(1),
                    0x0604 if header & 0x7f == 1 => {
                        s.bridges = s.bridges.saturating_add(1);
                        let buses = word(p, bus, dev, func, 0x18);
                        let (secondary, subordinate) = ((buses >> 8) as u8, (buses >> 16) as u8);
                        if secondary > bus && subordinate >= secondary && !queued[usize::from(secondary)] && tail < BUS_LIMIT {
                            queued[usize::from(secondary)] = true;
                            if let Some(q) = queue.get_mut(tail) {
                                *q = secondary;
                            }
                            tail += 1;
                        } else if secondary <= bus || subordinate < secondary {
                            s.skipped_bridges = s.skipped_bridges.saturating_add(1);
                        } else if tail >= BUS_LIMIT && !queued[usize::from(secondary)] {
                            s.limit_hit = true;
                        }
                    }
                    _ => {}
                }
            }
        }
    }
    s
}

/// What the PCI configuration space of one controller says (read-only).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PciInfo {
    pub caps: u8,
    pub pm: u8,
    pub msi: u8,
    pub msix: u8,
    pub pcie: u8,
    /// PM control state (0 = D0), MSI/MSI-X enable, PCIe Device Control (No Snoop is bit 11).
    pub power: u8,
    pub msi_on: bool,
    pub msix_on: bool,
    pub devctl: u16,
    pub truncated: bool,
}

pub fn pci_info<P: Pci>(p: &mut P, f: &UsbFn) -> PciInfo {
    let mut i = PciInfo::default();
    if f.status & 0x10 == 0 {
        return i;
    }
    let (b, d, n) = (f.bus, f.dev, f.func);
    let mut ptr = (word(p, b, d, n, 0x34) as u8) & !3;
    let mut seen = 0u64;
    for _ in 0..48 {
        if ptr < 0x40 {
            return i;
        }
        let bit = 1u64 << ((ptr >> 2) & 63);
        if seen & bit != 0 {
            i.truncated = true;
            return i;
        }
        seen |= bit;
        let hdr = word(p, b, d, n, ptr);
        i.caps = i.caps.saturating_add(1);
        match hdr as u8 {
            0x01 if i.pm == 0 && ptr <= 0xf8 => {
                i.pm = ptr;
                i.power = (word(p, b, d, n, ptr + 4) & 3) as u8;
            }
            0x05 if i.msi == 0 => {
                i.msi = ptr;
                i.msi_on = (hdr >> 16) & 1 != 0;
            }
            0x11 if i.msix == 0 => {
                i.msix = ptr;
                i.msix_on = (hdr >> 16) & 0x8000 != 0;
            }
            0x10 if i.pcie == 0 && ptr <= 0xf0 => {
                i.pcie = ptr;
                i.devctl = word(p, b, d, n, ptr + 8) as u16;
            }
            _ => {}
        }
        ptr = ((hdr >> 8) as u8) & !3;
    }
    i.truncated = ptr >= 0x40;
    i
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Proto {
    pub major: u8,
    pub minor: u8,
    pub first: u8,
    pub count: u8,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Port {
    pub index: u8,
    pub portsc: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Xhci {
    pub reads: u16,
    /// `None`: the registers were read as far as they went; `Some(why)`: the walk stopped early.
    pub stopped: Option<&'static str>,
    pub caplength: u8,
    pub version: u16,
    pub hcs1: u32,
    pub hcs2: u32,
    pub hcs3: u32,
    pub hcc1: u32,
    pub dboff: u32,
    pub rtsoff: u32,
    pub hcc2: u32,
    pub usbcmd: u32,
    pub usbsts: u32,
    pub pagesize: u32,
    pub dnctrl: u32,
    pub config: u32,
    /// Second reading of USBCMD and USBSTS at the end: a controller that changes by itself shows here.
    pub usbcmd2: u32,
    pub usbsts2: u32,
    pub ports: u8,
    pub ports_read: u8,
    pub connected: u8,
    pub conn: [Port; 8],
    pub caps: u8,
    pub ids: [u8; 16],
    pub legacy: Option<(u16, u32, u32)>,
    pub dbc: Option<(u16, u32)>,
    pub protocols: u8,
    pub proto: [Proto; 6],
    /// Raw PORTSC of the first 32 ports (the model and the survey support up to 31).
    pub portsc: [u32; 32],
    /// Ring pointers as the controller (or the firmware that ran it) left them: DCBAAP, CRCR (low dword; the pointer bits may
    /// read as zero) and interrupter 0's IMAN, ERSTSZ, ERSTBA and ERDP. Read, never used.
    pub dcbaap: u64,
    pub crcr: u32,
    pub ir_iman: u32,
    pub ir_erstsz: u32,
    pub ir_erstba: u64,
    pub ir_erdp: u64,
    pub ptr_stopped: Option<&'static str>,
}
impl Xhci {
    pub fn slots(&self) -> u32 {
        self.hcs1 & 0xff
    }
    pub fn interrupters(&self) -> u32 {
        (self.hcs1 >> 8) & 0x7ff
    }
    /// HCSPARAMS2 Max Scratchpad Buffers = Hi (bits 25:21) : Lo (bits 31:27), xHCI 1.2 section 5.3.4.
    pub fn scratchpads(&self) -> u32 {
        (((self.hcs2 >> 21) & 0x1f) << 5) | ((self.hcs2 >> 27) & 0x1f)
    }
}

struct Reader<'a, M: Mmio> {
    m: &'a mut M,
    reads: u16,
    /// The window this reader may touch: `WINDOW` for an xHCI, `EHCI_WINDOW` for an EHCI.
    limit: u32,
}
impl<M: Mmio> Reader<'_, M> {
    fn word(&mut self, off: u32) -> Result<u32, &'static str> {
        if off % 4 != 0 {
            return Err("unaligned offset");
        }
        if off > self.limit.saturating_sub(4) {
            return Err("outside window");
        }
        if self.reads >= READ_LIMIT {
            return Err("read budget");
        }
        self.reads += 1;
        self.m.read32(off).ok_or("read refused")
    }
}

/// Read every register worth reporting. Never writes, never fails the caller: `stopped` records where it ended.
pub fn inspect_xhci<M: Mmio>(m: &mut M) -> Xhci {
    let mut x = Xhci::default();
    let mut r = Reader { m, reads: 0, limit: WINDOW };
    x.stopped = walk(&mut r, &mut x).err();
    x.reads = r.reads;
    x
}

fn walk<M: Mmio>(r: &mut Reader<'_, M>, x: &mut Xhci) -> Result<(), &'static str> {
    let w0 = r.word(0)?;
    x.caplength = w0 as u8;
    x.version = (w0 >> 16) as u16;
    if w0 == 0xffff_ffff {
        return Err("reads as all ones");
    }
    x.hcs1 = r.word(4)?;
    x.hcs2 = r.word(8)?;
    x.hcs3 = r.word(0xc)?;
    x.hcc1 = r.word(0x10)?;
    x.dboff = r.word(0x14)?;
    x.rtsoff = r.word(0x18)?;
    let op = u32::from(x.caplength);
    if op < 0x20 || op % 4 != 0 {
        return Err("bad CAPLENGTH");
    }
    x.hcc2 = r.word(0x1c)?;
    x.usbcmd = r.word(op)?;
    x.usbsts = r.word(op + 4)?;
    x.pagesize = r.word(op + 8)?;
    x.dnctrl = r.word(op + 0x14)?;
    x.config = r.word(op + 0x38)?;
    // Port status registers: bounded, and only those the declared port count places inside the window.
    x.ports = (x.hcs1 >> 24) as u8;
    let mut next = 0usize;
    for p in 0..x.ports.min(PORT_LIMIT) {
        let sc = r.word(op + 0x400 + 0x10 * u32::from(p))?;
        x.ports_read += 1;
        if let Some(slot) = x.portsc.get_mut(usize::from(p)) {
            *slot = sc;
        }
        if sc & 1 != 0 {
            x.connected = x.connected.saturating_add(1);
            if let Some(slot) = x.conn.get_mut(next) {
                *slot = Port { index: p, portsc: sc };
                next += 1;
            }
        }
    }
    // Extended capabilities (xHCI 1.2 section 7): a bounded chain of dword-granular headers.
    // The next pointer only moves forward (a count of dwords from this header), so the chain cannot loop; it can
    // only run off the window or on for too long, and both end the walk.
    let mut off = (x.hcc1 >> 16) << 2;
    let mut ids = 0usize;
    while off != 0 {
        if usize::from(x.caps) >= CAP_LIMIT {
            return Err("capability list too long");
        }
        let h = r.word(off)?;
        let id = h as u8;
        x.caps += 1;
        if let Some(s) = x.ids.get_mut(ids) {
            *s = id;
            ids += 1;
        }
        match id {
            1 if x.legacy.is_none() => x.legacy = Some((off as u16, h, r.word(off + 4)?)),
            10 if x.dbc.is_none() => x.dbc = Some((off as u16, r.word(off + 0x20)?)),
            2 => {
                let name = r.word(off + 4)?;
                let ports = r.word(off + 8)?;
                if name == u32::from_le_bytes(*b"USB ")
                    && let Some(s) = x.proto.get_mut(usize::from(x.protocols))
                {
                    *s = Proto { major: (h >> 24) as u8, minor: (h >> 16) as u8, first: ports as u8, count: (ports >> 8) as u8 };
                    x.protocols += 1;
                }
            }
            _ => {}
        }
        let next = ((h >> 8) & 0xff) << 2;
        if next == 0 {
            break;
        }
        off += next; // at most 0x3fffc + 32 * 0x3fc: cannot overflow
    }
    // The pointers are read after the capability walk and their failure is recorded separately: a controller whose runtime
    // registers are outside the window still gets its second USBCMD/USBSTS reading.
    x.ptr_stopped = pointers(r, x, op).err();
    x.usbcmd2 = r.word(op)?;
    x.usbsts2 = r.word(op + 4)?;
    Ok(())
}

fn pointers<M: Mmio>(r: &mut Reader<'_, M>, x: &mut Xhci, op: u32) -> Result<(), &'static str> {
    x.crcr = r.word(op + 0x18)?;
    let (lo, hi) = (r.word(op + 0x30)?, r.word(op + 0x34)?);
    x.dcbaap = u64::from(hi) << 32 | u64::from(lo);
    let rt = x.rtsoff;
    if rt % 32 != 0 {
        return Err("RTSOFF unaligned");
    }
    let ir = rt.checked_add(0x20).ok_or("RTSOFF overflows")?;
    x.ir_iman = r.word(ir)?;
    x.ir_erstsz = r.word(ir.checked_add(8).ok_or("RTSOFF overflows")?)?;
    let (blo, bhi) = (r.word(ir.checked_add(0x10).ok_or("RTSOFF overflows")?)?, r.word(ir.checked_add(0x14).ok_or("RTSOFF overflows")?)?);
    x.ir_erstba = u64::from(bhi) << 32 | u64::from(blo);
    let (dlo, dhi) = (r.word(ir.checked_add(0x18).ok_or("RTSOFF overflows")?)?, r.word(ir.checked_add(0x1c).ok_or("RTSOFF overflows")?)?);
    x.ir_erdp = u64::from(dhi) << 32 | u64::from(dlo);
    Ok(())
}

// ---- Intel port routing (read-only) --------------------------------------------------------------------------------------

/// Intel xHCI vendor-specific PCI configuration registers that decide which USB 2.0 ports the xHCI owns and whether the
/// SuperSpeed lanes are enabled (the registers Linux's `usb_enable_intel_xhci_ports` writes). Only ever read here.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Routing {
    /// 0xD0 XUSB2PR: bit n set routes USB 2.0 port n+1 to the xHCI, clear leaves it with its EHCI.
    pub xusb2pr: u32,
    /// 0xD4 XUSB2PRM: which of those ports can be routed at all.
    pub xusb2prm: u32,
    /// 0xD8 USB3_PSSEN: SuperSpeed port enables, and 0xDC USB3PRM the ports that can be enabled.
    pub usb3_pssen: u32,
    pub usb3prm: u32,
}
/// USB 2.0 ports the xHCI of an 8-Series PCH has (the survey photo shows `usb2.00 ports 1+15`).
pub const ROUTING_PORT_MASK: u32 = 0x7fff;
impl Routing {
    pub fn usb2_routable(&self) -> u32 {
        self.xusb2prm & ROUTING_PORT_MASK
    }
    pub fn usb2_to_xhci(&self) -> u32 {
        self.xusb2pr & self.usb2_routable()
    }
    pub fn usb2_to_ehci(&self) -> u32 {
        !self.xusb2pr & self.usb2_routable()
    }
    pub fn usb3_enabled(&self) -> u32 {
        self.usb3_pssen & self.usb3prm
    }
    pub fn usb3_disabled(&self) -> u32 {
        !self.usb3_pssen & self.usb3prm
    }
}

pub fn intel_routing<P: Pci>(p: &mut P, f: &UsbFn) -> Option<Routing> {
    if f.vendor != 0x8086 || !f.is_xhci() || f.header & 0x7f != 0 {
        return None;
    }
    let r = Routing {
        xusb2pr: word(p, f.bus, f.dev, f.func, 0xd0),
        xusb2prm: word(p, f.bus, f.dev, f.func, 0xd4),
        usb3_pssen: word(p, f.bus, f.dev, f.func, 0xd8),
        usb3prm: word(p, f.bus, f.dev, f.func, 0xdc),
    };
    // A function without these registers reads all ones everywhere.
    if r.xusb2pr == 0xffff_ffff && r.xusb2prm == 0xffff_ffff && r.usb3_pssen == 0xffff_ffff && r.usb3prm == 0xffff_ffff {
        return None;
    }
    Some(r)
}

// ---- EHCI (read-only) --------------------------------------------------------------------------------------------------

/// EHCI registers sit in the first 0x100 bytes of a 1 KiB BAR.
pub const EHCI_WINDOW: u32 = 0x100;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Ehci {
    pub reads: u16,
    pub stopped: Option<&'static str>,
    pub caplength: u8,
    pub version: u16,
    pub hcs: u32,
    pub hcc: u32,
    pub usbcmd: u32,
    pub usbsts: u32,
    pub configflag: u32,
    pub ports: u8,
    pub portsc: [u32; 16],
    pub connected: u8,
    /// Ports whose Port Owner bit is set: handed to a companion controller.
    pub companion_owned: u8,
    /// USB legacy support in PCI configuration space: (offset, USBLEGSUP, USBLEGCTLSTS).
    pub legacy: Option<(u8, u32, u32)>,
}
impl Ehci {
    pub fn halted(&self) -> bool {
        self.usbsts & (1 << 12) != 0
    }
}

pub fn inspect_ehci<M: Mmio, P: Pci>(m: &mut M, p: &mut P, f: &UsbFn) -> Ehci {
    let mut e = Ehci::default();
    let mut r = Reader { m, reads: 0, limit: EHCI_WINDOW };
    e.stopped = ehci_walk(&mut r, &mut e).err();
    e.reads = r.reads;
    // USB legacy support lives in PCI configuration space, at the offset HCCPARAMS names (EECP).
    let mut at = ((e.hcc >> 8) & 0xff) as u8;
    for _ in 0..8 {
        if !(0x40..=0xf8).contains(&at) {
            break;
        }
        let h = word(p, f.bus, f.dev, f.func, at);
        if h as u8 == 1 {
            e.legacy = Some((at, h, word(p, f.bus, f.dev, f.func, at + 4)));
            break;
        }
        let next = ((h >> 8) & 0xff) as u8;
        if next == 0 {
            break;
        }
        at = next;
    }
    e
}

fn ehci_walk<M: Mmio>(r: &mut Reader<'_, M>, e: &mut Ehci) -> Result<(), &'static str> {
    let w0 = r.word(0)?;
    if w0 == 0xffff_ffff {
        return Err("reads as all ones");
    }
    e.caplength = w0 as u8;
    e.version = (w0 >> 16) as u16;
    e.hcs = r.word(4)?;
    e.hcc = r.word(8)?;
    let op = u32::from(e.caplength);
    if !(0x10..=0x40).contains(&op) || op % 4 != 0 {
        return Err("bad CAPLENGTH");
    }
    e.usbcmd = r.word(op)?;
    e.usbsts = r.word(op + 4)?;
    e.configflag = r.word(op + 0x40)?;
    e.ports = (e.hcs & 0xf) as u8;
    for i in 0..e.ports {
        let sc = r.word(op + 0x44 + 4 * u32::from(i))?;
        if let Some(slot) = e.portsc.get_mut(usize::from(i)) {
            *slot = sc;
        }
        e.connected += (sc & 1) as u8;
        e.companion_owned += (sc >> 13 & 1) as u8;
    }
    Ok(())
}

// ---- rendering ---------------------------------------------------------------------------------------------------------

/// One output line, built without `core::fmt`; truncates at its capacity.
pub struct Line {
    b: [u8; 120],
    n: usize,
}
impl Default for Line {
    fn default() -> Self {
        Self::new()
    }
}
impl Line {
    pub const fn new() -> Self {
        Self { b: [0; 120], n: 0 }
    }
    #[inline(never)]
    pub fn s(&mut self, t: &str) -> &mut Self {
        for &c in t.as_bytes() {
            self.c(c);
        }
        self
    }
    #[inline(never)]
    pub fn c(&mut self, c: u8) -> &mut Self {
        if self.n < self.b.len() - 2
            && let Some(slot) = self.b.get_mut(self.n)
        {
            *slot = if c.is_ascii_graphic() || c == b' ' { c } else { b'?' };
            self.n += 1;
        }
        self
    }
    /// Lowercase hexadecimal with at least `digits` digits and a `0x` prefix.
    #[inline(never)]
    pub fn x(&mut self, v: u64, digits: u32) -> &mut Self {
        self.s("0x").hx(v, digits)
    }
    /// The same without the prefix (PCI addresses are written 00:14.0).
    #[inline(never)]
    pub fn hx(&mut self, v: u64, digits: u32) -> &mut Self {
        let mut started = false;
        for i in (0..16u32).rev() {
            let nib = ((v >> (4 * i)) & 0xf) as u8;
            if nib != 0 || started || i < digits || i == 0 {
                started = true;
                self.c(if nib < 10 { b'0' + nib } else { b'a' + nib - 10 });
            }
        }
        self
    }
    #[inline(never)]
    pub fn d(&mut self, mut v: u64) -> &mut Self {
        let mut t = [0u8; 20];
        let mut i = t.len();
        loop {
            i = i.saturating_sub(1);
            if let Some(slot) = t.get_mut(i) {
                *slot = b'0' + (v % 10) as u8;
            }
            v /= 10;
            if v == 0 || i == 0 {
                break;
            }
        }
        for &c in t.get(i..).unwrap_or(&[]) {
            self.c(c);
        }
        self
    }
    /// A set of port numbers (bit n is port n+1) as ranges, `-` when empty: `1-8,10`.
    #[inline(never)]
    pub fn set(&mut self, mask: u32) -> &mut Self {
        let mut any = false;
        let mut i = 0u32;
        while i < 32 {
            if mask >> i & 1 != 0 {
                let start = i;
                while i + 1 < 32 && mask >> (i + 1) & 1 != 0 {
                    i += 1;
                }
                if any {
                    self.s(",");
                }
                any = true;
                self.d(u64::from(start) + 1);
                if i > start {
                    self.s("-").d(u64::from(i) + 1);
                }
            }
            i += 1;
        }
        if !any {
            self.s("-");
        }
        self
    }
    #[inline(never)]
    pub fn flush(&mut self, out: &mut dyn Out) {
        for crlf in [b'\r', b'\n'] {
            // `c` would neutralise the control bytes, so the two terminators are stored directly; there are always
            // two spare bytes because `c` stops at `len - 2`.
            if let Some(slot) = self.b.get_mut(self.n) {
                *slot = crlf;
                self.n += 1;
            }
        }
        out.put(self.b.get(..self.n).unwrap_or(&[]));
        self.n = 0;
    }
    pub fn bytes(&self) -> &[u8] {
        self.b.get(..self.n).unwrap_or(&[])
    }
}

fn bdf(l: &mut Line, f: &UsbFn) {
    l.hx(u64::from(f.bus), 2).s(":").hx(u64::from(f.dev), 2).s(".").d(u64::from(f.func));
}

pub fn render_scan(s: &Scan, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: survey (read-only) pci functions ").d(u64::from(s.seen)).s(" buses ").d(u64::from(s.buses)).s(" bridges ").d(u64::from(s.bridges));
    l.s(" usb ").d(u64::from(s.usb_total)).s(" iommu-fn ").d(u64::from(s.iommu_functions));
    l.flush(out);
    if s.limit_hit || s.skipped_bridges != 0 || s.usb_total > u16::from(s.recorded) {
        l.s("usb: scan incomplete:");
        if s.limit_hit {
            l.s(" function limit");
        }
        if s.skipped_bridges != 0 {
            l.s(" bridges without usable bus numbers ").d(u64::from(s.skipped_bridges));
        }
        if s.usb_total > u16::from(s.recorded) {
            l.s(" controllers beyond ").d(u64::from(s.recorded)).s(" not listed");
        }
        l.flush(out);
    }
    for f in s.usb.iter().take(usize::from(s.recorded)) {
        l.s("usb: ").s(f.kind()).s(" ");
        bdf(&mut l, f);
        l.s(" ").x(u64::from(f.vendor), 4).s(":").x(u64::from(f.device), 4).s(" cls ").x(u64::from(f.class_rev >> 8), 6);
        l.s(" rev ").x(u64::from(f.class_rev & 0xff), 2).s(" sub ").x(u64::from(f.subsystem), 8).s(" cmd ").x(u64::from(f.command), 4);
        l.flush(out);
    }
}

pub fn render_pci(f: &UsbFn, i: &PciInfo, bar: &Result<u64, &'static str>, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: ");
    bdf(&mut l, f);
    match bar {
        Ok(b) => l.s(" bar0 ").x(*b, 8),
        Err(why) => l.s(" bar0 unusable: ").s(why),
    };
    l.s(" irq ").d(u64::from(f.irq_line)).s("/pin").d(u64::from(f.irq_pin)).s(" pm D").d(u64::from(i.power));
    l.s(" msi ").d(u64::from(i.msi_on)).s(" msix ").d(u64::from(i.msix_on)).s(" caps ").d(u64::from(i.caps));
    if i.pcie != 0 {
        l.s(" pcie-devctl ").x(u64::from(i.devctl), 4).s(" nosnoop ").d(u64::from(i.devctl >> 11 & 1));
    }
    l.flush(out);
}

pub fn render_xhci(f: &UsbFn, x: &Xhci, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: ");
    bdf(&mut l, f);
    l.s(" hci ").x(u64::from(x.version), 4).s(" caplen ").x(u64::from(x.caplength), 2).s(" slots ").d(u64::from(x.slots()));
    l.s(" intr ").d(u64::from(x.interrupters())).s(" ports ").d(u64::from(x.ports)).s(" scratchpads ").d(u64::from(x.scratchpads()));
    l.s(" ac64 ").d(u64::from(x.hcc1 & 1)).s(" csz ").d(u64::from(x.hcc1 >> 2 & 1)).s(" ppc ").d(u64::from(x.hcc1 >> 3 & 1));
    l.flush(out);
    l.s("usb:   hcs1 ").x(u64::from(x.hcs1), 8).s(" hcs2 ").x(u64::from(x.hcs2), 8).s(" hcs3 ").x(u64::from(x.hcs3), 8);
    l.s(" hcc1 ").x(u64::from(x.hcc1), 8).s(" hcc2 ").x(u64::from(x.hcc2), 8);
    l.flush(out);
    l.s("usb:   usbcmd ").x(u64::from(x.usbcmd), 8).s(" usbsts ").x(u64::from(x.usbsts), 8).s(" (R/S ").d(u64::from(x.usbcmd & 1));
    l.s(" HCH ").d(u64::from(x.usbsts & 1)).s(" HSE ").d(u64::from(x.usbsts >> 2 & 1)).s(" HCE ").d(u64::from(x.usbsts >> 12 & 1)).s(")");
    l.s(" pagesize ").x(u64::from(x.pagesize), 4).s(" config ").x(u64::from(x.config), 4);
    l.flush(out);
    if x.usbcmd != x.usbcmd2 || x.usbsts != x.usbsts2 {
        l.s("usb:   WARNING: usbcmd/usbsts changed by themselves between two reads: ").x(u64::from(x.usbcmd2), 8).s(" ").x(u64::from(x.usbsts2), 8);
        l.flush(out);
    }
    l.s("usb:   ports read ").d(u64::from(x.ports_read)).s(" connected ").d(u64::from(x.connected));
    for p in x.conn.iter().take(usize::from(x.connected.min(8))) {
        l.s(" p").d(u64::from(p.index) + 1).s("=").x(u64::from(p.portsc), 8);
    }
    l.flush(out);
    l.s("usb:   ext caps ").d(u64::from(x.caps)).s(" ids");
    for id in x.ids.iter().take(usize::from(x.caps).min(x.ids.len())) {
        l.s(" ").d(u64::from(*id));
    }
    match x.legacy {
        Some((off, sup, ctl)) => {
            l.s(" legacy@").x(u64::from(off), 4).s(" bios ").d(u64::from(sup >> 16 & 1)).s(" os ").d(u64::from(sup >> 24 & 1));
            l.s(" smi ").x(u64::from(ctl), 8);
        }
        None => {
            l.s(" no-legacy");
        }
    }
    if let Some((off, ctrl)) = x.dbc {
        l.s(" dbc@").x(u64::from(off), 4).s(" ctrl ").x(u64::from(ctrl), 8);
    }
    l.flush(out);
    if x.protocols != 0 {
        l.s("usb:   protocols");
        for p in x.proto.iter().take(usize::from(x.protocols)) {
            l.s(" usb").hx(u64::from(p.major), 1).s(".").hx(u64::from(p.minor), 2).s(" ports ").d(u64::from(p.first)).s("+").d(u64::from(p.count));
        }
        l.flush(out);
    }
    if let Some(why) = x.stopped {
        l.s("usb:   walk stopped: ").s(why).s(" after ").d(u64::from(x.reads)).s(" reads");
        l.flush(out);
    }
}

/// The last lines of the report, short enough for an 80-column text display: what a person photographing the screen needs.
pub fn render_summary(f: &UsbFn, x: &Xhci, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: SUMMARY xhci ");
    bdf(&mut l, f);
    l.s(" ").hx(u64::from(f.vendor), 4).s(":").hx(u64::from(f.device), 4);
    if x.reads == 0 {
        l.s(" NOT READ");
        l.flush(out);
        return;
    }
    l.s(" v").hx(u64::from(x.version), 4).s(" ports ").d(u64::from(x.ports)).s(" connected ").d(u64::from(x.connected));
    l.s(" slots ").d(u64::from(x.slots())).s(" spad ").d(u64::from(x.scratchpads()));
    l.flush(out);
    l.s("usb:   ");
    match x.legacy {
        Some((_, sup, _)) => l.s("bios-owned ").d(u64::from(sup >> 16 & 1)).s(" os-owned ").d(u64::from(sup >> 24 & 1)),
        None => l.s("no legacy-handoff capability"),
    };
    l.s(" running ").d(u64::from(x.usbcmd & 1)).s(" halted ").d(u64::from(x.usbsts & 1));
    if x.dbc.is_some() {
        l.s(" debug-cap");
    }
    if let Some(why) = x.stopped {
        l.s(" STOPPED: ").s(why);
    }
    l.flush(out);
}

pub fn render_note(msg: &str, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: ").s(msg);
    l.flush(out);
}

// ---- rendering of the Increment 2 additions ----------------------------------------------------------------------------

/// All ports' raw PORTSC (eight per line) with counts of connected, powered, enabled and over-current ports.
pub fn render_xhci_ports(x: &Xhci, out: &mut dyn Out) {
    let shown = usize::from(x.ports_read).min(x.portsc.len());
    let (mut conn, mut power, mut enabled, mut oc) = (0u32, 0u32, 0u32, 0u32);
    for sc in x.portsc.iter().take(shown) {
        conn += sc & 1;
        enabled += sc >> 1 & 1;
        oc += sc >> 3 & 1;
        power += sc >> 9 & 1;
    }
    let mut l = Line::new();
    l.s("usb:   portsc of ").d(shown as u64).s(" of ").d(u64::from(x.ports)).s(" ports: connected ").d(u64::from(conn)).s(" enabled ").d(u64::from(enabled));
    l.s(" powered ").d(u64::from(power)).s(" over-current ").d(u64::from(oc));
    if usize::from(x.ports_read) > shown {
        l.s(" (first 32 shown)");
    }
    l.flush(out);
    for (row, chunk) in x.portsc.get(..shown).unwrap_or(&[]).chunks(8).enumerate() {
        l.s("usb:    ");
        for (k, sc) in chunk.iter().enumerate() {
            l.s(" p").d((row * 8 + k + 1) as u64).s("=").hx(u64::from(*sc), 8);
        }
        l.flush(out);
    }
}

/// Ring pointers with the firmware memory class of the page each names (`class` maps an address to its memory-map type).
pub fn render_pointers(f: &UsbFn, x: &Xhci, class: &dyn Fn(u64) -> &'static str, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: ");
    bdf(&mut l, f);
    if let Some(why) = x.ptr_stopped {
        l.s(" ring pointers not fully read: ").s(why);
        l.flush(out);
    }
    let mut l = Line::new();
    l.s("usb:   dcbaap ").x(x.dcbaap, 8).s(" [").s(class(x.dcbaap)).s("] crcr(lo) ").x(u64::from(x.crcr), 8);
    l.s(" ir0 erstba ").x(x.ir_erstba, 8).s(" [").s(class(x.ir_erstba)).s("] erdp ").x(x.ir_erdp, 8).s(" [").s(class(x.ir_erdp)).s("]");
    l.flush(out);
    l.s("usb:   ir0 iman ").x(u64::from(x.ir_iman), 8).s(" erstsz ").x(u64::from(x.ir_erstsz), 8);
    l.flush(out);
}

pub fn render_routing(f: &UsbFn, r: &Routing, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: ");
    bdf(&mut l, f);
    l.s(" intel routing (pci cfg 0xd0-0xdc, read only): xusb2pr ").x(u64::from(r.xusb2pr), 8).s(" xusb2prm ").x(u64::from(r.xusb2prm), 8);
    l.flush(out);
    l.s("usb:   usb3_pssen ").x(u64::from(r.usb3_pssen), 8).s(" usb3prm ").x(u64::from(r.usb3prm), 8);
    l.flush(out);
    l.s("usb:   usb2 ports routable ").set(r.usb2_routable()).s(": to xhci ").set(r.usb2_to_xhci()).s(", left with ehci ").set(r.usb2_to_ehci());
    l.flush(out);
    l.s("usb:   superspeed ports enabled ").set(r.usb3_enabled());
    l.flush(out);
    l.s("usb:   superspeed ports disabled ").set(r.usb3_disabled()).s("  (bit n = port n+1; datasheet layout, unverified here)");
    l.flush(out);
}

fn ehci_speed(sc: u32) -> &'static str {
    if sc >> 2 & 1 != 0 {
        "hs"
    } else if sc >> 10 & 3 == 1 {
        "ls"
    } else {
        "fs?"
    }
}

pub fn render_ehci(f: &UsbFn, e: &Ehci, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: ehci ");
    bdf(&mut l, f);
    l.s(" ").hx(u64::from(f.vendor), 4).s(":").hx(u64::from(f.device), 4).s(" hci ").x(u64::from(e.version), 4).s(" ports ").d(u64::from(e.ports));
    l.s(" companions ").d(u64::from(e.hcs >> 12 & 0xf)).s(" x ").d(u64::from(e.hcs >> 8 & 0xf)).s(" cmd ").x(u64::from(e.usbcmd), 8).s(" sts ").x(u64::from(e.usbsts), 8);
    l.s(" halted ").d(u64::from(e.halted())).s(" configflag ").d(u64::from(e.configflag & 1));
    l.flush(out);
    if let Some(why) = e.stopped {
        l.s("usb:   walk stopped: ").s(why).s(" after ").d(u64::from(e.reads)).s(" reads");
        l.flush(out);
    }
    let shown = usize::from(e.ports).min(e.portsc.len());
    for (row, chunk) in e.portsc.get(..shown).unwrap_or(&[]).chunks(8).enumerate() {
        l.s("usb:    ");
        for (k, sc) in chunk.iter().enumerate() {
            l.s(" p").d((row * 8 + k + 1) as u64).s("=").hx(u64::from(*sc), 8);
        }
        l.flush(out);
    }
    l.s("usb:   connected ").d(u64::from(e.connected)).s(" companion-owned ").d(u64::from(e.companion_owned));
    if let Some((off, sup, ctl)) = e.legacy {
        l.s(" legacy@").x(u64::from(off), 2).s(" bios ").d(u64::from(sup >> 16 & 1)).s(" os ").d(u64::from(sup >> 24 & 1)).s(" smi ").x(u64::from(ctl), 8);
    } else {
        l.s(" no-legacy");
    }
    l.flush(out);
    // An Intel PCH EHCI normally has two root ports that feed an internal "rate matching hub" (Windows shows it as a Generic
    // USB Hub, Linux as 8087:0024 / 8087:8000); the physical ports hang off that hub. A connected high-speed root port is
    // therefore probably that hub, and the devices behind it, and which XUSB2PR bit they use, are not visible without
    // enumerating it. This is a hypothesis for the owner to check, not something the survey establishes.
    for (i, sc) in e.portsc.iter().take(shown).enumerate() {
        if sc & 1 == 0 {
            continue;
        }
        l.s("usb:   ehci root port ").d(i as u64 + 1).s(" connected ").s(ehci_speed(*sc)).s(" enabled ").d(u64::from(sc >> 2 & 1)).s(" owner ").s(if sc >> 13 & 1 != 0 { "companion" } else { "ehci" });
        l.s(" (a hub behind it? devices below it are not visible)");
        l.flush(out);
    }
}

/// Final-summary line for the four Intel routing registers, exactly as `intel_routing` captured them. `None` means they
/// were not read (not an Intel xHCI, or the function reads all ones), which is different from a register that is zero.
/// Presentation only: it takes the captured values, never a `Pci`, so it cannot read hardware.
pub fn render_summary_routing(r: &Option<Routing>, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: SUMMARY routing raw: ");
    match r {
        Some(r) => {
            l.s("xusb2pr ").x(u64::from(r.xusb2pr), 8).s(" xusb2prm ").x(u64::from(r.xusb2prm), 8);
            l.s(" pssen ").x(u64::from(r.usb3_pssen), 8).s(" usb3prm ").x(u64::from(r.usb3prm), 8);
        }
        None => {
            l.s("not read");
        }
    }
    l.flush(out);
}

/// Root ports shown per final-summary row: six `" pNN=xxxxxxxx"` entries stay within the 118 visible columns.
const SUMMARY_PORTS_PER_ROW: usize = 6;

/// Final-summary lines for one EHCI: controller status, then the captured PORTSC words. A walk that stopped leaves the
/// registers after the stop at their default zero, so those values are reported as unavailable instead of as zeros.
/// Presentation only: it takes the captured `Ehci`, never an `Mmio` or `Pci`, so it cannot read hardware.
pub fn render_summary_ehci(f: &UsbFn, e: &Ehci, out: &mut dyn Out) {
    let mut l = Line::new();
    l.s("usb: SUMMARY ehci ").hx(u64::from(f.device), 4);
    if let Some(why) = e.stopped {
        l.s(" status NOT READ: ").s(why).s(" after ").d(u64::from(e.reads)).s(" reads");
        l.flush(out);
        return;
    }
    l.s(" cmd ").x(u64::from(e.usbcmd), 8).s(" sts ").x(u64::from(e.usbsts), 8).s(" halted ").d(u64::from(e.halted()));
    l.s(" configflag ").d(u64::from(e.configflag & 1)).s(" ports ").d(u64::from(e.ports));
    match e.legacy {
        Some((_, sup, _)) => {
            l.s(" legacy bios ").d(u64::from(sup >> 16 & 1)).s(" os ").d(u64::from(sup >> 24 & 1));
        }
        None => {
            l.s(" no-legacy");
        }
    }
    l.flush(out);
    let shown = usize::from(e.ports).min(e.portsc.len());
    if shown == 0 {
        l.s("usb: SUMMARY ehci ").hx(u64::from(f.device), 4).s(" portsc: none");
        l.flush(out);
    }
    for (row, chunk) in e.portsc.get(..shown).unwrap_or(&[]).chunks(SUMMARY_PORTS_PER_ROW).enumerate() {
        l.s("usb: SUMMARY ehci ").hx(u64::from(f.device), 4).s(" portsc:");
        for (k, sc) in chunk.iter().enumerate() {
            l.s(" p").d((row * SUMMARY_PORTS_PER_ROW + k + 1) as u64).s("=").hx(u64::from(*sc), 8);
        }
        l.flush(out);
    }
}
