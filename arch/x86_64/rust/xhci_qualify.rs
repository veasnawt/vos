//! USB.2-C1 private C ABI: read-only PCI/xHCI qualification of the QEMU
//! profile. Configuration words and BAR registers are read through C adapters
//! bound to one selected function. No PCI configuration or controller register
//! is written, no DMA memory exists, no worker starts and nothing is enumerated.
use crate::xhci::capabilities::{self, Inspection, Words};
use crate::xhci::containment::BUS_MASTER;
use crate::xhci::discovery::{self, Status};
use crate::xhci::qualify::{self, Config, Pci, Reason, State};
use crate::xhci::resources::{Kind, Lease};
use crate::xhci::{BAR_BYTES, Error};
use core::mem::{align_of, offset_of, size_of};

/// Header (7), PAGESIZE, at most 64 xECP headers with legacy (2) or protocol
/// (3) bodies, USBCMD/USBSTS and legacy words twice: well below 256 DWORDs.
const READ_LIMIT: u32 = 256;
/// Revalidation passes (USB.2-C3b.1) each get their own `READ_LIMIT`; at most this many.
#[cfg(not(vos_contain_test))]
const PHASE_LIMIT: u8 = 4;
/// USB.2-C3b.2 test images: two more passes, the reset poll and the post-reset reads.
#[cfg(vos_contain_test)]
const PHASE_LIMIT: u8 = 7;

unsafe extern "C" {
    // xhci_qualify.h: valid only inside the qualification session.
    // Configuration: candidate-only, aligned offsets below 256; read-only.
    fn xhci_qualify_config32(offset: u32, value: *mut u32) -> i32;
    fn xhci_qualify_bar_acquire(base: u64, out: *mut Raw) -> i32;
    fn xhci_qualify_bar_release(token: u64) -> i32;
    fn xhci_qualify_bar_retain(token: u64);
    fn xhci_qualify_bar_read32(token: u64, offset: u32, value: *mut u32) -> i32;
}

/// `struct usb_resource` (usb_resources.h).
#[repr(C)]
#[derive(Default)]
struct Raw {
    token: u64,
    address: u64,
}
const _: () = assert!(size_of::<Raw>() == 16 && align_of::<Raw>() == 8);

/// The read-only BAR lease: a canonical Mapping-kind Lease plus the only
/// access it permits, aligned 32-bit reads. Non-Copy and !Send/!Sync through
/// Lease. The window address is never handed out; there is no write method.
/// Dropping without release retains the C registry entry (no implicit free).
struct BarLease {
    lease: Option<Lease>,
}
impl BarLease {
    /// C binds the lease to the selected function's current BAR; `base` must
    /// equal it, or nothing is acquired.
    fn acquire(base: u64) -> Result<Self, i32> {
        let mut raw = Raw::default();
        // SAFETY: exact C prototype; exclusive initialized stack output. The
        // service checks the session, kernel CR3, IF=0 and non-IRQ context.
        let result = unsafe { xhci_qualify_bar_acquire(base, &mut raw) };
        if result != 0 {
            return Err(result);
        }
        match Lease::new(Kind::Mapping, raw.token, raw.address) {
            Ok(lease) => Ok(Self { lease: Some(lease) }),
            Err(_) => {
                // SAFETY: same private service/owner; never release bad output.
                unsafe { xhci_qualify_bar_retain(raw.token) };
                Err(8)
            }
        }
    }
    fn read32(&self, offset: u32) -> Result<u32, i32> {
        let Some(lease) = &self.lease else { return Err(8) };
        let mut value = 0u32;
        // SAFETY: exact C prototype; exclusive stack output. C revalidates the
        // live token, lease kind, alignment and extent before a volatile load.
        match unsafe { xhci_qualify_bar_read32(lease.token(), offset, &mut value) } {
            0 => Ok(value),
            error => Err(error),
        }
    }
    fn release(&mut self) -> Result<(), i32> {
        let Some(lease) = &self.lease else { return Err(8) };
        // SAFETY: C verifies the live generation, kind and page tables before
        // mutation; failure leaves this handle and the registry intact.
        match unsafe { xhci_qualify_bar_release(lease.token()) } {
            0 => {
                self.lease = None;
                Ok(())
            }
            error => Err(error),
        }
    }
}
impl Drop for BarLease {
    fn drop(&mut self) {
        if let Some(lease) = &self.lease {
            // SAFETY: diagnostic only; the devmap registry keeps the lease.
            unsafe { xhci_qualify_bar_retain(lease.token()) };
        }
    }
}

/// `struct xhci_qualify_report` in xhci_qualify.h; layout asserted on both sides.
#[repr(C)]
pub struct Report {
    reason_text: *const u8,
    base: u64,
    status: i32,
    reason: u32,
    candidates: u32,
    complete: u32,
    vendor: u32,
    device: u32,
    class_revision: u32,
    command_before: u32,
    command_after: u32,
    pci_status: u32,
    bar_low: u32,
    bar_high: u32,
    bar_low_after: u32,
    bar_high_after: u32,
    bar_bits: u32,
    pci_caps: u32,
    pm_offset: u32,
    pmcsr_before: u32,
    pmcsr_after: u32,
    power_state: u32,
    msi_offset: u32,
    msi_control_before: u32,
    msi_control_after: u32,
    msix_offset: u32,
    msix_control_before: u32,
    msix_control_after: u32,
    msix_table: u32,
    msix_pba: u32,
    version: u32,
    caplength: u32,
    hcs1: u32,
    hcs2: u32,
    hcc1: u32,
    doorbell: u32,
    runtime: u32,
    pagesize: u32,
    xecp_headers: u32,
    protocol_ports: u32,
    legacy_offset: u32,
    usblegsup_before: u32,
    usblegsup_after: u32,
    usblegctlsts_before: u32,
    usblegctlsts_after: u32,
    usbcmd_before: u32,
    usbcmd_after: u32,
    usbsts_before: u32,
    usbsts_after: u32,
    mmio_reads: u32,
    lease_acquired: u32,
    lease_released: u32,
    mapping_error: i32,
    usb1_compatible: u32,
}
impl Report {
    pub(crate) fn status(&self) -> i32 {
        self.status
    }
    pub(crate) fn pci_command(&self) -> u32 {
        self.command_before
    }
}
const _: () = {
    assert!(size_of::<Report>() == 224 && align_of::<Report>() == 8);
    assert!(offset_of!(Report, base) == 8 && offset_of!(Report, status) == 16);
    assert!(offset_of!(Report, vendor) == 32 && offset_of!(Report, msix_pba) == 124);
    assert!(offset_of!(Report, version) == 128 && offset_of!(Report, legacy_offset) == 168);
    assert!(offset_of!(Report, usbcmd_before) == 188 && offset_of!(Report, mmio_reads) == 204);
    assert!(offset_of!(Report, usb1_compatible) == 220);
};

pub(crate) struct PciConfig;
impl Config for PciConfig {
    fn read32(&mut self, offset: u8) -> Result<u32, Reason> {
        let mut value = 0u32;
        // SAFETY: exact C prototype; exclusive stack output. C restricts the
        // read to the selected function and an aligned offset below 256.
        match unsafe { xhci_qualify_config32(u32::from(offset), &mut value) } {
            0 => Ok(value),
            _ => Err(Reason::Interface),
        }
    }
}

/// Copies one DWORD at a time from the read-only lease, within the profile
/// extent and a fixed budget. Every value is copied before interpretation.
pub(crate) struct BarWords<'a> {
    lease: &'a BarLease,
    reads: u32,
    limit: u32,
    /// Reads of finished phases and the number of phases begun after the first.
    done: u32,
    phases: u8,
}
impl BarWords<'_> {
    /// Begin a revalidation pass with a fresh per-pass budget; false after `PHASE_LIMIT` passes.
    pub(crate) fn next_phase(&mut self) -> bool {
        if self.phases + 1 >= PHASE_LIMIT {
            return false;
        }
        self.phases += 1;
        self.done += self.reads;
        self.reads = 0;
        self.limit = READ_LIMIT;
        true
    }
    /// USB.2-C3b.2: a new pass with its own budget (the bounded reset poll).
    #[cfg(vos_contain_test)]
    pub(crate) fn phase(&mut self, reads: u32) -> bool {
        self.next_phase() && {
            self.limit = reads;
            true
        }
    }
    /// The live lease token, for the one USBCMD write adapter.
    #[cfg(vos_contain_test)]
    pub(crate) fn token(&self) -> u64 {
        self.lease.lease.as_ref().map_or(0, |l| l.token())
    }
    pub(crate) fn total_reads(&self) -> u32 {
        self.done + self.reads
    }
}
impl Words for BarWords<'_> {
    fn word(&mut self, offset: u32) -> Result<u32, Error> {
        if offset % 4 != 0 {
            return Err(Error::Alignment);
        }
        if offset >= BAR_BYTES {
            return Err(Error::Truncated);
        }
        if self.reads == self.limit {
            return Err(Error::Budget);
        }
        self.reads += 1;
        self.lease.read32(offset).map_err(|_| Error::Backend)
    }
}

fn record_pci(r: &mut Report, p: &Pci) {
    r.vendor = u32::from(p.vendor);
    r.device = u32::from(p.device);
    r.class_revision = p.class_revision;
    r.command_before = u32::from(p.state.command);
    r.pci_status = u32::from(p.status);
    r.bar_low = p.state.bar_low;
    r.bar_high = p.state.bar_high;
    r.base = p.base;
    r.bar_bits = u32::from(p.bar_bits);
    r.pci_caps = u32::from(p.capabilities);
    r.pm_offset = u32::from(p.pm);
    r.pmcsr_before = u32::from(p.state.pmcsr);
    r.power_state = u32::from(p.power_state);
    r.msi_offset = u32::from(p.msi);
    r.msi_control_before = u32::from(p.state.msi_control);
    r.msix_offset = u32::from(p.msix);
    r.msix_control_before = u32::from(p.state.msix_control);
    r.msix_table = p.msix_table;
    r.msix_pba = p.msix_pba;
}

fn record_after(r: &mut Report, s: &State) {
    r.command_after = u32::from(s.command);
    r.bar_low_after = s.bar_low;
    r.bar_high_after = s.bar_high;
    r.pmcsr_after = u32::from(s.pmcsr);
    r.msi_control_after = u32::from(s.msi_control);
    r.msix_control_after = u32::from(s.msix_control);
}

fn record_inspection(r: &mut Report, i: &Inspection) {
    r.version = i.header[0] >> 16;
    r.caplength = i.operational;
    r.hcs1 = i.header[1];
    r.hcs2 = i.header[2];
    r.hcc1 = i.header[4];
    r.doorbell = i.doorbell;
    r.runtime = i.runtime;
    r.pagesize = i.pagesize;
    r.xecp_headers = u32::from(i.extended);
    r.protocol_ports = i.protocol_ports;
    if let Some(l) = i.legacy {
        r.legacy_offset = l.offset;
        r.usblegsup_before = l.usblegsup;
        r.usblegctlsts_before = l.usblegctlsts;
    }
}

/// Runs while the lease is held, after the first and before the second
/// observation of USBCMD/USBSTS/legacy words, so their stability check spans
/// it. Reads only: `BarWords` has no write operation.
/// A hook returns the PCI command bits it verifiably cleared: always 0, except
/// USB.2-C3b.2's bus-master clear, the only change the re-observation accepts.
pub(crate) type Hook<'h> = &'h mut dyn FnMut(&mut BarWords<'_>, &Inspection) -> Result<u16, Reason>;

/// Everything that touches the leased BAR. Second observations of USBCMD,
/// USBSTS and legacy-support words follow the walk, before release.
fn inspect_bar(w: &mut BarWords<'_>, r: &mut Report, hook: Hook<'_>) -> Result<u16, Reason> {
    let i = capabilities::inspect(w).map_err(Reason::from)?;
    record_inspection(r, &i);
    // USB.1's accepted decoder must also accept the same copied header.
    let mut usb1 = discovery::Report::default();
    if discovery::decode_caps(Some(i.header), &mut usb1) != Status::Ok {
        return Err(Reason::Usb1Compatibility);
    }
    r.usb1_compatible = 1;
    let op = i.operational; // inspect proved op + 0x400 lies inside the extent
    r.usbcmd_before = w.word(op).map_err(Reason::from)?;
    r.usbsts_before = w.word(op + 4).map_err(Reason::from)?;
    if let Some(l) = i.legacy {
        r.usblegsup_after = w.word(l.offset).map_err(Reason::from)?;
        r.usblegctlsts_after = w.word(l.offset + 4).map_err(Reason::from)?;
    }
    let cleared = hook(w, &i)?;
    r.usbcmd_after = w.word(op).map_err(Reason::from)?;
    r.usbsts_after = w.word(op + 4).map_err(Reason::from)?;
    Ok(cleared)
}

fn finish(r: &mut Report, status: i32, reason: Reason) -> i32 {
    r.status = status;
    r.reason = reason as u32;
    r.reason_text = reason.c_string().as_ptr();
    status
}

pub(crate) fn qualify(candidates: u32, complete: bool, physical_bits: u32, r: &mut Report, hook: Hook<'_>) -> i32 {
    r.candidates = candidates;
    r.complete = u32::from(complete);
    if let Err(reason) = qualify::select(candidates, complete) {
        return finish(r, 1, reason);
    }
    let pci = match qualify::inspect_pci(&mut PciConfig, physical_bits) {
        Ok(pci) => pci,
        Err(reason) => {
            r.base = 0;
            return finish(r, 1, reason);
        }
    };
    record_pci(r, &pci);
    let mut lease = match BarLease::acquire(pci.base) {
        Ok(lease) => lease,
        Err(error) => {
            r.mapping_error = error;
            return finish(r, 1, Reason::Mapping);
        }
    };
    r.lease_acquired = 1;
    let mut words = BarWords { lease: &lease, reads: 0, limit: READ_LIMIT, done: 0, phases: 0 };
    let inspected = inspect_bar(&mut words, r, hook);
    r.mmio_reads = words.total_reads();
    // Release before any PCI re-observation; failure retains the lease (Drop).
    if lease.release().is_err() {
        return finish(r, -1, Reason::Release);
    }
    r.lease_released = 1;
    let cleared = *inspected.as_ref().unwrap_or(&0);
    let mut expected = pci.state;
    expected.command &= !cleared;
    match qualify::observe(&mut PciConfig, &pci) {
        Ok(after) => {
            record_after(r, &after);
            if cleared & !BUS_MASTER != 0 || after != expected {
                return finish(r, -1, Reason::StateChanged);
            }
        }
        Err(reason) => return finish(r, -1, reason),
    }
    if let Err(reason) = inspected {
        // Integrity faults outrank profile rejections; other refusals are a
        // fail-closed qualification result with nothing changed or retained.
        return finish(r, if reason == Reason::StateChanged { -1 } else { 1 }, reason);
    }
    if r.usbcmd_before != r.usbcmd_after
        || r.usbsts_before != r.usbsts_after
        || r.usblegsup_before != r.usblegsup_after
        || r.usblegctlsts_before != r.usblegctlsts_after
    {
        return finish(r, -1, Reason::StateChanged);
    }
    finish(r, 0, Reason::Qualified)
}

/// All-zero initial report (reason_text null until `finish`).
pub(crate) const ZERO: Report = Report {
    reason_text: core::ptr::null(),
    base: 0,
    status: 0,
    reason: 0,
    candidates: 0,
    complete: 0,
    vendor: 0,
    device: 0,
    class_revision: 0,
    command_before: 0,
    command_after: 0,
    pci_status: 0,
    bar_low: 0,
    bar_high: 0,
    bar_low_after: 0,
    bar_high_after: 0,
    bar_bits: 0,
    pci_caps: 0,
    pm_offset: 0,
    pmcsr_before: 0,
    pmcsr_after: 0,
    power_state: 0,
    msi_offset: 0,
    msi_control_before: 0,
    msi_control_after: 0,
    msix_offset: 0,
    msix_control_before: 0,
    msix_control_after: 0,
    msix_table: 0,
    msix_pba: 0,
    version: 0,
    caplength: 0,
    hcs1: 0,
    hcs2: 0,
    hcc1: 0,
    doorbell: 0,
    runtime: 0,
    pagesize: 0,
    xecp_headers: 0,
    protocol_ports: 0,
    legacy_offset: 0,
    usblegsup_before: 0,
    usblegsup_after: 0,
    usblegctlsts_before: 0,
    usblegctlsts_after: 0,
    usbcmd_before: 0,
    usbcmd_after: 0,
    usbsts_before: 0,
    usbsts_after: 0,
    mmio_reads: 0,
    lease_acquired: 0,
    lease_released: 0,
    mapping_error: 0,
    usb1_compatible: 0,
};

/// Returns 0 qualified, 1 rejected (fail closed, nothing retained), -1 for an
/// integrity failure: state changed or the lease could not be released.
// SAFETY: unique private symbol declared in xhci_qualify.h. `out` must be
// exclusive writable storage for the asserted layout; it is fully initialized
// in place before any field is read. Called only by xhci_qualify_boot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_xhci_qualify_v1(
    candidates: u32,
    complete: u32,
    physical_bits: u32,
    out: *mut Report,
) -> i32 {
    if out.is_null() || !(out as usize).is_multiple_of(align_of::<Report>()) {
        return -2;
    }
    // SAFETY: checked non-null/aligned exclusive output. It is initialized in
    // place, whole, before the only reference is formed; caller bytes are
    // never read. No local copy keeps the frame inside the stack budget.
    let r = unsafe {
        out.write(ZERO);
        &mut *out
    };
    qualify(candidates, complete != 0, physical_bits, r, &mut |_, _| Ok(0))
}
