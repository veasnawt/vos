//! USB input bring-up, Increments 1-2: private C ABI for the read-only PCI, xHCI, EHCI and firmware-table survey. The C
//! adapters in xhci_survey.c supply configuration reads, register reads through leased windows, read-only firmware-table
//! reads and text output; every decision and every line of text comes from `crate::xhci::survey` and
//! `crate::xhci::firmware`. The only way to touch the hardware from here is a read. Nothing here depends on whether the
//! machine booted from Legacy BIOS or UEFI: the Multiboot2 information block is the same, and the controllers are PCI.
use crate::xhci::firmware::{self, Cover, MemMap, Phys, Tables, UnitRegs};
use crate::xhci::survey::{self, Ehci, Mmio, Out, Pci, Routing, UsbFn, Xhci};

unsafe extern "C" {
    // xhci_survey.h: valid only inside the survey session (kernel CR3, IF=0, non-IRQ thread context).
    fn xhci_survey_pci32(bus: u32, dev: u32, func: u32, off: u32) -> u32;
    fn xhci_survey_bar_acquire(bus: u32, dev: u32, func: u32, base: u64, limit: u32, token: *mut u64) -> i32;
    fn xhci_survey_mmio_acquire(base: u64, token: *mut u64) -> i32;
    fn xhci_survey_bar_read32(token: u64, off: u32, value: *mut u32) -> i32;
    fn xhci_survey_bar_release(token: u64) -> i32;
    fn xhci_survey_phys_read(pa: u64, out: *mut u8, len: u32) -> i32;
    fn xhci_survey_puts(text: *const u8, len: u32);
    fn xhci_survey_i8042_status() -> u32;
}

/// Configuration reads of any function: the C side rejects unaligned and out-of-range requests.
struct Config;
impl Pci for Config {
    fn read32(&mut self, bus: u8, dev: u8, func: u8, off: u8) -> u32 {
        // SAFETY: exact C prototype; scalar arguments only; the service is read-only.
        unsafe { xhci_survey_pci32(u32::from(bus), u32::from(dev), u32::from(func), u32::from(off)) }
    }
}

/// Aligned 32-bit reads of the live lease, which C revalidates (token, kind, alignment, extent) on every read.
struct Window {
    token: u64,
}
impl Mmio for Window {
    fn read32(&mut self, off: u32) -> Option<u32> {
        let mut value = 0u32;
        // SAFETY: exact C prototype; exclusive initialized stack output.
        match unsafe { xhci_survey_bar_read32(self.token, off, &mut value) } {
            0 => Some(value),
            _ => None,
        }
    }
}

struct Console;
impl Out for Console {
    fn put(&mut self, text: &[u8]) {
        // SAFETY: a live slice and its exact length; C only reads it, byte by byte.
        unsafe { xhci_survey_puts(text.as_ptr(), text.len() as u32) }
    }
}

/// Firmware-table memory, read-only; C refuses everything the memory map does not call reserved or ACPI memory.
struct FwPhys;
impl Phys for FwPhys {
    fn read(&mut self, pa: u64, out: &mut [u8]) -> bool {
        // SAFETY: a live exclusive slice and its exact length; C writes at most `len` bytes into it.
        unsafe { xhci_survey_phys_read(pa, out.as_mut_ptr(), out.len() as u32) == 0 }
    }
}

const DEVMAP_ERRORS: [&str; 9] = ["ok", "invalid", "overflow", "beyond address width", "RAM", "overlaps a mapping", "mapping limit", "no memory", "bad state"];

fn refused(err: i32) -> &'static str {
    usize::try_from(err).ok().and_then(|i| DEVMAP_ERRORS.get(i)).copied().unwrap_or("unknown")
}

/// Command word and both BAR dwords: what a read-only survey must leave exactly as it found it.
fn pci_state(c: &mut Config, f: &UsbFn) -> [u32; 3] {
    [c.read32(f.bus, f.dev, f.func, 4) & 0xffff, c.read32(f.bus, f.dev, f.func, 0x10), c.read32(f.bus, f.dev, f.func, 0x14)]
}

/// Lease a window, run `read` on it, release it. `Err` carries the refusal text; the `bool` is whether the release worked.
fn read_bar<T>(f: &UsbFn, base: u64, limit: u32, read: impl FnOnce(&mut Window) -> T) -> Result<(T, bool), &'static str> {
    let mut token = 0u64;
    // SAFETY: exact C prototype; exclusive stack output. C binds the lease to this function's BAR as it reads now.
    let err = unsafe { xhci_survey_bar_acquire(u32::from(f.bus), u32::from(f.dev), u32::from(f.func), base, limit, &mut token) };
    if err != 0 {
        return Err(refused(err));
    }
    let value = read(&mut Window { token });
    // SAFETY: the token comes from the acquisition above; C rechecks the live generation and page tables.
    let released = unsafe { xhci_survey_bar_release(token) } == 0;
    Ok((value, released))
}

fn read_unit<T>(base: u64, read: impl FnOnce(&mut Window) -> T) -> Option<(T, bool)> {
    if !firmware::unit_base_ok(base) {
        return None;
    }
    let mut token = 0u64;
    // SAFETY: exact C prototype; exclusive stack output. C refuses anything outside the chipset window and RAM.
    if unsafe { xhci_survey_mmio_acquire(base, &mut token) } != 0 {
        return None;
    }
    let value = read(&mut Window { token });
    // SAFETY: the token comes from the acquisition above.
    let released = unsafe { xhci_survey_bar_release(token) } == 0;
    Some((value, released))
}

fn survey_one(c: &mut Config, f: &UsbFn, physical_bits: u32, map: &MemMap, out: &mut Console, x: &mut Xhci) -> i32 {
    let info = survey::pci_info(c, f);
    let bar = f.bar_base();
    survey::render_pci(f, &info, &bar, out);
    if let Some(r) = survey::intel_routing(c, f) {
        survey::render_routing(f, &r, out);
    }
    let Ok(base) = bar else {
        x.stopped = Some("BAR unusable");
        return 0;
    };
    if physical_bits < 64 && (base + u64::from(survey::WINDOW)) >> physical_bits != 0 {
        survey::render_note("  register window beyond the CPU's physical address width; not read", out);
        x.stopped = Some("beyond address width");
        return 0;
    }
    let before = pci_state(c, f);
    let (got, released) = match read_bar(f, base, survey::WINDOW, |w| survey::inspect_xhci(w)) {
        Ok(v) => v,
        Err(why) => {
            let mut l = survey::Line::new();
            l.s("usb:   register window refused: ").s(why).s(" (not read)");
            l.flush(out);
            x.stopped = Some("window refused");
            return 0;
        }
    };
    *x = got;
    survey::render_xhci(f, x, out);
    survey::render_xhci_ports(x, out);
    let class = |a: u64| -> &'static str {
        if a == 0 {
            "none"
        } else if map.is_empty() {
            "memory map unknown"
        } else {
            map.kind(a).name()
        }
    };
    survey::render_pointers(f, x, &class, out);
    if !released {
        survey::render_note("  ERROR: the register window could not be released", out);
        return -1;
    }
    if pci_state(c, f) != before {
        survey::render_note("  ERROR: the PCI command or BAR of this controller changed during the survey", out);
        return -1;
    }
    0
}

fn survey_ehci(c: &mut Config, f: &UsbFn, out: &mut Console, e: &mut Ehci) -> i32 {
    let base = match f.ehci_base() {
        Ok(b) => b,
        Err(why) => {
            let mut l = survey::Line::new();
            l.s("usb: ehci ");
            l.hx(u64::from(f.bus), 2).s(":").hx(u64::from(f.dev), 2).s(".").d(u64::from(f.func)).s(" bar0 unusable: ").s(why);
            l.flush(out);
            return 0;
        }
    };
    let before = pci_state(c, f);
    // The 1 KiB register file is inside one page; reads are confined to its first 0x100 bytes.
    let (got, released) = match read_bar(f, base, survey::EHCI_WINDOW, |w| {
        let mut p = Config;
        survey::inspect_ehci(w, &mut p, f)
    }) {
        Ok(v) => v,
        Err(why) => {
            let mut l = survey::Line::new();
            l.s("usb: ehci ");
            l.hx(u64::from(f.bus), 2).s(":").hx(u64::from(f.dev), 2).s(".").d(u64::from(f.func)).s(" register window refused: ").s(why).s(" (not read)");
            l.flush(out);
            return 0;
        }
    };
    *e = got;
    survey::render_ehci(f, e, out);
    if !released {
        survey::render_note("  ERROR: the EHCI register window could not be released", out);
        return -1;
    }
    if pci_state(c, f) != before {
        survey::render_note("  ERROR: the PCI command or BAR of this EHCI changed during the survey", out);
        return -1;
    }
    0
}

/// Everything about ACPI IOMMU tables and the remapping units, as four separate facts.
#[inline(never)]
fn survey_dma(rsdp: &Result<firmware::Rsdp, &'static str>, target: (u8, u8, u8), out: &mut Console) -> (firmware::DmaFacts, i32) {
    let mut phys = FwPhys;
    let tables: Option<Tables> = rsdp.as_ref().ok().map(|r| firmware::walk_acpi(&mut phys, r));
    firmware::render_acpi(rsdp, tables.as_ref(), out);
    let mut status = 0;
    let mut units = [(Cover::No, UnitRegs::default(), false); firmware::UNIT_LIMIT];
    let mut n = 0usize;
    let mut found = 0u8;
    if let Some(t) = &tables {
        if let Some(pa) = t.dmar {
            let d = firmware::parse_dmar(&mut phys, pa, target);
            firmware::render_dmar(&d, out);
            found = found.saturating_add(d.units_total);
            for (i, u) in d.units.iter().take(usize::from(d.n_units)).enumerate() {
                let regs = read_unit(u.base, firmware::read_intel_unit);
                if matches!(&regs, Some((_, false))) {
                    status = -1;
                }
                firmware::render_unit(i, "intel", u.base, u.flags, Some(u.cover), regs.as_ref().map(|r| &r.0), out);
                if let (Some((r, _)), Some(slot)) = (regs, units.get_mut(n)) {
                    *slot = (u.cover, r, false);
                    n += 1;
                }
            }
        }
        if let Some(pa) = t.ivrs {
            let v = firmware::parse_ivrs(&mut phys, pa);
            let mut l = survey::Line::new();
            l.s("usb: ivrs units ").d(u64::from(v.units_total)).s(" memory-definition blocks ").d(u64::from(v.ivmd));
            if let Some(why) = v.err {
                l.s(" ERROR: ").s(why);
            }
            l.flush(out);
            found = found.saturating_add(v.units_total);
            for (i, base) in v.bases.iter().take(usize::from(v.n_units)).enumerate() {
                let regs = read_unit(*base, firmware::read_amd_unit);
                if matches!(&regs, Some((_, false))) {
                    status = -1;
                }
                firmware::render_unit(i, "amd", *base, 0, None, regs.as_ref().map(|r| &r.0), out);
                // AMD device lists are not resolved: whether the xHCI is covered stays unknown.
                if let (Some((r, _)), Some(slot)) = (regs, units.get_mut(n)) {
                    *slot = (Cover::Unknown, r, true);
                    n += 1;
                }
            }
        }
    }
    let facts = match &tables {
        Some(t) => firmware::dma_facts(t, units.get(..n).unwrap_or(&[]), found),
        None => firmware::DmaFacts::default(),
    };
    firmware::render_dma(&facts, out);
    (facts, status)
}

fn summary_lines(routing: &Option<Routing>, ehci: &[(UsbFn, Ehci)], facts: &firmware::DmaFacts, out: &mut Console) {
    let mut l = survey::Line::new();
    l.s("usb: SUMMARY usb2 routing: ");
    match routing {
        Some(r) => {
            l.s("to xhci ").set(r.usb2_to_xhci()).s(" left with ehci ").set(r.usb2_to_ehci());
        }
        None => {
            l.s("not read");
        }
    }
    l.flush(out);
    l.s("usb: SUMMARY ehci connected root ports:");
    let mut any = false;
    for (f, e) in ehci {
        any = true;
        l.s(" ").hx(u64::from(f.device), 4).s("=").d(u64::from(e.connected));
    }
    if !any {
        l.s(" none");
    }
    l.flush(out);
    l.s("usb: SUMMARY dma: table ").s(facts.table_present.name()).s(" hw ").s(facts.hardware_responds.name()).s(" on ").s(facts.translation_enabled.name());
    l.s(" xhci ").s(facts.xhci_translated.name());
    l.flush(out);
    l.s("usb: SUMMARY DMA isolation NOT ESTABLISHED");
    l.flush(out);
}

/// Returns 0 when the survey completed with every window released and no PCI state changed, -1 otherwise.
// SAFETY: `info` is null or points to `info_len` readable bytes of the Multiboot2 information block (direct-mapped memory
// that bootinfo validated); the survey only reads them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_xhci_survey_v2(physical_bits: u32, info: *const u8, info_len: u32) -> i32 {
    let mut out = Console;
    let mut c = Config;
    survey::render_note("USB input survey v2: PCI configuration, xHCI, EHCI and firmware tables are only read; nothing is written", &mut out);
    let scan = survey::scan(&mut c);
    survey::render_scan(&scan, &mut out);
    let block: &[u8] = if info.is_null() || info_len == 0 {
        &[]
    } else {
        // SAFETY: see the function contract; length bounded by C to 1 MiB.
        unsafe { core::slice::from_raw_parts(info, info_len as usize) }
    };
    let (map, rsdp) = firmware::parse_info(block);
    let mut status = 0;
    let mut inspected = 0usize;
    let mut results = [Xhci::default(); 4];
    let mut which = [UsbFn::default(); 4];
    let mut routing = None;
    let mut target = (0u8, 0x14u8, 0u8);
    for f in scan.usb.iter().take(usize::from(scan.recorded)).filter(|f| f.is_xhci()) {
        let (Some(x), Some(w)) = (results.get_mut(inspected), which.get_mut(inspected)) else {
            survey::render_note("more than 4 xHCI controllers: the rest are listed but not read", &mut out);
            break;
        };
        if inspected == 0 {
            target = (f.bus, f.dev, f.func);
            routing = survey::intel_routing(&mut c, f);
        }
        *w = *f;
        status |= survey_one(&mut c, f, physical_bits, &map, &mut out, x);
        inspected += 1;
    }
    if inspected == 0 {
        survey::render_note("no xHCI controller found on the buses that were scanned", &mut out);
    }
    let mut ehci = [(UsbFn::default(), Ehci::default()); 4];
    let mut n_ehci = 0usize;
    for f in scan.usb.iter().take(usize::from(scan.recorded)).filter(|f| f.is_ehci()) {
        let Some(slot) = ehci.get_mut(n_ehci) else {
            survey::render_note("more than 4 EHCI controllers: the rest are listed but not read", &mut out);
            break;
        };
        slot.0 = *f;
        status |= survey_ehci(&mut c, f, &mut out, &mut slot.1);
        n_ehci += 1;
    }
    let (facts, dma_status) = survey_dma(&rsdp, target, &mut out);
    status |= dma_status;
    let mut l = survey::Line::new();
    // SAFETY: exact C prototype, no arguments; reading the legacy controller's status port has no side effect.
    let sts = unsafe { xhci_survey_i8042_status() };
    l.s("usb: i8042 status ").x(u64::from(sts), 2);
    if sts == 0xff {
        l.s(" (no legacy keyboard controller answers)");
    } else {
        l.s(" (output buffer ").d(u64::from(sts & 1)).s(", input buffer ").d(u64::from(sts >> 1 & 1)).s(")");
    }
    l.flush(&mut out);
    let mut l = survey::Line::new();
    l.s("usb: IOMMU: ").d(u64::from(scan.iommu_functions)).s(" PCI IOMMU function(s) of class 08/06; ACPI IOMMU tables are reported above");
    l.flush(&mut out);
    for (f, x) in which.iter().zip(results.iter()).take(inspected) {
        survey::render_summary(f, x, &mut out);
    }
    summary_lines(&routing, ehci.get(..n_ehci).unwrap_or(&[]), &facts, &mut out);
    if status == 0 { 0 } else { -1 }
}
