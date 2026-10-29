//! USB.2-C3a pure tests: firmware-residue assessment and dry-run lifecycle.
#![forbid(unsafe_code)]
use std::collections::{BTreeMap, BTreeSet};
use vos_xhci::xhci::Error;
use vos_xhci::xhci::lifecycle::{self, Facts, Refusal, Step, Violation};
use vos_xhci::xhci::residue::*;

const MB: u64 = 1 << 20;

/// Physical memory model: free/allocated/protected/untracked frames, a
/// registry with the kernel's capacity, and readable bytes only once owned.
struct Fake {
    mem: BTreeMap<u64, u64>,
    allocated: BTreeSet<u64>,
    protected: BTreeSet<u64>,
    /// USB.2-C3a.3: frames the boot excluded (readable in place, never reserved).
    excluded: BTreeSet<u64>,
    owned: Vec<(u64, Role)>,
    interrupters: Vec<Interrupter>,
    reads_outside: u32,
    fail_reserve: Option<u64>,
}
impl Fake {
    fn new() -> Self {
        Fake { mem: BTreeMap::new(), allocated: BTreeSet::new(), protected: BTreeSet::new(),
               excluded: BTreeSet::new(), owned: vec![],
               interrupters: vec![Interrupter::default(); 16], reads_outside: 0, fail_reserve: None }
    }
    fn put(&mut self, at: u64, v: u64) {
        self.mem.insert(at, v);
    }
    fn frames(&self) -> Vec<u64> {
        let mut f: Vec<u64> = self.owned.iter().map(|o| o.0).collect();
        f.sort();
        f
    }
}
impl Memory for Fake {
    fn page(&mut self, frame: u64) -> Page {
        assert_eq!(frame % 4096, 0);
        if self.owned.iter().any(|o| o.0 == frame) {
            Page::Owned
        } else if frame >= 1 << 30 {
            Page::Untracked
        } else if self.excluded.contains(&frame) {
            Page::Excluded
        } else if self.protected.contains(&frame) || frame < MB {
            Page::Protected
        } else if self.allocated.contains(&frame) {
            Page::Allocated
        } else {
            Page::Free
        }
    }
    fn reserve(&mut self, frame: u64, role: Role) -> Result<(), Error> {
        assert_eq!(self.page(frame), Page::Free, "only free frames reach reserve");
        if self.fail_reserve == Some(frame) {
            return Err(Error::Backend);
        }
        if self.owned.len() == RESERVE_LIMIT {
            return Err(Error::Budget);
        }
        self.owned.push((frame, role));
        Ok(())
    }
    fn read64(&mut self, phys: u64) -> Result<u64, Error> {
        let frame = phys & !4095;
        if phys % 8 != 0 || !(self.owned.iter().any(|o| o.0 == frame) || self.excluded.contains(&frame)) {
            self.reads_outside += 1;
            return Err(Error::Backend);
        }
        Ok(*self.mem.get(&phys).unwrap_or(&0))
    }
    fn interrupter(&mut self, index: u8) -> Result<Interrupter, Error> {
        self.interrupters.get(usize::from(index)).copied().ok_or(Error::Bounds)
    }
}

/// OVMF as observed: DCBAA 0x674a000 (64 slots), command ring 0x674a240
/// (256 TRBs, Link at the end back to the start), event segment 0x674b240
/// (512 TRBs), ERST 0x674d240, interrupter 0 IE set; halted, no devices.
fn ovmf() -> (Fake, Registers) {
    let mut f = Fake::new();
    let ring = 0x674a240u64;
    let link = ring + 255 * 16;
    f.put(link, ring);
    f.put(link + 8, u64::from(6u32 << 10 | 2) << 32); // Link TRB, toggle cycle
    f.put(0x674d240, 0x674b240);
    f.put(0x674d248, 512);
    f.interrupters[0] = Interrupter { iman: 2, erstsz: 1, erstba: 0x674d240, erdp: 0x674b240 };
    let r = Registers { usbcmd: 0, usbsts: 1, crcr: ring | 1, dcbaap: 0x674a000, config: 64, hcs1: 0x0800_1040,
                        hcs2: 0xf, hcc1: 0x0008_7001, interrupters: 16, physical_bits: 40 };
    (f, r)
}

#[test]
fn ovmf_residue_is_bounded_protected_and_complete() {
    let (mut f, r) = ovmf();
    let o = assess(&mut f, &r);
    assert!(o.complete() && o.halted(), "{o:?}");
    assert_eq!(f.frames(), vec![0x674a000, 0x674b000, 0x674c000, 0x674d000]);
    assert_eq!((o.reserved, o.hazards, o.protected), (4, 0, 0));
    assert_eq!(o.interrupter_enables, 1);
    assert_eq!(f.reads_outside, 0, "every read was inside an owned frame");
    let roles: Vec<Role> = f.owned.iter().map(|o| o.1).collect();
    assert_eq!(roles, vec![Role::Dcbaa, Role::CommandRing, Role::EventTable, Role::EventSegment]);
}

#[test]
fn seabios_residue_in_reserved_memory_is_uninspected_not_safe() {
    let (mut f, mut r) = ovmf();
    f.protected.insert(0x7fdf000);
    r.crcr = 0x7fdfc01;
    r.dcbaap = 0x7fdfd80;
    f.interrupters[0] = Interrupter { iman: 0, erstsz: 1, erstba: 0x7fdfd40, erdp: 0x7fdfa00 };
    let o = assess(&mut f, &r);
    assert_eq!(o.findings, UNINSPECTED);
    assert!(!o.complete() && o.reserved == 0 && o.protected == 3 && f.reads_outside == 0);
}

#[test]
fn already_allocated_frame_is_a_hazard_never_reserved() {
    let (mut f, r) = ovmf();
    f.allocated.insert(0x674c000);
    let o = assess(&mut f, &r);
    assert!(o.findings & HAZARD != 0 && o.hazards == 1);
    assert!(!f.frames().contains(&0x674c000), "an allocated frame never becomes firmware-reserved");
    assert_eq!(f.frames(), vec![0x674a000, 0x674b000, 0x674d000]);
    // A reservation the platform refuses is also a hazard, not success.
    let (mut f, r) = ovmf();
    f.fail_reserve = Some(0x674d000);
    let o = assess(&mut f, &r);
    assert!(o.findings & HAZARD != 0 && !f.frames().contains(&0x674d000));
}

#[test]
fn duplicate_claims_do_not_double_count() {
    let (mut f, r) = ovmf();
    let o = assess(&mut f, &r);
    assert_eq!((o.reserved, o.owned_again), (4, 3));
    // A second assessment over the same registry reserves nothing new.
    let o2 = assess(&mut f, &r);
    assert_eq!((o2.reserved, o2.owned_again, f.owned.len()), (0, 7, 4));
}

#[test]
fn exhaustion_fails_closed() {
    let (mut f, r) = ovmf();
    // A 4096-TRB segment needs 16 pages; with DCBAA and ring the registry overflows.
    f.put(0x674d248, 4096);
    f.put(0x674d240, 0x6800000);
    f.interrupters[0].erdp = 0x6800000;
    let o = assess(&mut f, &r);
    assert!(o.findings & EXHAUSTED != 0 && !o.complete());
    assert_eq!(f.owned.len(), RESERVE_LIMIT);
}

#[test]
fn running_controller_and_live_flags() {
    for (cmd, sts) in [(1, 0), (0, 0), (1, 1)] {
        let (mut f, mut r) = ovmf();
        r.usbcmd = cmd;
        r.usbsts = sts;
        let o = assess(&mut f, &r);
        assert!(o.findings & RUNNING != 0 && !o.halted());
        assert_eq!(f.owned.len(), 4, "structures are still protected");
    }
}

#[test]
fn device_contexts_and_scratchpads_leave_it_incomplete() {
    let (mut f, r) = ovmf();
    f.put(0x674a000 + 8 * 3, 0x6900000); // slot 3 device context
    let o = assess(&mut f, &r);
    assert_eq!(o.findings, DEVICES);
    assert!(f.frames().contains(&0x6900000));
    let (mut f, mut r) = ovmf();
    r.hcs2 = 0x0800_000f; // one scratchpad
    f.put(0x674a000, 0x6a00000);
    let o = assess(&mut f, &r);
    assert!(o.findings & SCRATCHPADS != 0 && f.frames().contains(&0x6a00000));
    let (mut f, r) = ovmf();
    f.put(0x674a000, 0x6a00000); // scratchpad pointer without scratchpads
    assert!(assess(&mut f, &r).findings & MALFORMED != 0);
}

#[test]
fn malformed_and_adversarial_pointers() {
    let cases: Vec<Box<dyn Fn(&mut Fake, &mut Registers)>> = vec![
        Box::new(|_, r| r.dcbaap = 0x674a008),                 // misaligned DCBAA
        Box::new(|_, r| r.config = 0x41),                       // more slots than HCSPARAMS1
        Box::new(|f, _| f.interrupters[0].erstba = 0x674d250),  // misaligned ERST
        Box::new(|f, _| f.interrupters[0].erstsz = 2),          // larger than ERST Max (hcs2=0xf -> 2^0)
        Box::new(|f, _| f.put(0x674d248, 8)),                   // segment below 16 TRBs
        Box::new(|f, _| f.put(0x674d248, 4097)),                // segment above 4096 TRBs
        Box::new(|f, _| f.put(0x674d240, 0x674b250)),           // misaligned segment
        Box::new(|_, r| r.dcbaap = (1 << 40) - 8),              // beyond physical width
        Box::new(|_, r| r.dcbaap = u64::MAX & !0x3f),           // address overflow
    ];
    for (n, case) in cases.iter().enumerate() {
        let (mut f, mut r) = ovmf();
        case(&mut f, &mut r);
        let o = assess(&mut f, &r);
        assert!(o.findings & (MALFORMED | ERDP_OUTSIDE) != 0, "case {n}: {o:?}");
        assert_eq!(f.reads_outside, 0, "case {n}: no read outside owned frames");
    }
    // ERDP outside every segment.
    let (mut f, r) = ovmf();
    f.interrupters[0].erdp = 0x6000000;
    assert!(assess(&mut f, &r).findings & ERDP_OUTSIDE != 0);
    // Untracked (above 1 GiB) structures are safe from reuse but unreadable.
    let (mut f, mut r) = ovmf();
    r.dcbaap = 0x8000_0000;
    let o = assess(&mut f, &r);
    assert!(o.findings & UNINSPECTED != 0 && !f.frames().contains(&0x8000_0000));
}

#[test]
fn command_ring_walk_is_bounded() {
    // Unknown ring location on a programmed controller.
    let (mut f, mut r) = ovmf();
    r.crcr = 0;
    assert!(assess(&mut f, &r).findings & RING_OPEN != 0);
    // No Link TRB at all: the walk stops at its page bound.
    let (mut f, r) = ovmf();
    f.mem.retain(|&k, _| k < 0x674a240 || k >= 0x674b240);
    let o = assess(&mut f, &r);
    assert!(o.findings & (RING_OPEN | EXHAUSTED) != 0, "{o:?}");
    // A Link TRB to a misaligned target, and a chain of too many segments.
    let (mut f, r) = ovmf();
    f.put(0x674a240 + 255 * 16, 0x674a248);
    assert!(assess(&mut f, &r).findings & RING_OPEN != 0);
    let (mut f, r) = ovmf();
    let mut at = 0x674a240 + 255 * 16;
    for seg in 0..6u64 {
        let next = 0x6c00000 + seg * 0x1000;
        f.put(at, next);
        f.put(at + 8, u64::from(6u32 << 10) << 32);
        at = next;
    }
    assert!(assess(&mut f, &r).findings & (RING_OPEN | EXHAUSTED) != 0);
}

fn qemu_facts() -> Facts {
    Facts { qualified: true, residue_complete: true, halted: true, run_stop: false, bus_master: true,
            controller_ready: true, interrupter_enables: 1, legacy_present: false, legacy_os_owned: false }
}

#[test]
fn plan_matches_the_qemu_profile_and_validates() {
    let f = qemu_facts();
    let p = lifecycle::plan(&f).unwrap();
    assert_eq!(p.steps(), &[Step::MaskInterrupter(0), Step::AwaitHalted, Step::DisableMaster, Step::Reset,
                            Step::AwaitResetReady, Step::VerifyCleared]);
    assert_eq!(lifecycle::validate(p.steps(), &f), Ok(()));
    let codes: Vec<u8> = p.steps().iter().map(|s| s.code()).collect();
    assert_eq!(codes, vec![0x10, 2, 3, 4, 5, 6]);
    assert!(p.steps()[2].action().is_some() && p.steps()[1].observation().is_some());
    // A running controller needs Halt before the halt is awaited.
    let running = Facts { halted: false, run_stop: true, ..f };
    let p = lifecycle::plan(&running).unwrap();
    assert_eq!(&p.steps()[1..3], &[Step::Halt, Step::AwaitHalted]);
    assert_eq!(lifecycle::validate(p.steps(), &running), Ok(()));
    // All sixteen interrupters enabled still fits the plan bound.
    let all = Facts { interrupter_enables: 0xffff, ..f };
    let p = lifecycle::plan(&all).unwrap();
    assert_eq!(p.steps().len(), 21);
    assert_eq!(lifecycle::validate(p.steps(), &all), Ok(()));
}

#[test]
fn refusals() {
    let f = qemu_facts();
    for (facts, refusal) in [
        (Facts { qualified: false, ..f }, Refusal::Unqualified),
        (Facts { residue_complete: false, ..f }, Refusal::ResidueIncomplete),
        (Facts { controller_ready: false, ..f }, Refusal::NotReady),
        (Facts { run_stop: true, ..f }, Refusal::Inconsistent),
        (Facts { halted: false, ..f }, Refusal::Inconsistent),
        (Facts { legacy_present: true, ..f }, Refusal::LegacyNotOwned),
    ] {
        assert_eq!(lifecycle::plan(&facts), Err(refusal));
        let valid = lifecycle::plan(&f).unwrap();
        assert_eq!(lifecycle::validate(valid.steps(), &facts), Err(Violation::Refused(refusal)));
    }
    let owned = Facts { legacy_present: true, legacy_os_owned: true, ..f };
    assert!(lifecycle::plan(&owned).is_ok());
}

#[test]
fn ordering_violations_are_rejected() {
    use Step::*;
    let f = qemu_facts();
    for (steps, v) in [
        (vec![AwaitHalted, DisableMaster, Reset, AwaitResetReady, VerifyCleared], Violation::MissingMask),
        (vec![MaskInterrupter(0), DisableMaster, AwaitHalted, Reset, AwaitResetReady, VerifyCleared], Violation::MissingHalt),
        (vec![MaskInterrupter(0), AwaitHalted, Reset, DisableMaster, AwaitResetReady, VerifyCleared], Violation::MissingMasterOff),
        (vec![MaskInterrupter(0), AwaitHalted, DisableMaster, AwaitResetReady, Reset, VerifyCleared], Violation::Order),
        (vec![MaskInterrupter(0), AwaitHalted, DisableMaster, Reset, VerifyCleared], Violation::Order),
        (vec![MaskInterrupter(0), AwaitHalted, DisableMaster, Reset, AwaitResetReady], Violation::Incomplete),
        (vec![MaskInterrupter(0), AwaitHalted, DisableMaster, Reset, AwaitResetReady, VerifyCleared, Reset], Violation::Trailing),
        (vec![MaskInterrupter(0), MaskInterrupter(0)], Violation::Duplicate),
        (vec![MaskInterrupter(1)], Violation::Unneeded),
        (vec![MaskInterrupter(0), Halt], Violation::Unneeded),           // not running
        (vec![MaskInterrupter(0), AwaitHalted, AwaitHalted], Violation::Duplicate),
        (vec![MaskInterrupter(0), AwaitHalted, DisableMaster, DisableMaster], Violation::Duplicate),
        (vec![MaskInterrupter(0), AwaitHalted, DisableMaster, Reset, Reset], Violation::Duplicate),
        (vec![AwaitHalted, MaskInterrupter(0)], Violation::MissingMask),
        (vec![MaskInterrupter(16)], Violation::Unneeded),
        (vec![MaskInterrupter(0); 25], Violation::Length),
    ] {
        assert_eq!(lifecycle::validate(&steps, &f), Err(v), "{steps:?}");
    }
    // Bus mastering already off: DisableMaster is an unneeded write.
    let off = Facts { bus_master: false, ..f };
    assert_eq!(lifecycle::validate(&[MaskInterrupter(0), AwaitHalted, DisableMaster], &off), Err(Violation::Unneeded));
    assert_eq!(lifecycle::validate(lifecycle::plan(&off).unwrap().steps(), &off), Ok(()));
    let running = Facts { halted: false, run_stop: true, ..f };
    assert_eq!(lifecycle::validate(&[MaskInterrupter(0), AwaitHalted], &running), Err(Violation::MissingHalt));
}

// ---- USB.2-C3a.3: boot-excluded firmware memory, inspected in place ----

fn ovmf_excluded() -> (Fake, Registers) {
    let (mut f, r) = ovmf();
    for p in [0x674a000u64, 0x674b000, 0x674c000, 0x674d000] {
        f.excluded.insert(p);
    }
    (f, r)
}

#[test]
fn excluded_firmware_residue_is_inspected_in_place_never_reserved() {
    let (mut f, r) = ovmf_excluded();
    let o = assess(&mut f, &r);
    assert!(o.complete() && o.halted() && o.reuse_protected() && o.inspection_complete(), "{o:?}");
    assert_eq!((o.reserved, o.owned_again, o.excluded, o.protected, o.hazards, o.pages), (0, 0, 7, 0, 0, 7));
    assert!(f.owned.is_empty(), "excluded frames are never reserved");
    assert_eq!(f.reads_outside, 0, "every read was inside an excluded frame");
    let facts = Facts { residue_complete: o.complete(), ..qemu_facts() };
    let plan = lifecycle::plan(&facts).unwrap();
    assert_eq!(lifecycle::validate(plan.steps(), &facts), Ok(()));
}

#[test]
fn excluded_and_free_frames_mix_without_double_ownership() {
    let (mut f, r) = ovmf_excluded();
    f.excluded.remove(&0x674c000);                      // middle of the event segment is ordinary free RAM
    let o = assess(&mut f, &r);
    assert!(o.complete() && o.reuse_protected() && o.inspection_complete(), "{o:?}");
    assert_eq!(f.frames(), vec![0x674c000]);
    assert_eq!((o.reserved, o.excluded), (1, 6));
    assert_eq!(f.reads_outside, 0);
}

#[test]
fn excluded_contents_are_still_validated() {
    // Malformed ERST segment (8 TRBs) inside excluded memory.
    let (mut f, r) = ovmf_excluded();
    f.put(0x674d248, 8);
    let o = assess(&mut f, &r);
    assert!(!o.complete() && !o.inspection_complete() && o.reuse_protected(), "{o:?}");
    assert_ne!(o.findings & MALFORMED, 0);
    // Link TRB with reserved low bits inside an excluded ring.
    let (mut f, r) = ovmf_excluded();
    f.put(0x674a240 + 255 * 16, 0x674a248);
    let o = assess(&mut f, &r);
    assert!(!o.inspection_complete() && o.findings & RING_OPEN != 0, "{o:?}");
    // ERDP outside the excluded segment.
    let (mut f, r) = ovmf_excluded();
    f.interrupters[0].erdp = 0x674e000;
    let o = assess(&mut f, &r);
    assert!(!o.inspection_complete() && o.findings & ERDP_OUTSIDE != 0, "{o:?}");
    assert_eq!(f.reads_outside, 0);
}

#[test]
fn malformed_and_out_of_range_pointers_never_read_outside_excluded_frames() {
    let cases: [(&str, fn(&mut Fake, &mut Registers), u16); 6] = [
        ("DCBAA beyond the physical address width", |_, r| r.dcbaap = 1 << 40, MALFORMED),
        ("DCBAA above 1 GiB (untracked)", |_, r| r.dcbaap = 0x8000_0000, UNINSPECTED),
        ("command ring wraps the address space", |_, r| r.crcr = 0xffff_ffff_ffff_ffc1, MALFORMED),
        ("ERST in a map-protected frame next to excluded ones", |f, _| {
            f.excluded.remove(&0x674d000);
            f.protected.insert(0x674d000);
        }, UNINSPECTED),
        ("segment straddles excluded into allocated RAM", |f, _| {
            f.excluded.remove(&0x674c000);
            f.allocated.insert(0x674c000);
        }, HAZARD),
        ("event segment pointer beyond RAM", |f, _| f.put(0x674d240, 0x4000_0000 - 0x1000), UNINSPECTED),
    ];
    for (what, edit, flag) in cases {
        let (mut f, mut r) = ovmf_excluded();
        edit(&mut f, &mut r);
        let o = assess(&mut f, &r);
        assert!(!o.complete() && o.findings & flag != 0, "{what}: {o:?}");
        assert_eq!(f.reads_outside, 0, "{what}: read outside an owned/excluded frame");
        assert!(f.owned.iter().all(|&(p, _)| !f.excluded.contains(&p)), "{what}: excluded frame reserved");
    }
}

#[test]
fn reuse_protection_and_inspection_completeness_are_separate() {
    // SeaBIOS: discovered frames map-protected, contents not inspected.
    let (mut f, mut r) = ovmf();
    f.protected.insert(0x7fdf000);
    r.crcr = 0x7fdfc01;
    r.dcbaap = 0x7fdfd80;
    f.interrupters[0] = Interrupter { iman: 0, erstsz: 1, erstba: 0x7fdfd40, erdp: 0x7fdfa00 };
    let o = assess(&mut f, &r);
    assert!(o.reuse_protected() && !o.inspection_complete() && !o.complete(), "{o:?}");
    // An allocated firmware frame: neither protected nor complete.
    let (mut f, r) = ovmf_excluded();
    f.excluded.remove(&0x674b000);
    f.allocated.insert(0x674b000);
    let o = assess(&mut f, &r);
    assert!(!o.reuse_protected() && !o.inspection_complete(), "{o:?}");
    // Registry exhaustion: not reuse protected.
    let (mut f, r) = ovmf();
    f.owned = (0..RESERVE_LIMIT as u64).map(|i| (0x100000 + i * 4096, Role::Dcbaa)).collect();
    let o = assess(&mut f, &r);
    assert!(!o.reuse_protected() && o.findings & EXHAUSTED != 0, "{o:?}");
    // A running controller with fully inspected excluded residue: inspection is
    // complete, but the assessment is not (halted alone never implies safety, and
    // a complete inspection alone never implies a stopped controller).
    let (mut f, mut r) = ovmf_excluded();
    r.usbcmd = 1;
    r.usbsts = 0;
    let o = assess(&mut f, &r);
    assert!(o.inspection_complete() && o.reuse_protected() && !o.halted() && !o.complete(), "{o:?}");
}
