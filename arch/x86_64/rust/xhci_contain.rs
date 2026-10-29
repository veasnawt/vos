//! USB.2-C3b.1 private C ABI: read-only rehearsal of controller containment for
//! the qualified QEMU profile. Every observation reads PCI configuration through
//! C1's configuration adapter, registers through the C1 lease and frames through
//! the C3a.3 residue interface; none of them can write. This module declares no
//! write function and calls none: there is no PCI-write or register-write path.
use crate::xhci::capabilities;
use crate::xhci::containment::{self, Context, Observed, PCIE_ABSENT, PCIE_IDLE, PCIE_PENDING, Rehearsal, Source, State};
use crate::xhci::qualify::{self, Config};
use crate::xhci_qualify::{self, BarWords, PciConfig, Report};
use crate::xhci_residue::{Found, inspect};
use core::mem::{align_of, offset_of, size_of};

/// `struct xhci_contain_report` in xhci_qualify.h.
#[repr(C)]
pub struct ContainReport {
    q: Report,
    status: i32,
    state: u32,
    refusals: u32,
    uefi: u32,
    command: u32,
    pci_status: u32,
    usbcmd: u32,
    usbsts: u32,
    findings: u32,
    excluded: u32,
    interrupts_disabled: u32,
    plan_len: u32,
    trace_len: u32,
    /// Per entry: kind | result << 8 | width << 16 | step << 24, offset, old, new.
    trace: [[u32; 4]; containment::TRACE_LIMIT],
}
const _: () = {
    assert!(size_of::<ContainReport>() == 376 && align_of::<ContainReport>() == 8);
    assert!(offset_of!(ContainReport, status) == 224 && offset_of!(ContainReport, command) == 240);
    assert!(offset_of!(ContainReport, trace_len) == 272 && offset_of!(ContainReport, trace) == 276);
};

/// Live observations through the lease; every call is a fresh read pass.
struct Live<'a, 'b> {
    words: &'a mut BarWords<'b>,
    bits: u32,
    fresh: bool,
}

impl Source for Live<'_, '_> {
    fn observe(&mut self, o: &mut Observed) -> bool {
        // The first call continues the qualification pass; each later call is a new pass.
        if !core::mem::replace(&mut self.fresh, false) && !self.words.next_phase() {
            return false;
        }
        let Ok(i) = capabilities::inspect(&mut *self.words) else { return false };
        let Ok(pci) = qualify::inspect_pci(&mut PciConfig, self.bits) else { return false };
        let mut found = Found::default();
        if inspect(&mut *self.words, &i, self.bits, &mut found).is_err() {
            return false;
        }
        let pcie = match pci.pcie.checked_add(8) {
            None => return false,
            Some(_) if pci.pcie == 0 => PCIE_ABSENT,
            // Device Status is the high half of the DWORD at +8; bit 5 is Transactions Pending.
            Some(at) => match PciConfig.read32(at) {
                Ok(v) if (v >> 21) & 1 == 0 => PCIE_IDLE,
                Ok(_) => PCIE_PENDING,
                Err(_) => return false,
            },
        };
        let mut busy = false;
        for port in 0..u32::from(i.ports) {
            match capabilities::Words::word(&mut *self.words, i.operational + 0x400 + 16 * port) {
                Ok(v) => busy |= containment::port_busy(v),
                Err(_) => return false,
            }
        }
        let (r, out) = (&found.regs, &found.outcome);
        *o = Observed {
            bar: pci.base,
            id: u32::from(pci.vendor) | u32::from(pci.device) << 16,
            class_revision: pci.class_revision,
            command: pci.state.command,
            status: pci.status,
            msix_control: pci.state.msix_control,
            msi_control: pci.state.msi_control,
            pcie,
            legacy: i.legacy.is_some(),
            ports_busy: busy,
            extended: i.extended,
            header: i.header,
            protocol_ports: i.protocol_ports,
            usbcmd: r.usbcmd,
            usbsts: r.usbsts,
            config: r.config,
            crcr: r.crcr,
            dcbaap: r.dcbaap,
            interrupter_enables: out.interrupter_enables,
            ip_pending: out.interrupter_pending,
            findings: out.findings,
            reserved: out.reserved,
            owned_again: out.owned_again,
            protected: out.protected,
            hazards: out.hazards,
            excluded: out.excluded,
            memory: out.digest,
        };
        true
    }
}

fn fill(r: &mut ContainReport, a: &Observed, rh: &Rehearsal) {
    r.state = rh.state as u32;
    r.refusals = rh.refusals;
    r.command = u32::from(a.command);
    r.pci_status = u32::from(a.status);
    r.usbcmd = a.usbcmd;
    r.usbsts = a.usbsts;
    r.findings = u32::from(a.findings);
    r.excluded = u32::from(a.excluded);
    r.interrupts_disabled = u32::from(a.interrupts_disabled());
    r.plan_len = u32::from(rh.plan_len);
    r.trace_len = u32::from(rh.len);
    for (slot, t) in r.trace.iter_mut().zip(rh.entries()) {
        *slot = [u32::from(t.kind) | u32::from(t.result) << 8 | u32::from(t.width) << 16 | u32::from(t.step) << 24,
                 t.offset, t.old, t.new];
    }
}

/// Qualification status as for C1 (0 qualified, 1 rejected, -1 integrity); the
/// containment verdict is in the report. -2 for a bad output pointer. This
/// function reads only; it has no effect on the device or on any memory state.
// SAFETY: unique private symbol declared in xhci_qualify.h. `out` must be
// exclusive writable storage for the asserted layout; it is fully initialized
// in place before any field is read. Called once by xhci_contain_boot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_xhci_contain_v1(
    candidates: u32,
    complete: u32,
    physical_bits: u32,
    uefi: u32,
    out: *mut ContainReport,
) -> i32 {
    if out.is_null() || !(out as usize).is_multiple_of(align_of::<ContainReport>()) {
        return -2;
    }
    // SAFETY: checked non-null/aligned exclusive output, written whole before
    // the only reference is formed; caller bytes are never read.
    let r = unsafe {
        out.write_bytes(0, 1); // every field, the trace and the null C1 report pointer are zero
        &mut *out
    };
    r.status = 1;
    r.uefi = uefi;
    let mut first = Observed::default();
    let mut rehearsal: Option<Rehearsal> = None;
    let q = xhci_qualify::qualify(candidates, complete != 0, physical_bits, &mut r.q, &mut |w, i| {
        let c = Context { uefi: uefi != 0, qualified: true, operational: i.operational };
        let mut live = Live { words: w, bits: physical_bits, fresh: true };
        rehearsal = Some(containment::rehearse(&mut live, &c, &mut first));
        Ok(())
    });
    // The qualification must also have completed cleanly: lease released, PCI
    // state unchanged. Anything else invalidates the rehearsal.
    let not_uefi = if uefi == 0 { containment::NOT_UEFI } else { 0 };
    let mut rh = rehearsal.unwrap_or_else(|| Rehearsal::refused(containment::UNQUALIFIED | not_uefi));
    if q != 0 {
        rh.refusals |= containment::UNQUALIFIED;
        rh.state = State::Refused;
    }
    fill(r, &first, &rh);
    r.status = if q < 0 { -1 } else if rh.state == State::DryRunComplete { 0 } else { 1 };
    r.status
}
