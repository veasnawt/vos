//! Private USB.1 C ABI. Device bytes are copied by the existing C map adapter.
use crate::xhci::discovery::{self, MapError, ProbeIo, Reason, Report, Status};
use core::ffi::c_void;
use core::mem::{align_of, offset_of, size_of};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct PciAddr {
    bus: u8,
    dev: u8,
    function: u8,
}
#[repr(C)]
pub struct Ops {
    context: *mut c_void,
    physical_bits: u32,
    config: Option<unsafe extern "C" fn(*mut c_void, PciAddr, u32) -> u32>,
    check: Option<unsafe extern "C" fn(*mut c_void, u64, u64) -> i32>,
    map: Option<unsafe extern "C" fn(*mut c_void, u64, usize, *mut *const u32, *mut usize) -> i32>,
}
type Config = unsafe extern "C" fn(*mut c_void, PciAddr, u32) -> u32;
type Check = unsafe extern "C" fn(*mut c_void, u64, u64) -> i32;
#[repr(C)]
pub struct Info {
    status: u32,
    reason: *const u8,
    vendor: u16,
    device: u16,
    command: u16,
    pci_status: u16,
    version: u16,
    class_revision: u32,
    bar_low: u32,
    bar_high: u32,
    hcs1: u32,
    hcs2: u32,
    hcs3: u32,
    hcc1: u32,
    doorbell: u32,
    runtime: u32,
    layout_end: u32,
    base: u64,
    caplength: u32,
    slots: u32,
    interrupters: u32,
    ports: u32,
    address_bits: u32,
    mapping_error: i32,
}
const _: () = {
    assert!(size_of::<Info>() == 104 && align_of::<Info>() == 8);
    assert!(offset_of!(Info, reason) == 8 && offset_of!(Info, vendor) == 16);
    assert!(offset_of!(Info, class_revision) == 28 && offset_of!(Info, base) == 72);
    assert!(offset_of!(Info, mapping_error) == 100);
    assert!(size_of::<Ops>() == 40 && offset_of!(Ops, config) == 16);
    assert!(size_of::<PciAddr>() == 3 && align_of::<PciAddr>() == 1);
};
unsafe extern "C" {
    fn vos_xhci_copy_caps(ops: *const Ops, base: u64, out: *mut u32, error: *mut i32) -> i32;
}
impl Info {
    fn report(&self) -> Report {
        Report {
            // Decode always replaces status/reason; neither pointer is followed.
            status: Status::Invalid,
            reason: Reason::Interface,
            vendor: self.vendor,
            device: self.device,
            command: self.command,
            pci_status: self.pci_status,
            version: self.version,
            class_revision: self.class_revision,
            bar_low: self.bar_low,
            bar_high: self.bar_high,
            hcs1: self.hcs1,
            hcs2: self.hcs2,
            hcs3: self.hcs3,
            hcc1: self.hcc1,
            doorbell: self.doorbell,
            runtime: self.runtime,
            layout_end: self.layout_end,
            base: self.base,
            caplength: self.caplength,
            slots: self.slots,
            interrupters: self.interrupters,
            ports: self.ports,
            address_bits: self.address_bits,
            mapping_error: self.mapping_error,
        }
    }
    fn from_report(r: Report) -> Self {
        Self {
            status: r.status as u32,
            reason: r.reason.c_string().as_ptr(),
            vendor: r.vendor,
            device: r.device,
            command: r.command,
            pci_status: r.pci_status,
            version: r.version,
            class_revision: r.class_revision,
            bar_low: r.bar_low,
            bar_high: r.bar_high,
            hcs1: r.hcs1,
            hcs2: r.hcs2,
            hcs3: r.hcs3,
            hcc1: r.hcc1,
            doorbell: r.doorbell,
            runtime: r.runtime,
            layout_end: r.layout_end,
            base: r.base,
            caplength: r.caplength,
            slots: r.slots,
            interrupters: r.interrupters,
            ports: r.ports,
            address_bits: r.address_bits,
            mapping_error: r.mapping_error,
        }
    }
}
struct Callbacks<'a> {
    ops: &'a Ops,
    address: PciAddr,
    config: Config,
    check: Check,
}
impl ProbeIo for Callbacks<'_> {
    fn config(&mut self, offset: u32) -> u32 {
        // SAFETY: validated callback, synchronous trusted context; fixed aligned
        // offsets <=20. The C caller owns callbacks for the whole invocation.
        unsafe { (self.config)(self.ops.context, self.address, offset) }
    }
    fn check(&mut self, base: u64, bytes: u64) -> i32 {
        // SAFETY: same callback lifetime; safe core validated the extent.
        unsafe { (self.check)(self.ops.context, base, bytes) }
    }
    fn snapshot(&mut self, base: u64) -> Result<[u32; 7], MapError> {
        let mut words = [0; 7];
        let mut error = 0;
        // SAFETY: live validated ops; exclusive initialized output/error. C
        // checks map length/alignment, copies exactly seven DWORDs, retains no RAM.
        let result = unsafe { vos_xhci_copy_caps(self.ops, base, words.as_mut_ptr(), &mut error) };
        match result {
            0 => Ok(words),
            1 => Err(MapError::Mapping(error)),
            _ => Err(MapError::Invalid),
        }
    }
}

// SAFETY: unique existing private symbols matching xhci.h, not userspace ABI.
#[unsafe(no_mangle)]
pub extern "C" fn xhci_class_matches(revision: u32) -> i32 {
    discovery::class_matches(revision) as i32
}

/// Requires initialized, aligned, exclusive output; NULL or >=28 bytes of
/// aligned copied input RAM. Inputs must not alias output; no MMIO pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn xhci_decode_caps(words: *const u32, bytes: usize, out: *mut Info) -> u32 {
    if out.is_null() || !out.is_aligned() {
        return Status::Invalid as u32;
    }
    // SAFETY: caller establishes initialized exclusive output lifetime.
    let target = unsafe { &mut *out };
    let mut report = target.report();
    let words = if words.is_null() || !words.is_aligned() || bytes < discovery::CAP_BYTES {
        None
    } else {
        // SAFETY: alignment/length checked; caller owns readable copied RAM.
        Some(unsafe { words.cast::<[u32; 7]>().read() })
    };
    let status = discovery::decode_caps(words, &mut report);
    *target = Info::from_report(report);
    status as u32
}

/// Caller owns valid ops/context/callbacks and a distinct writable output;
/// callbacks must not mutate/alias ops or output during this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn xhci_probe(address: PciAddr, ops: *const Ops, out: *mut Info) -> u32 {
    if out.is_null() || !out.is_aligned() {
        return Status::Invalid as u32;
    }
    let mut report = Report::default();
    let report = if ops.is_null() || !ops.is_aligned() {
        report.outcome(Status::Invalid, Reason::Interface);
        report
    } else {
        // SAFETY: trusted caller's initialized ops, read-only for the call.
        let ops = unsafe { &*ops };
        if let (Some(config), Some(check), Some(_)) = (ops.config, ops.check, ops.map) {
            discovery::probe(
                &mut Callbacks {
                    ops,
                    address,
                    config,
                    check,
                },
                ops.physical_bits,
                &mut report,
            );
            report
        } else {
            report.outcome(Status::Invalid, Reason::Interface);
            report
        }
    };
    let status = report.status as u32;
    // SAFETY: exclusive writable output, never read (may be uninitialized in C).
    unsafe {
        out.write(Info::from_report(report));
    }
    status
}
