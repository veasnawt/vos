//! USB.2-C3a.2 private C ABI: the EFI firmware memory exclusion decision,
//! called once by efi_memory.c before pmm_init. Rust parses, validates and
//! classifies; C only enters the emitted ranges into the reservation table
//! (protect-only) and reports. No PMM, heap, device or page-table access.
use crate::efi_memory::{self, EfiMap, Error, Evidence, MemoryMap, Plan};
use core::ffi::c_void;
use core::mem::{align_of, offset_of, size_of};

/// `struct efi_memory_input` in efi_memory.h.
#[repr(C)]
pub struct EfiInput {
    mmap: *const u8,
    efi_mmap: *const u8,
    window_lo: u64,
    window_hi: u64,
    mmap_size: u32,
    efi_mmap_size: u32,
    efi_mmap_tags: u32,
    efi_firmware: u32,
    boot_services_active: u32,
    capacity: u32,
}

/// `struct efi_memory_report` in efi_memory.h.
#[repr(C)]
pub struct EfiReport {
    frames: u64,
    status: i32,
    error: u32,
    descriptors: u32,
    ranges: u32,
    coalesced: u32,
    reserved: u32,
}

const _: () = {
    assert!(size_of::<EfiInput>() == 56 && align_of::<EfiInput>() == 8);
    assert!(offset_of!(EfiInput, window_lo) == 16 && offset_of!(EfiInput, mmap_size) == 32);
    assert!(offset_of!(EfiInput, capacity) == 52);
    assert!(size_of::<EfiReport>() == 32 && align_of::<EfiReport>() == 8);
    assert!(offset_of!(EfiReport, status) == 8 && offset_of!(EfiReport, coalesced) == 24);
};

/// Adds [start, end) to the platform's reservation table; nonzero on failure.
type Sink = unsafe extern "C" fn(ctx: *mut c_void, start: u64, end: u64) -> i32;

/// Largest tag accepted from C (the Multiboot2 block itself is bounded by C).
const TAG_MAX: u32 = 1 << 20;

fn decide(i: &EfiInput, mmap: &[u8], efi: &[u8], sink: &mut dyn FnMut(u64, u64) -> bool,
          r: &mut EfiReport) -> Result<Plan, Error> {
    let evidence = Evidence {
        efi_firmware: i.efi_firmware != 0,
        efi_map_tags: i.efi_mmap_tags,
        boot_services_active: i.boot_services_active != 0,
    };
    if efi_memory::plan(&evidence)? == Plan::NotApplicable {
        return Ok(Plan::NotApplicable);
    }
    let map = EfiMap::parse(efi)?;
    r.descriptors = map.len() as u32;
    let ram = MemoryMap::parse(mmap)?;
    let s = efi_memory::exclusions(&map, &ram, i.window_lo, i.window_hi, i.capacity as usize, sink)?;
    r.ranges = s.ranges;
    r.frames = s.frames;
    r.coalesced = s.coalesced as u32;
    Ok(Plan::Apply)
}

// SAFETY: unique private symbol declared in efi_memory.h. `input` and `out`
// must be valid, aligned and exclusive for the call; `out` is written whole
// before any field is read. Each non-null tag pointer must address `size`
// readable bytes for the whole call (C passes tags inside the Multiboot2
// block that mb2_parse bounded, which stays reserved and is never written).
// `sink` is called only with `ctx`, synchronously, in ascending order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_efi_exclusions_v1(
    input: *const EfiInput,
    sink: Option<Sink>,
    ctx: *mut c_void,
    out: *mut EfiReport,
) -> i32 {
    if out.is_null() || !(out as usize).is_multiple_of(align_of::<EfiReport>()) {
        return -2;
    }
    // SAFETY: checked non-null/aligned exclusive output, written whole first.
    let r = unsafe {
        out.write(EfiReport { frames: 0, status: -1, error: 0, descriptors: 0, ranges: 0, coalesced: 0, reserved: 0 });
        &mut *out
    };
    if input.is_null() || !(input as usize).is_multiple_of(align_of::<EfiInput>()) {
        r.error = Error::Truncated.code();
        return -1;
    }
    // SAFETY: checked non-null/aligned input, read-only for the call.
    let i = unsafe { &*input };
    let Some(sink) = sink else {
        r.error = Error::Sink.code();
        return -1;
    };
    let slice = |p: *const u8, n: u32| -> &[u8] {
        if p.is_null() || n == 0 || n > TAG_MAX {
            &[]
        } else {
            // SAFETY: the caller contract above (n readable bytes at p).
            unsafe { core::slice::from_raw_parts(p, n as usize) }
        }
    };
    let (mmap, efi) = (slice(i.mmap, i.mmap_size), slice(i.efi_mmap, i.efi_mmap_size));
    let mut add = |s: u64, e: u64| {
        // SAFETY: C callback with the caller's context, per the contract above.
        unsafe { sink(ctx, s, e) == 0 }
    };
    match decide(i, mmap, efi, &mut add, r) {
        Ok(Plan::Apply) => r.status = 0,
        Ok(Plan::NotApplicable) => r.status = 1,
        Err(e) => {
            r.status = -1;
            r.error = e.code();
        }
    }
    r.status
}
