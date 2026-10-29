//! USB.2-C2 private kernel adapter: the canonical DMA ownership Pool over the
//! kernel PMM, plus the opt-in software-only exercise. Nothing here produces a
//! device address, publishes a page or touches a PCI or controller register.
use crate::xhci::Error;
use crate::xhci::dma::{Frames, Page, Pool, Reason, State};
use crate::xhci::resources::Kind;
use core::cell::UnsafeCell;
use core::mem::{align_of, offset_of, size_of};

unsafe extern "C" {
    // usb_dma.h: kernel CR3, IF=0, non-IRQ context; allow-listed frames only.
    fn usb_dma_frame_alloc(frame: *mut u64) -> i32;
    fn usb_dma_frame_zero(frame: u64) -> i32;
    fn usb_dma_frame_free(frame: u64) -> i32;
    fn usb_dma_faults(alloc_at: i32, zero_at: i32, free_at: i32);
    fn pmm_free_count() -> u64;
}

struct Pmm;
impl Frames for Pmm {
    fn alloc(&mut self) -> Result<u64, Error> {
        let mut frame = 0u64;
        // SAFETY: exact C prototype; exclusive initialized stack output.
        match unsafe { usb_dma_frame_alloc(&mut frame) } {
            0 => Ok(frame),
            _ => Err(Error::Allocation),
        }
    }
    fn zero(&mut self, frame: u64) -> Result<(), Error> {
        // SAFETY: scalar argument; C refuses frames it did not hand out.
        match unsafe { usb_dma_frame_zero(frame) } {
            0 => Ok(()),
            _ => Err(Error::Backend),
        }
    }
    fn free(&mut self, frame: u64) -> Result<(), Error> {
        // SAFETY: scalar argument; C refuses frames it did not hand out.
        match unsafe { usb_dma_frame_free(frame) } {
            0 => Ok(()),
            _ => Err(Error::Backend),
        }
    }
}

fn faults(alloc_at: i32, zero_at: i32, free_at: i32) {
    // SAFETY: scalar arguments; affects only the next adapter calls.
    unsafe { usb_dma_faults(alloc_at, zero_at, free_at) }
}
fn pmm_free() -> u64 {
    // SAFETY: read-only counter with no arguments.
    unsafe { pmm_free_count() }
}

/// The boot's single xHCI DMA pool. Quarantined and retained records stay
/// here, owned, for the rest of the boot.
struct Global {
    pool: UnsafeCell<Pool>,
    ran: UnsafeCell<bool>,
}
// SAFETY: touched only by `vos_usb_dma_exercise_v1`, called once from the
// serialized boot thread with IF=0 (usb_dma_boot); the run flag refuses reentry.
unsafe impl Sync for Global {}
// Device limit: 32-bit addressing (the QEMU profile is AC64, the PMM is below 1 GiB).
static XHCI_DMA: Global = Global { pool: UnsafeCell::new(Pool::new(1 << 32)), ran: UnsafeCell::new(false) };

/// `struct usb_dma_report` in usb_dma.h.
#[repr(C)]
pub struct Report {
    set_frames: [u64; 4],
    retained_frame: u64,
    status: i32,
    allocated: u32,
    prepared: u32,
    released: u32,
    quarantined: u32,
    active_now: u32,
    retained_now: u32,
    allocation_failures: u32,
    rollback_failures: u32,
    violations: u32,
    budget_refused: u32,
    nondma_refused: u32,
    wrong_state_refused: u32,
    injected_steps: u32,
    retained_state: u32,
}
const _: () = {
    assert!(size_of::<Report>() == 104 && align_of::<Report>() == 8);
    assert!(offset_of!(Report, status) == 40 && offset_of!(Report, retained_state) == 96);
};

const SET: [Kind; 4] = [Kind::Command, Kind::Event, Kind::Dcbaa, Kind::Erst];

/// Each stage returns its number on failure. Frames are diagnostic copies
/// from the pool's records, never used as authority.
fn exercise(pool: &mut Pool, r: &mut Report) -> Result<(), i32> {
    let f = &mut Pmm;
    let base = pmm_free();
    // 1. Transactional set: four pages allocated, zeroed and prepared.
    let mut set: [Option<Page>; 4] = [None, None, None, None];
    pool.allocate_set(f, &SET, &mut set).map_err(|_| 1)?;
    if pmm_free() + 4 != base || pool.accounting().active_now != 4 {
        return Err(1);
    }
    for slot in 0..4 {
        let e = pool.entry(slot).ok_or(1)?;
        if e.state != State::Prepared {
            return Err(1);
        }
        r.set_frames[slot] = e.frame;
    }
    // 2. Budget and kind refusals change nothing.
    if pool.allocate(f, Kind::Command).is_err() {
        r.budget_refused = 1;
    }
    if pool.allocate(f, Kind::Control).is_err() {
        r.nondma_refused = 1;
    }
    if r.budget_refused != 1 || r.nondma_refused != 1 || pmm_free() + 4 != base {
        return Err(2);
    }
    // 3. Unpublished release is exact.
    if pool.cancel(f, &mut set) != 0 || pmm_free() != base || pool.accounting().active_now != 0 {
        return Err(3);
    }
    // 4. Allocation failure injected at every step rolls back exactly.
    for step in 0..4 {
        faults(step, -1, -1);
        let mut partial: [Option<Page>; 4] = [None, None, None, None];
        let failed = pool.allocate_set(f, &SET, &mut partial) == Err(Error::Allocation);
        faults(-1, -1, -1);
        if !failed || partial.iter().any(Option::is_some) || pmm_free() != base || pool.accounting().active_now != 0 {
            return Err(4);
        }
        r.injected_steps += 1;
    }
    // 5. An out-of-order transition is refused and the page stays usable.
    let page = pool.allocate(f, Kind::Event).map_err(|_| 5)?;
    if pool.prepare(&page).is_ok() {
        return Err(5);
    }
    r.wrong_state_refused = 1;
    let page = pool.zero(f, page).map_err(|_| 5)?;
    pool.prepare(&page).map_err(|_| 5)?;
    pool.release(f, page).map_err(|_| 5)?;
    if pmm_free() != base {
        return Err(5);
    }
    // 6. Deliberate quarantine: owned by the pool for the rest of the boot.
    let page = pool.allocate(f, Kind::Erst).map_err(|_| 6)?;
    let page = pool.zero(f, page).map_err(|_| 6)?;
    pool.prepare(&page).map_err(|_| 6)?;
    let slot = (0..8).find(|&s| pool.entry(s).is_some_and(|e| e.state == State::Prepared)).ok_or(6)?;
    pool.quarantine(page, Reason::Deliberate).map_err(|_| 6)?;
    let e = pool.entry(slot).ok_or(6)?;
    r.retained_frame = e.frame;
    r.retained_state = e.state as u32;
    if e.state != State::Quarantined || e.reason != Reason::Deliberate || pmm_free() + 1 != base {
        return Err(6);
    }
    Ok(())
}

/// 0 pass, positive failing stage, -2 bad output or a repeated run.
// SAFETY: unique private symbol declared in usb_dma.h. `out` must be
// exclusive writable storage for the asserted layout; it is fully initialized
// in place before any field is read. Called once by usb_dma_boot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vos_usb_dma_exercise_v1(out: *mut Report) -> i32 {
    if out.is_null() || !(out as usize).is_multiple_of(align_of::<Report>()) {
        return -2;
    }
    // SAFETY: see Global; the boot thread is the only caller.
    let (pool, ran) = unsafe { (&mut *XHCI_DMA.pool.get(), &mut *XHCI_DMA.ran.get()) };
    if *ran {
        return -2;
    }
    *ran = true;
    // SAFETY: checked non-null/aligned exclusive output, written whole before
    // the only reference is formed; caller bytes are never read.
    let r = unsafe {
        out.write(Report {
            set_frames: [0; 4],
            retained_frame: 0,
            status: 0,
            allocated: 0,
            prepared: 0,
            released: 0,
            quarantined: 0,
            active_now: 0,
            retained_now: 0,
            allocation_failures: 0,
            rollback_failures: 0,
            violations: 0,
            budget_refused: 0,
            nondma_refused: 0,
            wrong_state_refused: 0,
            injected_steps: 0,
            retained_state: 0,
        });
        &mut *out
    };
    r.status = match exercise(pool, r) {
        Ok(()) => 0,
        Err(stage) => stage,
    };
    let a = pool.accounting();
    r.allocated = a.allocated;
    r.prepared = a.prepared;
    r.released = a.released;
    r.quarantined = a.quarantined;
    r.active_now = u32::from(a.active_now);
    r.retained_now = u32::from(a.retained_now);
    r.allocation_failures = a.allocation_failures;
    r.rollback_failures = a.rollback_failures;
    r.violations = a.violations;
    r.status
}
