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
    /// Base of memory BAR 0: `Err` says why there is none usable.
    pub fn bar_base(&self) -> Result<u64, &'static str> {
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
        if base % 4096 != 0 {
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
}
impl<M: Mmio> Reader<'_, M> {
    fn word(&mut self, off: u32) -> Result<u32, &'static str> {
        if off % 4 != 0 {
            return Err("unaligned offset");
        }
        if off > WINDOW - 4 {
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
    let mut r = Reader { m, reads: 0 };
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
    x.usbcmd2 = r.word(op)?;
    x.usbsts2 = r.word(op + 4)?;
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
