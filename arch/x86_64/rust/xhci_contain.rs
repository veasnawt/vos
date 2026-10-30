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
        let Ok(subsystem) = PciConfig.read32(0x2c) else { return false };
        let mut found = Found::default();
        if inspect(&mut *self.words, &i, self.bits, &mut found).is_err() {
            return false;
        }
        let Some(pcie) = pcie_state(pci.pcie) else { return false };
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
            subsystem,
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

/// PCIe Transactions Pending: Device Status is the high half of the DWORD at
/// capability + 8, bit 5. None if unreadable.
fn pcie_state(cap: u8) -> Option<u8> {
    match cap.checked_add(8) {
        None => None,
        Some(_) if cap == 0 => Some(PCIE_ABSENT),
        Some(at) => match PciConfig.read32(at) {
            Ok(v) if (v >> 21) & 1 == 0 => Some(PCIE_IDLE),
            Ok(_) => Some(PCIE_PENDING),
            Err(_) => None,
        },
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
        let c = Context { uefi: uefi != 0, qualified: true, operational: i.operational, ..Context::default() };
        let mut live = Live { words: w, bits: physical_bits, fresh: true };
        rehearsal = Some(containment::rehearse(&mut live, &c, &mut first));
        Ok(0)
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

// ---- USB.2-C3b.2: controlled containment (QEMU test images only) ----
#[cfg(vos_contain_test)]
mod execute {
    use super::*;
    use crate::xhci::capabilities::{Inspection, Words};
    use crate::xhci::containment::{AfterReset, BUS_MASTER, Effects, Execution, Quick};

    unsafe extern "C" {
        // xhci_qualify.h (VOS_CONTAIN_TEST only): the two single-purpose write
        // adapters. Each checks the session, its precondition and once-only.
        fn xhci_contain_w1(old: u16, value: u16) -> i32;
        fn xhci_contain_w2(token: u64, offset: u32, old: u32, value: u32) -> i32;
    }

    /// `struct xhci_contain_execute_report` in xhci_qualify.h.
    #[repr(C)]
    pub struct ExecuteReport {
        q: Report,
        status: i32,
        state: u32,
        refusals: u32,
        environment: u32,
        writes: u32,
        polls: u32,
        w1: [u32; 4],
        usbsts_after_w1: u32,
        w2: [u32; 2],
        usbcmd: u32,
        usbsts: u32,
        dnctrl: u32,
        config: u32,
        ports_active: u32,
        interrupters_clear: u32,
        command: u32,
        pci_status: u32,
        pcie: u32,
        crcr: u64,
        dcbaap: u64,
    }
    const _: () = assert!(size_of::<ExecuteReport>() == 328 && offset_of!(ExecuteReport, state) == 228
        && offset_of!(ExecuteReport, crcr) == 312);

    /// The live controller plus the inspection the session started from.
    struct Exec<'a, 'b> {
        live: Live<'a, 'b>,
        i: Inspection,
    }

    impl Exec<'_, '_> {
        #[inline(never)]
        fn rd(&mut self, offset: u32) -> Option<u32> {
            self.live.words.word(offset).ok()
        }
        fn rd64(&mut self, offset: u32) -> Option<u64> {
            Some(u64::from(self.rd(offset)?) | u64::from(self.rd(offset + 4)?) << 32)
        }
        fn read_after(&mut self, a: &mut AfterReset) -> Option<()> {
            let (op, rt) = (self.i.operational, self.i.runtime);
            a.usbcmd = self.rd(op)?;
            a.usbsts = self.rd(op + 4)?;
            a.dnctrl = self.rd(op + 0x14)?;
            a.crcr = self.rd64(op + 0x18)?;
            a.dcbaap = self.rd64(op + 0x30)?;
            a.config = self.rd(op + 0x38)?;
            let mut active = 0;
            for p in 0..u32::from(self.i.ports) {
                active |= self.rd(op + 0x400 + 16 * p)?;
            }
            a.ports_active = active & 0x1b != 0; // CCS, PED, OCA, PR; change bits ignored
            let mut any = 0;
            for n in 0..u32::from(self.i.interrupters) {
                for off in [0, 8, 0x10, 0x14, 0x18, 0x1c] {
                    any |= self.rd(rt + 0x20 + 32 * n + off)?; // IMAN, ERSTSZ, ERSTBA, ERDP
                }
            }
            a.interrupters_clear = any == 0;
            let pci = qualify::inspect_pci(&mut PciConfig, self.live.bits).ok()?;
            a.pcie = pcie_state(pci.pcie)?;
            a.command = pci.state.command;
            a.status = pci.status;
            a.msix_control = pci.state.msix_control;
            a.msi_control = pci.state.msi_control;
            a.bar = pci.base;
            Some(())
        }
    }

    impl Source for Exec<'_, '_> {
        fn observe(&mut self, o: &mut Observed) -> bool {
            self.live.observe(o)
        }
    }

    impl Effects for Exec<'_, '_> {
        fn write_command(&mut self, old: u16, value: u16) -> bool {
            // SAFETY: exact C prototype, scalar arguments; C revalidates everything.
            unsafe { xhci_contain_w1(old, value) == 0 }
        }
        fn write_usbcmd(&mut self, old: u32, value: u32) -> bool {
            let token = self.live.words.token();
            // SAFETY: exact C prototype, scalar arguments; C revalidates everything.
            unsafe { xhci_contain_w2(token, self.i.operational, old, value) == 0 }
        }
        fn quick(&mut self, q: &mut Quick, ports: bool) -> bool {
            let op = self.i.operational;
            let (Ok(cs), Some(c), Some(s)) = (PciConfig.read32(4), self.rd(op), self.rd(op + 4)) else { return false };
            let mut busy = false;
            for p in 0..u32::from(self.i.ports) * u32::from(ports) {
                let Some(v) = self.rd(op + 0x400 + 16 * p) else { return false };
                busy |= containment::port_busy(v);
            }
            *q = Quick { command: cs as u16, status: (cs >> 16) as u16, usbcmd: c, usbsts: s, ports_busy: busy };
            true
        }
        fn phase(&mut self, reads: u32) -> bool {
            self.live.words.phase(reads)
        }
        fn after_reset(&mut self, a: &mut AfterReset) -> bool {
            self.read_after(a).is_some()
        }
    }

    #[inline(never)]
    fn session(w: &mut BarWords<'_>, i: &Inspection, bits: u32, uefi: u32, environment: u32, first: &mut Observed)
        -> Execution {
        let c = Context { uefi: uefi != 0, qualified: true, operational: i.operational, execute: true, environment };
        let mut x = Exec { live: Live { words: w, bits, fresh: true }, i: *i };
        containment::execute(&mut x, &c, first)
    }

    /// Qualification status as for C1; the containment outcome is in the report.
    /// -2 for a bad output pointer. `environment` is the C test-environment mask.
    // SAFETY: unique private symbol declared in xhci_qualify.h (VOS_CONTAIN_TEST).
    // `out` must be exclusive writable storage for the asserted layout; it is
    // fully initialized in place first. Called once by xhci_contain_execute_boot.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn vos_xhci_contain_execute_v1(
        candidates: u32,
        complete: u32,
        physical_bits: u32,
        uefi: u32,
        environment: u32,
        out: *mut ExecuteReport,
    ) -> i32 {
        if out.is_null() || !(out as usize).is_multiple_of(align_of::<ExecuteReport>()) {
            return -2;
        }
        // SAFETY: checked non-null/aligned exclusive output, zeroed whole before
        // the only reference is formed; caller bytes are never read.
        let r = unsafe {
            out.write_bytes(0, 1);
            &mut *out
        };
        r.environment = environment;
        let mut first = Observed::default();
        let mut done: Option<Execution> = None;
        let q = xhci_qualify::qualify(candidates, complete != 0, physical_bits, &mut r.q, &mut |w, i| {
            let e = session(w, i, physical_bits, uefi, environment, &mut first);
            done = Some(e);
            // What the read-back showed: only a W1 whose read-back has Bus Master Enable clear.
            Ok(if e.writes > 0 && e.w1[2] != 0 && e.w1[2] & BUS_MASTER == 0 { BUS_MASTER } else { 0 })
        });
        let not_uefi = if uefi == 0 { containment::NOT_UEFI } else { 0 };
        let mut rh = done.map_or(Rehearsal::refused(containment::UNQUALIFIED | not_uefi), |e| e.rehearsal);
        let mut state = done.map_or(State::Refused, |e| e.state);
        let writes = done.map_or(0, |e| e.writes);
        if q != 0 && writes == 0 {
            rh.refusals |= containment::UNQUALIFIED;
            state = State::Refused;
        } else if q < 0 && state == State::Contained {
            state = State::IntegrityFault;
        }
        r.refusals = rh.refusals;
        if let Some(e) = done {
            let a = &e.after;
            r.writes = u32::from(e.writes);
            r.polls = e.polls;
            r.w1 = e.w1.map(u32::from);
            r.usbsts_after_w1 = e.usbsts_after_w1;
            r.w2 = e.w2;
            r.usbcmd = a.usbcmd;
            r.usbsts = a.usbsts;
            r.dnctrl = a.dnctrl;
            r.config = a.config;
            r.ports_active = u32::from(a.ports_active);
            r.interrupters_clear = u32::from(a.interrupters_clear);
            r.command = u32::from(a.command);
            r.pci_status = u32::from(a.status);
            r.pcie = u32::from(a.pcie);
            r.crcr = a.crcr;
            r.dcbaap = a.dcbaap;
        }
        r.state = state as u32;
        r.status = if q < 0 { -1 } else if state == State::Contained { 0 } else { 1 };
        r.status
    }
}
