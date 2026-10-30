//! USB input bring-up, Increment 1: private C ABI for the read-only PCI and xHCI survey. The C adapters in
//! xhci_survey.c supply configuration reads, register reads through a leased window, and text output; every decision
//! and every line of text comes from `crate::xhci::survey`. The only way to touch the hardware from here is a read.
use crate::xhci::survey::{self, Mmio, Out, Pci, UsbFn, Xhci};

unsafe extern "C" {
    // xhci_survey.h: valid only inside the survey session (kernel CR3, IF=0, non-IRQ thread context).
    fn xhci_survey_pci32(bus: u32, dev: u32, func: u32, off: u32) -> u32;
    fn xhci_survey_bar_acquire(bus: u32, dev: u32, func: u32, base: u64, token: *mut u64) -> i32;
    fn xhci_survey_bar_read32(token: u64, off: u32, value: *mut u32) -> i32;
    fn xhci_survey_bar_release(token: u64) -> i32;
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

const DEVMAP_ERRORS: [&str; 9] = ["ok", "invalid", "overflow", "beyond address width", "RAM", "overlaps a mapping", "mapping limit", "no memory", "bad state"];

/// Command word and both BAR dwords: what a read-only survey must leave exactly as it found it.
fn pci_state(c: &mut Config, f: &UsbFn) -> [u32; 3] {
    [c.read32(f.bus, f.dev, f.func, 4) & 0xffff, c.read32(f.bus, f.dev, f.func, 0x10), c.read32(f.bus, f.dev, f.func, 0x14)]
}

fn survey_one(c: &mut Config, f: &UsbFn, physical_bits: u32, out: &mut Console, x: &mut Xhci) -> i32 {
    let info = survey::pci_info(c, f);
    let bar = f.bar_base();
    survey::render_pci(f, &info, &bar, out);
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
    let mut token = 0u64;
    // SAFETY: exact C prototype; exclusive stack output. C binds the lease to this function's BAR as it reads now.
    let err = unsafe { xhci_survey_bar_acquire(u32::from(f.bus), u32::from(f.dev), u32::from(f.func), base, &mut token) };
    if err != 0 {
        let mut l = survey::Line::new();
        l.s("usb:   register window refused: ").s(DEVMAP_ERRORS.get(err as usize).copied().unwrap_or("unknown")).s(" (not read)");
        l.flush(out);
        x.stopped = Some("window refused");
        return 0;
    }
    *x = survey::inspect_xhci(&mut Window { token });
    // SAFETY: the token comes from the acquisition above; C rechecks the live generation and page tables.
    let released = unsafe { xhci_survey_bar_release(token) };
    survey::render_xhci(f, x, out);
    if released != 0 {
        survey::render_note("  ERROR: the register window could not be released", out);
        return -1;
    }
    if pci_state(c, f) != before {
        survey::render_note("  ERROR: the PCI command or BAR of this controller changed during the survey", out);
        return -1;
    }
    0
}

/// Returns 0 when the survey completed with every window released and no PCI state changed, -1 otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn vos_xhci_survey_v1(physical_bits: u32) -> i32 {
    let mut out = Console;
    let mut c = Config;
    survey::render_note("USB input survey: PCI configuration and xHCI registers are only read; nothing is written", &mut out);
    let scan = survey::scan(&mut c);
    survey::render_scan(&scan, &mut out);
    let mut status = 0;
    let mut inspected = 0usize;
    let mut results = [Xhci::default(); 4];
    let mut which = [UsbFn::default(); 4];
    for f in scan.usb.iter().take(usize::from(scan.recorded)).filter(|f| f.is_xhci()) {
        let (Some(x), Some(w)) = (results.get_mut(inspected), which.get_mut(inspected)) else {
            survey::render_note("more than 4 xHCI controllers: the rest are listed but not read", &mut out);
            break;
        };
        *w = *f;
        status |= survey_one(&mut c, f, physical_bits, &mut out, x);
        inspected += 1;
    }
    if inspected == 0 {
        survey::render_note("no xHCI controller found on the buses that were scanned", &mut out);
    }
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
    l.s("usb: IOMMU: ").d(u64::from(scan.iommu_functions)).s(" PCI IOMMU function(s); ACPI DMAR and IVRS are NOT read, so an Intel or AMD IOMMU is UNKNOWN");
    l.flush(&mut out);
    for (f, x) in which.iter().zip(results.iter()).take(inspected) {
        survey::render_summary(f, x, &mut out);
    }
    if status == 0 { 0 } else { -1 }
}
