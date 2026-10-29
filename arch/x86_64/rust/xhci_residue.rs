//! USB.2-C3a private C ABI: firmware DMA residue guard and dry-run lifecycle
//! for the qualified QEMU profile. Registers are read through the C1 lease;
//! the only effects are PMM reservations of firmware-owned frames. Nothing
//! writes a PCI or controller register and nothing executes the plan.
use crate::xhci::Error;
use crate::xhci::capabilities::{Inspection, Words};
use crate::xhci::lifecycle::{self, Facts, PLAN_LIMIT};
use crate::xhci::qualify::Reason;
use crate::xhci::residue::{self, Interrupter, Memory, Outcome, Page, Registers, Role};
use crate::xhci_qualify::{self, BarWords, Report, ZERO};
use core::mem::{align_of, offset_of, size_of};

unsafe extern "C" {
    // xhci_qualify.h: valid only inside the residue session.
    fn xhci_residue_page(frame: u64) -> i32;
    fn xhci_residue_reserve(frame: u64, role: u32) -> i32;
    fn xhci_residue_read64(phys: u64, value: *mut u64) -> i32;
}

/// `struct xhci_residue_report` in xhci_qualify.h.
#[repr(C)]
pub struct ResidueReport {
    q: Report,
    crcr: u64,
    dcbaap: u64,
    erstba0: u64,
    erdp0: u64,
    status: i32,
    usbcmd: u32,
    usbsts: u32,
    config: u32,
    iman0: u32,
    erstsz0: u32,
    findings: u32,
    reserved: u32,
    owned_again: u32,
    protected_pages: u32,
    hazards: u32,
    pages: u32,
    interrupters: u32,
    interrupter_enables: u32,
    refusal: u32,
    violation: u32,
    plan_len: u32,
    plan: [u8; PLAN_LIMIT],
    excluded: u32,
    /// Bit 0: reuse protected; bit 1: inspection complete (USB.2-C3a.3).
    inspection: u32,
}
const _: () = {
    assert!(size_of::<ResidueReport>() == 360 && align_of::<ResidueReport>() == 8);
    assert!(offset_of!(ResidueReport, excluded) == 348 && offset_of!(ResidueReport, inspection) == 352);
    assert!(offset_of!(ResidueReport, crcr) == 224 && offset_of!(ResidueReport, status) == 256);
    assert!(offset_of!(ResidueReport, refusal) == 312 && offset_of!(ResidueReport, plan) == 324);
};

/// Registers and frames through the C1 lease and the C residue registry.
struct Kernel<'a, 'b> {
    words: &'a mut BarWords<'b>,
    runtime: u32,
}
impl Kernel<'_, '_> {
    fn reg64(&mut self, offset: u32) -> Result<u64, Error> {
        let low = self.words.word(offset)?;
        Ok(u64::from(low) | (u64::from(self.words.word(offset + 4)?) << 32))
    }
}
impl Memory for Kernel<'_, '_> {
    fn page(&mut self, frame: u64) -> Page {
        // SAFETY: scalar argument; C classifies from the PMM bitmaps.
        match unsafe { xhci_residue_page(frame) } {
            0 => Page::Free,
            1 => Page::Owned,
            2 => Page::Allocated,
            4 => Page::Untracked,
            5 => Page::Excluded,
            _ => Page::Protected,
        }
    }
    fn reserve(&mut self, frame: u64, role: Role) -> Result<(), Error> {
        // SAFETY: scalar arguments; C refuses allocated, protected or duplicate frames.
        match unsafe { xhci_residue_reserve(frame, role as u32) } {
            0 => Ok(()),
            -2 => Err(Error::Budget),
            _ => Err(Error::Backend),
        }
    }
    fn read64(&mut self, phys: u64) -> Result<u64, Error> {
        let mut value = 0u64;
        // SAFETY: exclusive stack output; C reads only inside guard-owned frames.
        match unsafe { xhci_residue_read64(phys, &mut value) } {
            0 => Ok(value),
            _ => Err(Error::Backend),
        }
    }
    fn interrupter(&mut self, index: u8) -> Result<Interrupter, Error> {
        let base = self.runtime + 0x20 + 32 * u32::from(index);
        Ok(Interrupter {
            iman: self.words.word(base)?,
            erstsz: self.words.word(base + 8)?,
            erstba: self.reg64(base + 0x10)?,
            erdp: self.reg64(base + 0x18)?,
        })
    }
}

#[derive(Default)]
struct Found {
    regs: Registers,
    outcome: Outcome,
    legacy: bool,
    legacy_os_owned: bool,
    first: Interrupter,
}

/// Read the residue registers (inspect proved op + 0x400 and the runtime
/// block lie inside the extent), then assess and protect.
fn inspect(w: &mut BarWords<'_>, i: &Inspection, bits: u32, found: &mut Found) -> Result<(), Reason> {
    let op = i.operational;
    let mut k = Kernel { words: w, runtime: i.runtime };
    let read = |k: &mut Kernel<'_, '_>, off| k.words.word(off).map_err(Reason::from);
    found.regs = Registers {
        usbcmd: read(&mut k, op)?,
        usbsts: read(&mut k, op + 4)?,
        crcr: k.reg64(op + 0x18).map_err(Reason::from)?,
        dcbaap: k.reg64(op + 0x30).map_err(Reason::from)?,
        config: read(&mut k, op + 0x38)?,
        hcs1: i.header[1],
        hcs2: i.header[2],
        hcc1: i.header[4],
        interrupters: i.interrupters.min(u16::from(residue::INTERRUPTER_LIMIT)) as u8,
        physical_bits: bits as u8,
    };
    found.first = k.interrupter(0).map_err(Reason::from)?;
    found.outcome = residue::assess(&mut k, &found.regs);
    if let Some(l) = i.legacy {
        found.legacy = true;
        found.legacy_os_owned = l.os_owned() && !l.bios_owned();
    }
    Ok(())
}

fn finish(r: &mut ResidueReport, f: &Found) -> i32 {
    let o = &f.outcome;
    r.usbcmd = f.regs.usbcmd;
    r.usbsts = f.regs.usbsts;
    r.config = f.regs.config;
    r.crcr = f.regs.crcr;
    r.dcbaap = f.regs.dcbaap;
    r.iman0 = f.first.iman;
    r.erstsz0 = f.first.erstsz;
    r.erstba0 = f.first.erstba;
    r.erdp0 = f.first.erdp;
    r.findings = u32::from(o.findings);
    r.reserved = u32::from(o.reserved);
    r.owned_again = u32::from(o.owned_again);
    r.protected_pages = u32::from(o.protected);
    r.hazards = u32::from(o.hazards);
    r.pages = u32::from(o.pages);
    r.excluded = u32::from(o.excluded);
    r.inspection = u32::from(o.reuse_protected()) | u32::from(o.inspection_complete()) << 1;
    r.interrupters = u32::from(f.regs.interrupters);
    r.interrupter_enables = u32::from(o.interrupter_enables);
    let facts = Facts {
        qualified: r.q.status() == 0,
        residue_complete: o.complete(),
        halted: f.regs.usbsts & 1 != 0,
        run_stop: f.regs.usbcmd & 1 != 0,
        bus_master: r.q.pci_command() & 4 != 0,
        controller_ready: f.regs.usbsts & (1 << 11) == 0,
        interrupter_enables: o.interrupter_enables,
        legacy_present: f.legacy,
        legacy_os_owned: f.legacy_os_owned,
    };
    match lifecycle::plan(&facts) {
        Ok(plan) => {
            for (slot, step) in r.plan.iter_mut().zip(plan.steps()) {
                *slot = step.code();
            }
            r.plan_len = plan.steps().len() as u32;
            r.violation = match lifecycle::validate(plan.steps(), &facts) {
                Ok(()) => 0,
                Err(v) => v.code(),
            };
            r.status = if r.violation == 0 { 0 } else { 1 };
        }
        Err(refusal) => {
            r.refusal = refusal as u32 + 1;
            r.status = if o.hazards != 0 { -1 } else { 1 };
        }
    }
    r.status
}

/// Qualification status as for C1 (0 qualified, 1 rejected, -1 integrity);
/// the residue verdict is in the report. -2 for a bad output pointer.
// SAFETY: unique private symbol declared in xhci_qualify.h. `out` must be
// exclusive writable storage for the asserted layout; it is fully initialized
// in place before any field is read. Called once by xhci_residue_boot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_xhci_residue_v1(
    candidates: u32,
    complete: u32,
    physical_bits: u32,
    out: *mut ResidueReport,
) -> i32 {
    if out.is_null() || !(out as usize).is_multiple_of(align_of::<ResidueReport>()) {
        return -2;
    }
    // SAFETY: checked non-null/aligned exclusive output, written whole before
    // the only reference is formed; caller bytes are never read.
    let r = unsafe {
        out.write(ResidueReport {
            q: ZERO,
            crcr: 0,
            dcbaap: 0,
            erstba0: 0,
            erdp0: 0,
            status: 1,
            usbcmd: 0,
            usbsts: 0,
            config: 0,
            iman0: 0,
            erstsz0: 0,
            findings: 0,
            reserved: 0,
            owned_again: 0,
            protected_pages: 0,
            hazards: 0,
            pages: 0,
            interrupters: 0,
            interrupter_enables: 0,
            refusal: 0,
            violation: 0,
            plan_len: 0,
            plan: [0; PLAN_LIMIT],
            excluded: 0,
            inspection: 0,
        });
        &mut *out
    };
    let mut found = Found::default();
    let mut ran = false;
    let q = xhci_qualify::qualify(candidates, complete != 0, physical_bits, &mut r.q, &mut |w, i| {
        ran = true;
        inspect(w, i, physical_bits, &mut found)
    });
    if ran {
        finish(r, &found);
    } else {
        r.refusal = lifecycle::Refusal::Unqualified as u32 + 1;
    }
    q
}
