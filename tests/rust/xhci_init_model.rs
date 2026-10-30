//! USB.2-C4.1 hosted tests for the xHCI initialization protocol model: deterministic state-machine
//! scenarios, refusal and failure matrices, QEMU-versus-specification device behaviour, seeded
//! randomized transitions with state and device-safety invariants, write-order checks over the
//! event log, and completeness of rule/reason coverage. No test touches PCI, MMIO or physical memory.
#![forbid(unsafe_code)]
#[path = "xhci_init_support.rs"]
mod support;
use std::collections::HashSet;
use support::*;
use vos_xhci_init::*;

const ALL_PAGES: [PageKind; 4] = [PageKind::Command, PageKind::Event, PageKind::Dcbaa, PageKind::Erst];

fn qemu() -> Harness {
    Harness::new(Quirks::qemu(), ReleasePolicy::AfterQuiesce)
}
fn with(f: impl FnOnce(&mut Quirks)) -> Harness {
    let mut q = Quirks::qemu();
    f(&mut q);
    Harness::new(q, ReleasePolicy::AfterQuiesce)
}
fn ok(h: &mut Harness, op: Operation) -> Accepted {
    match h.apply(op) {
        Ok(a) => a,
        Err(e) => panic!("{op:?} refused with {} in {}", e.name(), h.m.phase().name()),
    }
}
fn refused(h: &mut Harness, op: Operation, why: Reason) {
    assert_eq!(h.apply(op), Err(why), "{op:?} in {}", h.m.phase().name());
}
/// Follow the canonical session until `phase` is reached (before applying anything from it).
fn goto(h: &mut Harness, phase: Phase) {
    for _ in 0..4000 {
        if h.m.phase() == phase {
            return;
        }
        let op = h.next(h.target).unwrap_or_else(|| panic!("session ended in {} before {}", h.m.phase().name(), phase.name()));
        let _ = h.apply(op);
    }
    panic!("never reached {}", phase.name());
}
fn running() -> Harness {
    let mut h = qemu();
    goto(&mut h, Phase::Running);
    h
}
fn rec(m: &mut Machine, op: Operation) -> Result<Accepted, Reason> {
    let r = m.apply(op);
    record(&r);
    if let Some(t) = m.terminal_reason() {
        COVERAGE.lock().unwrap().get_or_insert_with(Default::default).1.insert(t.name());
    }
    r
}
fn bad_facts(f: impl FnOnce(&mut Facts)) -> Facts {
    let mut x = Device::new(Quirks::qemu()).facts();
    f(&mut x);
    x
}
fn qualify_with(f: Facts) -> Machine {
    let mut m = Machine::new(ReleasePolicy::AfterQuiesce);
    let a = rec(&mut m, Operation::Qualify(f)).unwrap();
    assert_eq!(a.from, Phase::Idle);
    m
}

// ---- happy path and write inventory -----------------------------------------------------------

#[test]
fn qemu_session_completes_260_noops_with_both_rings_wrapping() {
    let mut h = qemu();
    h.run(260, 100_000);
    assert_eq!(h.m.phase(), Phase::Quiesced, "{:?}", h.m.terminal_reason());
    assert_eq!(h.m.noops_completed(), 260);
    assert!(h.d.violations.is_empty(), "{:?}", h.d.violations);
    assert!(!h.d.hce);
    // 260 commands on a 255-slot ring: exactly one Link TRB wrap; 260 events on a 256-slot ring: one wrap.
    let wraps = h.log.iter().filter(|(o, r)| matches!(o, Operation::PublishLink { .. }) && r.is_ok()).count();
    assert_eq!(wraps, 1);
    assert_eq!(h.m.command_cursor().1, false, "producer cycle toggled once");
    for k in ALL_PAGES {
        assert_eq!(h.m.page(k).state, PageState::Free, "{k:?}");
    }
    assert_eq!(h.d.completions_posted, 260);
}

#[test]
fn slow_legacy_hardware_like_session_completes_in_both_erst_fetch_modes() {
    for mode in [ErstFetch::AtErstbaWrite, ErstFetch::AtRun] {
        let mut q = Quirks::slow();
        q.erst_fetch = mode;
        q.firmware_running = true;
        q.firmware_bme = true;
        let mut h = Harness::new(q, ReleasePolicy::AfterQuiesce);
        h.run(40, 100_000);
        assert_eq!(h.m.phase(), Phase::Quiesced, "{mode:?}: {:?}", h.m.terminal_reason());
        assert!(h.d.violations.is_empty(), "{mode:?}: {:?}", h.d.violations);
        assert_eq!(h.m.noops_completed(), 40);
    }
}

#[test]
fn the_write_inventory_is_exactly_n01_to_n18_and_in_order() {
    let mut q = Quirks::slow();
    q.firmware_running = true;
    q.firmware_bme = true;
    let mut h = Harness::new(q, ReleasePolicy::AfterQuiesce);
    h.run(3, 100_000);
    assert_eq!(h.m.phase(), Phase::Quiesced);
    let ids: Vec<&str> = h.d.writes.iter().map(|w| w.id).collect();
    let expected = [
        "N-04", "N-05", "N-01", "N-02", "N-03", "N-06", "N-07", "N-08", "N-09", "N-10", "N-11", "N-12", "N-13", "N-14", "N-15",
        "N-14", "N-15", "N-14", "N-15", "N-16", "N-17", "N-18",
    ];
    assert_eq!(ids, expected);
    let all: HashSet<&str> = ids.iter().copied().collect();
    for n in 1..=18 {
        assert!(all.contains(format!("N-{n:02}").as_str()), "N-{n:02} never emitted");
    }
    // The wire effects are the Gate 0 table: widths and values.
    let w = |id: &str| h.d.writes.iter().find(|w| w.id == id).copied().unwrap();
    assert_eq!((w("N-04").width, w("N-04").value), (8, 1));
    assert_eq!((w("N-02").width, w("N-02").value), (16, 0));
    assert_eq!((w("N-03").width, w("N-03").value), (32, 2));
    assert_eq!((w("N-06").width, w("N-06").value), (32, 1));
    assert_eq!((w("N-08").width, w("N-08").value), (64, CMD | 1));
    assert_eq!((w("N-11").width, w("N-11").value), (16, 1));
    assert_eq!(w("N-15").value & 8, 8, "ERDP acknowledgment sets EHB");
    assert!(h.d.writes.iter().all(|w| !matches!(w.target, Target::LegacySmi) || w.value == 0));
}

#[test]
fn no_session_ever_writes_ports_slots_interrupts_or_other_doorbells() {
    for seed in 0..40 {
        let mut r = Rng(seed + 1);
        let mut h = Harness::new(random_quirks(&mut r), ReleasePolicy::AfterQuiesce);
        h.run(10, 20_000);
        for w in &h.d.writes {
            assert!(
                matches!(w.target, Target::LegacyOsOwned | Target::LegacySmi | Target::RunStop | Target::Hcrst | Target::PciBme
                    | Target::Config | Target::Dcbaap | Target::Crcr | Target::Erstsz | Target::Erdp | Target::Erstba | Target::Doorbell0)
            );
            if w.target == Target::Doorbell0 {
                assert_eq!(w.value, 0, "only the command doorbell, target 0");
            }
        }
    }
}

// ---- QEMU 10.2.1 versus the specification --------------------------------------------------------------

#[test]
fn qemu_fetches_the_erst_inside_the_erstba_write_and_needs_bus_mastering() {
    // Raw device behaviour, bypassing the protocol: this is what the machine's ordering protects against.
    let mut d = Device::new(Quirks::qemu());
    d.erst_mem.insert(ERS, (EVT, 256));
    d.write(ModelEvent { id: "x", target: Target::Erstsz, width: 32, value: 1 });
    d.write(ModelEvent { id: "x", target: Target::Erstba, width: 64, value: ERS });
    assert!(d.hce, "QEMU: ERSTBA with BME clear raises HCE");
    assert_eq!(d.violations, vec![Violation::DmaWithoutBme]);
    // With BME set first the fetch succeeds inside the same write.
    let mut d = Device::new(Quirks::qemu());
    d.erst_mem.insert(ERS, (EVT, 256));
    d.write(ModelEvent { id: "x", target: Target::PciBme, width: 16, value: 1 });
    d.write(ModelEvent { id: "x", target: Target::Erstsz, width: 32, value: 1 });
    d.write(ModelEvent { id: "x", target: Target::Erstba, width: 64, value: ERS });
    assert!(d.fetched && !d.hce && d.violations.is_empty());
    // ERSTSZ other than 1 is rejected by QEMU at the same write.
    let mut d = Device::new(Quirks::qemu());
    d.erst_mem.insert(ERS, (EVT, 256));
    d.write(ModelEvent { id: "x", target: Target::PciBme, width: 16, value: 1 });
    d.write(ModelEvent { id: "x", target: Target::Erstsz, width: 32, value: 2 });
    d.write(ModelEvent { id: "x", target: Target::Erstba, width: 64, value: ERS });
    assert!(d.hce && d.violations == vec![Violation::ErstSizeInvalid]);
}

#[test]
fn the_alternate_fetch_timing_does_not_fault_until_run_and_the_protocol_is_safe_either_way() {
    let mut d = Device::new(Quirks { erst_fetch: ErstFetch::AtRun, ..Quirks::qemu() });
    d.erst_mem.insert(ERS, (EVT, 256));
    d.write(ModelEvent { id: "x", target: Target::Erstsz, width: 32, value: 1 });
    d.write(ModelEvent { id: "x", target: Target::Erstba, width: 64, value: ERS });
    assert!(!d.hce && !d.fetched, "no fetch at the ERSTBA write in this mode");
    // The protocol never relies on either timing: it requires verified BME before ERSTBA.
    for mode in [ErstFetch::AtErstbaWrite, ErstFetch::AtRun] {
        let mut h = Harness::new(Quirks { erst_fetch: mode, ..Quirks::qemu() }, ReleasePolicy::AfterQuiesce);
        goto(&mut h, Phase::BusMastering);
        refused(&mut h, Operation::SetErstBase(ERS), Reason::BusMasterUnverified);
        assert!(h.d.violations.is_empty() && !h.d.hce);
    }
}

#[test]
fn qemu_deviations_are_documented_as_device_options_not_protocol_assumptions() {
    // Synchronous reset (QEMU) and delayed reset (hardware-like) both satisfy the protocol.
    for polls in [0, 1, 5] {
        let mut h = with(|q| q.reset_polls = polls);
        h.run(2, 10_000);
        assert_eq!(h.m.phase(), Phase::Quiesced);
        assert!(h.d.violations.is_empty());
    }
    // A write while CNR is set is a device violation; the protocol never makes one because
    // ObserveReset(cnr) keeps it in Resetting.
    let mut h = with(|q| q.reset_polls = 3);
    goto(&mut h, Phase::Resetting);
    refused(&mut h, Operation::SetMaxSlots(1), Reason::ProgramBeforeContainment);
    assert!(h.d.violations.is_empty());
}

// ---- qualification, scratchpad eligibility, unresolved facts ---------------------------------------------

#[test]
fn qualification_refusals_are_named_prioritised_and_terminal() {
    let cases: Vec<(Facts, Reason)> = vec![
        (bad_facts(|f| f.profile = Profile::Unqualified), Reason::UnqualifiedPlatform),
        (bad_facts(|f| f.version = 0x0110), Reason::UnsupportedVersion),
        (bad_facts(|f| f.page_4k = false), Reason::PageSizeUnsupported),
        (bad_facts(|f| f.slots = 0), Reason::BadCapabilities),
        (bad_facts(|f| f.ports = 0), Reason::BadCapabilities),
        (bad_facts(|f| f.ports = 17), Reason::BadCapabilities),
        (bad_facts(|f| f.interrupters = 0), Reason::BadCapabilities),
        (bad_facts(|f| f.dma_limit = 0x10_0000), Reason::BadDmaLimit),
        (bad_facts(|f| f.dma_limit = (1 << 30) + 1), Reason::BadDmaLimit),
        (bad_facts(|f| f.hse = true), Reason::ControllerFault),
        (bad_facts(|f| f.hce = true), Reason::ControllerFault),
        (bad_facts(|f| { f.running = true; f.halted = true }), Reason::InconsistentRunState),
        (bad_facts(|f| { f.running = false; f.halted = false }), Reason::InconsistentRunState),
        (bad_facts(|f| f.iommu = Tri::Yes), Reason::IommuPresent),
        (bad_facts(|f| f.iommu = Tri::Unknown), Reason::IommuUnresolved),
        (bad_facts(|f| f.identity_dma = Tri::No), Reason::IdentityDmaDenied),
        (bad_facts(|f| f.identity_dma = Tri::Unknown), Reason::IdentityDmaUnresolved),
        (bad_facts(|f| f.no_snoop = Tri::Yes), Reason::NoSnoopEnabled),
        (bad_facts(|f| f.no_snoop = Tri::Unknown), Reason::NoSnoopUnresolved),
        (bad_facts(|f| f.msi = true), Reason::MsiEnabled),
        (bad_facts(|f| f.dbc = Dbc::Enabled), Reason::DbcEnabled),
        (bad_facts(|f| f.dbc = Dbc::Unknown), Reason::DbcUnresolved),
        (bad_facts(|f| f.scratchpads = 1), Reason::ScratchpadsRequired),
        (bad_facts(|f| f.scratchpads = 1023), Reason::ScratchpadsRequired),
        (bad_facts(|f| f.legacy = Legacy::Present { bios: true, os: true }), Reason::OwnershipConflict),
        (bad_facts(|f| f.connected = 1), Reason::DeviceAttached),
    ];
    for (f, why) in cases {
        let mut m = Machine::new(ReleasePolicy::AfterQuiesce);
        let a = rec(&mut m, Operation::Qualify(f)).unwrap();
        assert_eq!((a.to, a.why), (Phase::Refused, Rule::FaultRefused), "{why:?}");
        assert_eq!(m.terminal_reason(), Some(why));
        assert!(!m.exposed());
        // Terminal: nothing else is accepted, including a second qualification.
        assert_eq!(rec(&mut m, Operation::Qualify(Device::new(Quirks::qemu()).facts())), Err(Reason::AfterTerminal));
    }
    // Priority: the earlier rule wins when several defects are present.
    let f = bad_facts(|f| { f.profile = Profile::Unqualified; f.scratchpads = 4; f.connected = 1; f.msi = true });
    let mut m = Machine::new(ReleasePolicy::AfterQuiesce);
    rec(&mut m, Operation::Qualify(f)).unwrap();
    assert_eq!(m.terminal_reason(), Some(Reason::UnqualifiedPlatform));
    let f = bad_facts(|f| { f.scratchpads = 4; f.connected = 1; f.legacy = Legacy::Present { bios: true, os: true } });
    let mut m = Machine::new(ReleasePolicy::AfterQuiesce);
    rec(&mut m, Operation::Qualify(f)).unwrap();
    assert_eq!(m.terminal_reason(), Some(Reason::ScratchpadsRequired));
}

#[test]
fn unresolved_facts_are_never_treated_as_favourable() {
    // Every `Unknown` blocks qualification; there is no profile for which unknown means yes.
    for f in [bad_facts(|f| f.iommu = Tri::Unknown), bad_facts(|f| f.identity_dma = Tri::Unknown),
              bad_facts(|f| f.no_snoop = Tri::Unknown), bad_facts(|f| f.dbc = Dbc::Unknown)] {
        let m = qualify_with(f);
        assert_eq!(m.phase(), Phase::Refused);
        assert!(m.terminal_reason().is_some());
    }
    let m = qualify_with(bad_facts(|_| {}));
    assert_eq!(m.phase(), Phase::Owned);
    assert!(m.polling());
}

#[test]
fn idle_accepts_only_qualification_and_time() {
    let mut m = Machine::new(ReleasePolicy::AfterQuiesce);
    for op in [Operation::ClearRunStop, Operation::SetBusMaster, Operation::SetErstBase(ERS), Operation::RingCommandDoorbell,
               Operation::DmaAllocate { kind: PageKind::Command, addr: CMD }, Operation::ObserveStatus(Status { hch: true, hse: false, hce: false }),
               Operation::AssertReset, Operation::Cancel, Operation::BeginPollStep, Operation::ConsumeEvent(None)] {
        assert_eq!(rec(&mut m, op), Err(Reason::NotQualified), "{op:?}");
        assert_eq!(m, Machine::new(ReleasePolicy::AfterQuiesce));
    }
    assert!(rec(&mut m, Operation::Tick(5)).is_ok());
    rec(&mut m, Operation::Qualify(bad_facts(|_| {}))).unwrap();
    assert_eq!(rec(&mut m, Operation::Qualify(bad_facts(|_| {}))), Err(Reason::AlreadyQualified));
}

// ---- operation ordering --------------------------------------------------------------------------------------

#[test]
fn bus_master_and_erstba_ordering_is_enforced() {
    let mut h = qemu();
    goto(&mut h, Phase::Contained);
    refused(&mut h, Operation::SetBusMaster, Reason::BusMasterBeforeProgramming);
    refused(&mut h, Operation::SetErstBase(ERS), Reason::ErstbaBeforeStatic);
    refused(&mut h, Operation::SetRunStop, Reason::RunBeforeEventRing);
    goto(&mut h, Phase::Programmed);
    refused(&mut h, Operation::SetErstBase(ERS), Reason::ErstbaWithoutBusMaster);
    refused(&mut h, Operation::SetRunStop, Reason::RunBeforeEventRing);
    ok(&mut h, Operation::SetBusMaster);
    refused(&mut h, Operation::SetBusMaster, Reason::BusMasterAlreadySet);
    refused(&mut h, Operation::SetErstBase(ERS), Reason::BusMasterUnverified);
    refused(&mut h, Operation::SetRunStop, Reason::RunBeforeEventRing);
    ok(&mut h, Operation::ObservePci { bme: true, msi: false });
    refused(&mut h, Operation::SetErstBase(ERS + 0x1000), Reason::PointerMismatch);
    ok(&mut h, Operation::SetErstBase(ERS));
    refused(&mut h, Operation::SetErstBase(ERS), Reason::RegisterRepeated);
    refused(&mut h, Operation::SetRunStop, Reason::StatusNotChecked);
    { let st = h.d.status(); ok(&mut h, Operation::ObserveStatus(st)); }
    ok(&mut h, Operation::SetRunStop);
    refused(&mut h, Operation::SetRunStop, Reason::AlreadyRunning);
    assert!(h.d.violations.is_empty());
}

#[test]
fn halt_bme_clear_and_reset_must_happen_in_that_order() {
    let mut h = with(|q| { q.firmware_running = true; q.firmware_bme = true; });
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    assert_eq!(h.m.phase(), Phase::Owned);
    refused(&mut h, Operation::ClearBusMaster, Reason::BmeClearBeforeHalt);
    refused(&mut h, Operation::AssertReset, Reason::ResetBeforeHalt);
    { let st = h.d.status(); refused(&mut h, Operation::ObserveStatus(st), Reason::HaltNotRequested); }
    ok(&mut h, Operation::ClearRunStop);
    refused(&mut h, Operation::ClearRunStop, Reason::AlreadyHalted);
    refused(&mut h, Operation::ClearBusMaster, Reason::BmeClearBeforeHalt);
    { let st = h.d.status(); ok(&mut h, Operation::ObserveStatus(st)); }
    assert_eq!(h.m.phase(), Phase::Halted);
    refused(&mut h, Operation::AssertReset, Reason::ResetWithBusMaster);
    ok(&mut h, Operation::ClearBusMaster);
    refused(&mut h, Operation::ClearBusMaster, Reason::BusMasterAlreadyClear);
    refused(&mut h, Operation::AssertReset, Reason::BusMasterUnverified);
    ok(&mut h, Operation::ObservePci { bme: false, msi: false });
    ok(&mut h, Operation::AssertReset);
    assert!(h.d.violations.is_empty(), "{:?}", h.d.violations);
}

#[test]
fn static_registers_need_containment_prepared_pages_and_valid_values() {
    let mut h = qemu();
    goto(&mut h, Phase::Owned);
    refused(&mut h, Operation::SetMaxSlots(1), Reason::ProgramBeforeContainment);
    goto(&mut h, Phase::Contained);
    refused(&mut h, Operation::SetMaxSlots(0), Reason::SlotsInvalid);
    refused(&mut h, Operation::SetMaxSlots(2), Reason::SlotsInvalid);
    refused(&mut h, Operation::SetDcbaap(DCB), Reason::PageNotPrepared);
    refused(&mut h, Operation::SetCommandRing { addr: CMD, rcs: true }, Reason::PageNotPrepared);
    refused(&mut h, Operation::SetErstSize(1), Reason::PageNotPrepared);
    refused(&mut h, Operation::SetEventDequeue { addr: EVT, ehb: false }, Reason::PageNotPrepared);
    // Allocate and prepare all four, then each register has its own value rule.
    for k in ALL_PAGES {
        ok(&mut h, Operation::DmaAllocate { kind: k, addr: page_addr(k) });
        ok(&mut h, Operation::DmaZero(k));
    }
    ok(&mut h, Operation::PrepareCommandRing);
    ok(&mut h, Operation::PrepareEventRing);
    ok(&mut h, Operation::PrepareDcbaa { entry0: 0 });
    ok(&mut h, Operation::PrepareErst { segment: EVT, size: 256 });
    refused(&mut h, Operation::SetDcbaap(DCB + 64), Reason::PointerMismatch);
    refused(&mut h, Operation::SetCommandRing { addr: CMD, rcs: false }, Reason::CycleStateInvalid);
    refused(&mut h, Operation::SetCommandRing { addr: CMD + 64, rcs: true }, Reason::PointerMismatch);
    refused(&mut h, Operation::SetErstSize(0), Reason::ErstSizeInvalid);
    refused(&mut h, Operation::SetErstSize(2), Reason::ErstSizeInvalid);
    refused(&mut h, Operation::SetEventDequeue { addr: EVT, ehb: true }, Reason::EhbNotExpected);
    refused(&mut h, Operation::SetEventDequeue { addr: EVT + 16, ehb: false }, Reason::PointerMismatch);
    ok(&mut h, Operation::SetMaxSlots(1));
    ok(&mut h, Operation::SetDcbaap(DCB));
    ok(&mut h, Operation::SetCommandRing { addr: CMD, rcs: true });
    ok(&mut h, Operation::SetErstSize(1));
    assert_eq!(h.m.phase(), Phase::Programming);
    refused(&mut h, Operation::SetMaxSlots(1), Reason::RegisterRepeated);
    refused(&mut h, Operation::SetDcbaap(DCB), Reason::RegisterRepeated);
    refused(&mut h, Operation::SetCommandRing { addr: CMD, rcs: true }, Reason::RegisterRepeated);
    refused(&mut h, Operation::SetErstSize(1), Reason::RegisterRepeated);
    ok(&mut h, Operation::SetEventDequeue { addr: EVT, ehb: false });
    assert_eq!(h.m.phase(), Phase::Programmed);
    refused(&mut h, Operation::SetEventDequeue { addr: EVT, ehb: false }, Reason::ProgrammingClosed);
    refused(&mut h, Operation::SetMaxSlots(1), Reason::ProgrammingClosed);
    ok(&mut h, Operation::SetBusMaster);
    ok(&mut h, Operation::ObservePci { bme: true, msi: false });
    ok(&mut h, Operation::SetErstBase(ERS));
    { let st = h.d.status(); ok(&mut h, Operation::ObserveStatus(st)); }
    ok(&mut h, Operation::SetRunStop);
    refused(&mut h, Operation::SetCommandRing { addr: CMD, rcs: true }, Reason::CrcrWhileRunning);
    { let st = h.d.status(); ok(&mut h, Operation::ObserveStatus(st)); }
    refused(&mut h, Operation::SetCommandRing { addr: CMD, rcs: true }, Reason::CrcrWhileRunning);
    assert!(h.d.violations.is_empty());
}

#[test]
fn scratchpad_and_dcbaa_eligibility() {
    // Scratchpads required: refused at qualification, nothing written, no page ever allocated.
    let mut h = with(|q| q.scratchpads = 4);
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    assert_eq!((h.m.phase(), h.m.terminal_reason()), (Phase::Refused, Some(Reason::ScratchpadsRequired)));
    assert!(h.d.writes.is_empty());
    assert_eq!(h.apply(Operation::DmaAllocate { kind: PageKind::Dcbaa, addr: DCB }), Err(Reason::AfterTerminal));
    // A DCBAA entry 0 that would point at a scratchpad array is refused by the page builder.
    let mut h = qemu();
    goto(&mut h, Phase::Contained);
    ok(&mut h, Operation::DmaAllocate { kind: PageKind::Dcbaa, addr: DCB });
    ok(&mut h, Operation::DmaZero(PageKind::Dcbaa));
    refused(&mut h, Operation::PrepareDcbaa { entry0: 0x20_0000 }, Reason::ScratchpadPointerUnsupported);
    ok(&mut h, Operation::PrepareDcbaa { entry0: 0 });
}

#[test]
fn erst_contents_are_validated() {
    let mut h = qemu();
    goto(&mut h, Phase::Contained);
    ok(&mut h, Operation::DmaAllocate { kind: PageKind::Erst, addr: ERS });
    ok(&mut h, Operation::DmaZero(PageKind::Erst));
    refused(&mut h, Operation::PrepareErst { segment: EVT, size: 256 }, Reason::DependencyNotReady);
    ok(&mut h, Operation::DmaAllocate { kind: PageKind::Event, addr: EVT });
    ok(&mut h, Operation::DmaZero(PageKind::Event));
    refused(&mut h, Operation::PrepareErst { segment: EVT, size: 256 }, Reason::DependencyNotReady);
    ok(&mut h, Operation::PrepareEventRing);
    refused(&mut h, Operation::PrepareErst { segment: EVT + 0x1000, size: 256 }, Reason::ErstSegmentMismatch);
    refused(&mut h, Operation::PrepareErst { segment: EVT, size: 15 }, Reason::ErstSegmentTooSmall);
    refused(&mut h, Operation::PrepareErst { segment: EVT, size: 0 }, Reason::ErstSegmentTooSmall);
    refused(&mut h, Operation::PrepareErst { segment: EVT, size: 128 }, Reason::ErstSegmentSizeUnsupported);
    refused(&mut h, Operation::PrepareErst { segment: EVT, size: 512 }, Reason::ErstSegmentSizeUnsupported);
    ok(&mut h, Operation::PrepareErst { segment: EVT, size: 256 });
}

// ---- command ring and cycle bits ---------------------------------------------------------------------------

#[test]
fn command_ring_cycle_and_link_semantics() {
    let mut h = running();
    let noop = |h: &mut Harness| {
        ok(h, Operation::BeginPollStep);
        let (i, c) = h.m.command_cursor();
        ok(h, Operation::PublishNoop { index: i, cycle: c });
        ok(h, Operation::RingCommandDoorbell);
        let (idx, ccs) = h.m.event_cursor();
        let e = h.d.read_event(idx, ccs).expect("completion");
        ok(h, Operation::ConsumeEvent(Some(e)));
        { let a_ = h.m.expected_erdp(); ok(h, Operation::SetEventDequeue { addr: a_, ehb: true }); }
    };
    refused(&mut h, Operation::RingCommandDoorbell, Reason::NothingStaged);
    refused(&mut h, Operation::PublishNoop { index: 1, cycle: true }, Reason::CommandIndexMismatch);
    refused(&mut h, Operation::PublishNoop { index: 0, cycle: false }, Reason::CommandCycleMismatch);
    refused(&mut h, Operation::PublishLink { index: 255, cycle: true, toggle: true, target: CMD }, Reason::LinkNotExpected);
    for _ in 0..254 {
        noop(&mut h);
    }
    assert_eq!(h.m.command_cursor(), (254, true));
    let (i, c) = h.m.command_cursor();
    ok(&mut h, Operation::PublishNoop { index: i, cycle: c });
    assert!(h.m.link_required());
    refused(&mut h, Operation::RingCommandDoorbell, Reason::LinkMissing);
    refused(&mut h, Operation::PublishNoop { index: 255, cycle: true }, Reason::CommandOutstanding);
    refused(&mut h, Operation::PublishLink { index: 254, cycle: true, toggle: true, target: CMD }, Reason::CommandIndexMismatch);
    refused(&mut h, Operation::PublishLink { index: 255, cycle: false, toggle: true, target: CMD }, Reason::CommandCycleMismatch);
    refused(&mut h, Operation::PublishLink { index: 255, cycle: true, toggle: false, target: CMD }, Reason::LinkToggleMissing);
    refused(&mut h, Operation::PublishLink { index: 255, cycle: true, toggle: true, target: CMD + 16 }, Reason::LinkTargetMismatch);
    ok(&mut h, Operation::PublishLink { index: 255, cycle: true, toggle: true, target: CMD });
    assert_eq!(h.m.command_cursor(), (0, false), "link published: cursor wraps and the producer cycle toggles");
    ok(&mut h, Operation::RingCommandDoorbell);
    refused(&mut h, Operation::RingCommandDoorbell, Reason::CommandOutstanding);
    refused(&mut h, Operation::PublishNoop { index: 0, cycle: false }, Reason::CommandOutstanding);
    let (idx, ccs) = h.m.event_cursor();
    let e = h.d.read_event(idx, ccs).expect("completion");
    ok(&mut h, Operation::ConsumeEvent(Some(e)));
    { let a_ = h.m.expected_erdp(); ok(&mut h, Operation::SetEventDequeue { addr: a_, ehb: true }); }
    noop(&mut h); // index 0 with the toggled cycle
    assert_eq!(h.m.noops_completed(), 256);
    assert!(h.d.violations.is_empty());
}

// ---- event ring, ERDP and EHB ----------------------------------------------------------------------------------

fn post_one(h: &mut Harness) -> EventTrb {
    let (i, c) = h.m.command_cursor();
    ok(h, Operation::PublishNoop { index: i, cycle: c });
    ok(h, Operation::RingCommandDoorbell);
    let (idx, ccs) = h.m.event_cursor();
    h.d.read_event(idx, ccs).expect("completion")
}

#[test]
fn erdp_and_ehb_acknowledgments() {
    let mut h = running();
    refused(&mut h, Operation::SetEventDequeue { addr: EVT, ehb: true }, Reason::ErdpRepeat);
    let e = post_one(&mut h);
    ok(&mut h, Operation::ConsumeEvent(Some(e)));
    assert_eq!(h.m.unacked(), 1);
    refused(&mut h, Operation::SetEventDequeue { addr: EVT, ehb: true }, Reason::ErdpMismatch);
    refused(&mut h, Operation::SetEventDequeue { addr: EVT + 16, ehb: false }, Reason::EhbNotSet);
    ok(&mut h, Operation::SetEventDequeue { addr: EVT + 16, ehb: true });
    assert_eq!(h.m.unacked(), 0);
    // Repeating the same acknowledgment is refused; a stale one is a mismatch.
    refused(&mut h, Operation::SetEventDequeue { addr: EVT + 16, ehb: true }, Reason::ErdpRepeat);
    refused(&mut h, Operation::SetEventDequeue { addr: EVT, ehb: true }, Reason::ErdpMismatch);
    assert!(h.d.violations.is_empty());
}

#[test]
fn a_missing_acknowledgment_blocks_the_next_doorbell() {
    let mut h = running();
    let e = post_one(&mut h);
    ok(&mut h, Operation::ConsumeEvent(Some(e)));
    let (i, c) = h.m.command_cursor();
    ok(&mut h, Operation::PublishNoop { index: i, cycle: c });
    refused(&mut h, Operation::RingCommandDoorbell, Reason::AckMissing);
    { let a_ = h.m.expected_erdp(); ok(&mut h, Operation::SetEventDequeue { addr: a_, ehb: true }); }
    ok(&mut h, Operation::RingCommandDoorbell);
}

#[test]
fn polling_is_bounded_per_step_and_per_unacknowledged_batch() {
    let mut h = running();
    // 40 port-change events pile up (a flood of attachments), none acknowledged.
    h.d.attach(0xff);
    for _ in 0..4 {
        h.d.attach(0);
    }
    let mut seen = 0;
    ok(&mut h, Operation::BeginPollStep);
    loop {
        let (idx, ccs) = h.m.event_cursor();
        let Some(e) = h.d.read_event(idx, ccs) else { break };
        ok(&mut h, Operation::ConsumeEvent(Some(e)));
        seen += 1;
    }
    assert_eq!(seen, 8);
    assert_eq!(h.m.port_events(), 8);
    assert_eq!(h.m.empty_polls(), 0);
    ok(&mut h, Operation::ConsumeEvent(None));
    assert_eq!(h.m.empty_polls(), 1);
    { let a_ = h.m.expected_erdp(); ok(&mut h, Operation::SetEventDequeue { addr: a_, ehb: true }); }
    // Drive the budgets with synthetic events: 32 per step, 32 unacknowledged.
    let mut m = Machine::new(ReleasePolicy::AfterQuiesce);
    let mut h2 = running();
    std::mem::swap(&mut m, &mut h2.m);
    let feed = |m: &mut Machine, n: u16| -> Result<Accepted, Reason> {
        rec(m, Operation::ConsumeEvent(Some(EventTrb { index: n, cycle: true, kind: 34, pointer: 0, code: 1, param: 0, slot: 0, vf: 0, port: 1 })))
    };
    assert!(rec(&mut m, Operation::BeginPollStep).is_ok());
    for n in 0..32 {
        feed(&mut m, n).unwrap();
    }
    assert_eq!(feed(&mut m, 32), Err(Reason::PollBudgetExceeded));
    assert!(rec(&mut m, Operation::BeginPollStep).is_ok());
    assert_eq!(feed(&mut m, 32), Err(Reason::AckMissing));
    let a_ = m.expected_erdp();
    assert!(rec(&mut m, Operation::SetEventDequeue { addr: a_, ehb: true }).is_ok());
    assert!(feed(&mut m, 32).is_ok());
}

#[test]
fn event_validation_refusals_and_faults() {
    let mut h = running();
    let e = post_one(&mut h);
    refused(&mut h, Operation::ConsumeEvent(Some(EventTrb { index: 5, ..e })), Reason::EventIndexMismatch);
    refused(&mut h, Operation::ConsumeEvent(Some(EventTrb { cycle: false, ..e })), Reason::EventCycleMismatch);
    // Faults: each is an accepted transition into quarantine with a named cause.
    let faults: Vec<(EventTrb, Reason)> = vec![
        (EventTrb { pointer: e.pointer + 16, ..e }, Reason::CompletionPointerMismatch),
        (EventTrb { code: 4, ..e }, Reason::CommandFailed),
        (EventTrb { slot: 1, ..e }, Reason::CompletionFieldInvalid),
        (EventTrb { vf: 1, ..e }, Reason::CompletionFieldInvalid),
        (EventTrb { param: 9, ..e }, Reason::CompletionFieldInvalid),
        (EventTrb { kind: 34, port: 0, ..e }, Reason::PortOutOfRange),
        (EventTrb { kind: 34, port: 9, ..e }, Reason::PortOutOfRange),
        (EventTrb { kind: 37, ..e }, Reason::UnexpectedEventType),
    ];
    for (bad, why) in faults {
        let mut hh = running();
        let e2 = post_one(&mut hh);
        let bad = EventTrb { index: e2.index, cycle: e2.cycle, pointer: if bad.pointer == e.pointer { e2.pointer } else { e2.pointer + 16 }, ..bad };
        let a = ok(&mut hh, Operation::ConsumeEvent(Some(bad)));
        assert_eq!((a.to, a.why), (Phase::Quarantined, Rule::FaultEvent), "{why:?}");
        assert_eq!(hh.m.terminal_reason(), Some(why));
    }
    // A completion without an outstanding command is a fault too.
    let mut hh = running();
    let stray = EventTrb { index: 0, cycle: true, kind: 33, pointer: CMD, code: 1, param: 0, slot: 0, vf: 0, port: 0 };
    ok(&mut hh, Operation::ConsumeEvent(Some(stray)));
    assert_eq!(hh.m.terminal_reason(), Some(Reason::UnexpectedCompletion));
}

// ---- interrupts and polling mode ---------------------------------------------------------------------------------

#[test]
fn interrupts_ports_slots_and_other_commands_are_forbidden_everywhere() {
    for f in Forbidden::ALL {
        let want = match f {
            Forbidden::InterruptEnable => Reason::ForbiddenInterruptEnable,
            Forbidden::PortWrite => Reason::ForbiddenPortWrite,
            Forbidden::SlotDoorbell => Reason::ForbiddenSlotDoorbell,
            Forbidden::Command => Reason::ForbiddenCommand,
        };
        for phase in [Phase::Idle, Phase::Owned, Phase::Contained, Phase::Programmed, Phase::EventRingArmed, Phase::Running] {
            let mut h = qemu();
            goto(&mut h, phase);
            let before = h.m.clone();
            refused(&mut h, Operation::Forbidden(*f), want);
            assert_eq!(before, h.m);
        }
    }
    let mut m = Machine::new(ReleasePolicy::AfterQuiesce);
    assert!(m.polling());
    assert_eq!(rec(&mut m, Operation::Forbidden(Forbidden::InterruptEnable)), Err(Reason::ForbiddenInterruptEnable));
}

#[test]
fn msi_observed_at_any_time_is_a_fault() {
    let mut h = running();
    let a = ok(&mut h, Operation::ObservePci { bme: true, msi: true });
    assert_eq!((a.to, a.why), (Phase::Quarantined, Rule::FaultMsi));
    assert_eq!(h.m.terminal_reason(), Some(Reason::MsiEnabled));
}

// ---- firmware ownership ----------------------------------------------------------------------------------------------

fn legacy_h(bios_release: Option<u32>) -> Harness {
    with(|q| { q.legacy = true; q.bios_release_polls = bios_release; })
}

#[test]
fn firmware_handoff_sequence_and_conflicts() {
    let mut h = legacy_h(Some(2));
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    assert_eq!(h.m.phase(), Phase::Qualified);
    refused(&mut h, Operation::DisableLegacySmi, Reason::SmiBeforeOwnership);
    refused(&mut h, Operation::ClearRunStop, Reason::OwnershipUnresolved);
    { let st = h.d.status(); refused(&mut h, Operation::ObserveStatus(st), Reason::ObservationNotExpected); }
    ok(&mut h, Operation::RequestOsOwnership);
    assert_eq!(h.m.phase(), Phase::Handoff);
    refused(&mut h, Operation::RequestOsOwnership, Reason::AlreadyOsOwned);
    refused(&mut h, Operation::DisableLegacySmi, Reason::SmiBeforeOwnership);
    let a = ok(&mut h, Operation::ObserveHandoff { bios: true, os: true });
    assert_eq!(a.why, Rule::HandoffPending);
    ok(&mut h, Operation::Tick(1));
    ok(&mut h, Operation::Tick(2));
    ok(&mut h, Operation::Tick(3));
    let (b, o) = h.d.handoff();
    assert_eq!((b, o), (false, true));
    let a = ok(&mut h, Operation::ObserveHandoff { bios: b, os: o });
    assert_eq!((a.to, a.why), (Phase::HandedOff, Rule::HandoffComplete));
    ok(&mut h, Operation::DisableLegacySmi);
    assert_eq!(h.m.phase(), Phase::Owned);
    refused(&mut h, Operation::DisableLegacySmi, Reason::WrongPhase);
    refused(&mut h, Operation::RequestOsOwnership, Reason::AlreadyOsOwned);
    // The BIOS taking the controller back after the handoff is a conflict.
    let a = ok(&mut h, Operation::ObserveHandoff { bios: true, os: true });
    assert_eq!((a.to, a.why), (Phase::Failed, Rule::FaultOwnership));
    assert_eq!(h.m.terminal_reason(), Some(Reason::OwnershipConflict));
}

#[test]
fn firmware_that_never_releases_ends_in_a_clean_failure_without_forcing() {
    let mut h = legacy_h(None);
    h.run(1, 10_000);
    assert_eq!((h.m.phase(), h.m.terminal_reason()), (Phase::Failed, Some(Reason::HandoffTimeout)));
    // Exactly one write was made (the OS-owned request); the BIOS bit was never forced clear.
    assert_eq!(h.d.writes.len(), 1);
    assert_eq!(h.d.writes[0].id, "N-04");
    assert!(h.d.bios_owned);
}

#[test]
fn ownership_lost_noop_and_no_capability_cases() {
    let mut h = legacy_h(Some(5));
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    ok(&mut h, Operation::RequestOsOwnership);
    ok(&mut h, Operation::ObserveHandoff { bios: false, os: false });
    assert_eq!(h.m.terminal_reason(), Some(Reason::OwnershipLost));
    let mut h = qemu();
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    refused(&mut h, Operation::RequestOsOwnership, Reason::NoLegacyCapability);
    refused(&mut h, Operation::DisableLegacySmi, Reason::NoLegacyCapability);
    refused(&mut h, Operation::ObserveHandoff { bios: false, os: true }, Reason::NoLegacyCapability);
    // Legacy with the OS already owning: only the SMI step remains.
    let mut h = legacy_h(Some(0));
    h.d.os_owned = true;
    h.d.bios_owned = false;
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    refused(&mut h, Operation::RequestOsOwnership, Reason::AlreadyOsOwned);
    ok(&mut h, Operation::ObserveHandoff { bios: false, os: true });
    ok(&mut h, Operation::DisableLegacySmi);
    // Both owners in Qualified is a conflict; a plain Qualified phase also sees BIOS+OS transitions.
    let mut h = legacy_h(Some(5));
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    ok(&mut h, Operation::ObserveHandoff { bios: true, os: false });
    let a = ok(&mut h, Operation::ObserveHandoff { bios: true, os: true });
    assert_eq!(a.why, Rule::FaultOwnership);
}

// ---- unexpected device attachment ---------------------------------------------------------------------------------------

#[test]
fn device_attachment_before_run_fails_and_after_run_is_only_noted() {
    let mut h = with(|q| q.connected = 0b100);
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    assert_eq!(h.m.terminal_reason(), Some(Reason::DeviceAttached));
    let mut h = qemu();
    goto(&mut h, Phase::Contained);
    refused(&mut h, Operation::ObservePorts { connected: 1 << 12 }, Reason::PortMaskInvalid);
    ok(&mut h, Operation::ObservePorts { connected: 0 });
    let a = ok(&mut h, Operation::ObservePorts { connected: 1 });
    assert_eq!((a.to, a.why), (Phase::Failed, Rule::FaultDevice));
    // After Run an attachment is noted, produces a port-change event that is consumed and
    // acknowledged, and changes nothing else: no port is written, no slot is enabled.
    let mut h = running();
    h.d.attach(0b10);
    { let c = h.d.connected; ok(&mut h, Operation::ObservePorts { connected: c }); }
    assert_eq!(h.m.devices_seen(), 0b10);
    let (idx, ccs) = h.m.event_cursor();
    let e = h.d.read_event(idx, ccs).expect("port change event");
    ok(&mut h, Operation::ConsumeEvent(Some(e)));
    assert_eq!(h.m.port_events(), 1);
    { let a_ = h.m.expected_erdp(); ok(&mut h, Operation::SetEventDequeue { addr: a_, ehb: true }); }
    assert_eq!(h.m.phase(), Phase::Running);
    assert!(h.d.writes.iter().all(|w| !matches!(w.target, Target::LegacyOsOwned)));
    // An attachment during start-up (after reset, before Run) is a failure with quarantine once exposed.
    let mut h = qemu();
    goto(&mut h, Phase::EventRingArmed);
    let a = ok(&mut h, Operation::ObservePorts { connected: 1 });
    assert_eq!((a.to, a.why), (Phase::Quarantined, Rule::FaultDevice));
}

// ---- DMA address validity, alignment and lifetimes -------------------------------------------------------------------------

#[test]
fn dma_address_alignment_and_overlap_rules() {
    let mut h = qemu();
    goto(&mut h, Phase::Owned);
    let k = PageKind::Command;
    refused(&mut h, Operation::DmaAllocate { kind: k, addr: CMD + 8 }, Reason::Misaligned);
    refused(&mut h, Operation::DmaAllocate { kind: k, addr: CMD + 0x800 }, Reason::Misaligned);
    refused(&mut h, Operation::DmaAllocate { kind: k, addr: 0 }, Reason::AddressRange);
    refused(&mut h, Operation::DmaAllocate { kind: k, addr: 0x8000 }, Reason::AddressRange);
    refused(&mut h, Operation::DmaAllocate { kind: k, addr: 0xf_f000 }, Reason::AddressRange);
    refused(&mut h, Operation::DmaAllocate { kind: k, addr: (1 << 30) - 0x1000 + 0x1000 }, Reason::AddressRange);
    refused(&mut h, Operation::DmaAllocate { kind: k, addr: 0xffff_ffff_ffff_f000 }, Reason::AddressRange);
    ok(&mut h, Operation::DmaAllocate { kind: k, addr: (1 << 30) - 0x1000 });
    refused(&mut h, Operation::DmaAllocate { kind: k, addr: CMD }, Reason::PageAlreadyOwned);
    ok(&mut h, Operation::DmaAllocate { kind: PageKind::Event, addr: CMD });
    refused(&mut h, Operation::DmaAllocate { kind: PageKind::Dcbaa, addr: CMD }, Reason::PageOverlap);
    refused(&mut h, Operation::DmaAllocate { kind: PageKind::Dcbaa, addr: (1 << 30) - 0x1000 }, Reason::PageOverlap);
}

#[test]
fn dma_page_lifetime_state_machine() {
    let mut h = qemu();
    goto(&mut h, Phase::Owned);
    let k = PageKind::Dcbaa;
    refused(&mut h, Operation::DmaZero(k), Reason::PageNotOwned);
    refused(&mut h, Operation::PrepareDcbaa { entry0: 0 }, Reason::PageNotOwned);
    refused(&mut h, Operation::DmaRelease(k), Reason::PageNotOwned);
    ok(&mut h, Operation::DmaAllocate { kind: k, addr: DCB });
    refused(&mut h, Operation::PrepareDcbaa { entry0: 0 }, Reason::PageState);
    ok(&mut h, Operation::DmaZero(k));
    refused(&mut h, Operation::DmaZero(k), Reason::PageState);
    ok(&mut h, Operation::PrepareDcbaa { entry0: 0 });
    refused(&mut h, Operation::PrepareDcbaa { entry0: 0 }, Reason::PageState);
    ok(&mut h, Operation::DmaRelease(k));
    // Use after release is refused: the page is gone.
    refused(&mut h, Operation::DmaZero(k), Reason::PageNotOwned);
    refused(&mut h, Operation::DmaRelease(k), Reason::PageNotOwned);
    refused(&mut h, Operation::SetDcbaap(DCB), Reason::ProgramBeforeContainment);
    goto(&mut h, Phase::Contained);
    refused(&mut h, Operation::SetDcbaap(DCB), Reason::PageNotPrepared);
}

#[test]
fn published_pages_are_never_released_before_quiescence() {
    let mut h = qemu();
    goto(&mut h, Phase::Programmed);
    for k in [PageKind::Command, PageKind::Event, PageKind::Dcbaa] {
        assert_eq!(h.m.page(k).state, PageState::Published);
        refused(&mut h, Operation::DmaRelease(k), Reason::PageStillPublished);
    }
    // The ERST is prepared but unpublished until ERSTBA: it can go, the rest cannot.
    assert_eq!(h.m.page(PageKind::Erst).state, PageState::Prepared);
    refused(&mut h, Operation::DmaAllocate { kind: PageKind::Erst, addr: ERS + 0x8000 }, Reason::AllocationClosed);
    ok(&mut h, Operation::SetBusMaster);
    goto(&mut h, Phase::Running);
    for k in ALL_PAGES {
        refused(&mut h, Operation::DmaRelease(k), Reason::PageStillPublished);
    }
    // Stop path: halt, clear BME, reset: only now does the proof exist.
    h.target = 0;
    goto(&mut h, Phase::Quiesced);
    ok(&mut h, Operation::DmaRelease(PageKind::Command));
    let mut never = Harness::new(Quirks::qemu(), ReleasePolicy::Never);
    never.run(2, 10_000);
    assert_eq!(never.m.phase(), Phase::Quiesced);
    for k in ALL_PAGES {
        refused(&mut never, Operation::DmaRelease(k), Reason::ReleaseForbiddenByPolicy);
    }
    assert!(ALL_PAGES.iter().all(|k| never.m.page(*k).state == PageState::Published), "physical policy: bounded retention");
}

#[test]
fn allocation_closes_once_the_device_is_exposed() {
    // Before exposure (and while halting for the first time) allocation is open and release is allowed.
    let mut h = qemu();
    goto(&mut h, Phase::Programming);
    ok(&mut h, Operation::DmaRelease(PageKind::Erst));
    let mut h = qemu();
    goto(&mut h, Phase::Halted);
    ok(&mut h, Operation::DmaAllocate { kind: PageKind::Command, addr: CMD });
    // Stopping an exposed device (cancelled before Run) keeps allocation closed.
    let mut h = qemu();
    goto(&mut h, Phase::Programmed);
    ok(&mut h, Operation::Cancel);
    assert_eq!(h.m.phase(), Phase::Halted);
    assert!(h.m.exposed());
    refused(&mut h, Operation::DmaAllocate { kind: PageKind::Erst, addr: ERS + 0x8000 }, Reason::AllocationClosed);
    refused(&mut h, Operation::DmaZero(PageKind::Erst), Reason::AllocationClosed);
    let mut h = running();
    refused(&mut h, Operation::DmaAllocate { kind: PageKind::Command, addr: CMD + 0x8000 }, Reason::AllocationClosed);
    refused(&mut h, Operation::DmaZero(PageKind::Command), Reason::AllocationClosed);
}

// ---- timeouts, reset failures, recovery, quarantine ------------------------------------------------------------------------------

fn tick_until_terminal(h: &mut Harness, limit: u64) {
    for _ in 0..limit {
        if h.m.phase().terminal() {
            return;
        }
        let t = h.now + 1;
        let _ = h.apply(Operation::Tick(t));
    }
}

#[test]
fn halt_never_reached_times_out_without_exposure() {
    let mut h = with(|q| { q.firmware_running = true; q.never_halts = true; });
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    ok(&mut h, Operation::ClearRunStop);
    tick_until_terminal(&mut h, 20);
    assert_eq!((h.m.phase(), h.m.terminal_reason()), (Phase::Failed, Some(Reason::HaltTimeout)));
    assert!(h.now <= HALT_TICKS + 1);
}

#[test]
fn reset_that_never_completes_times_out_before_and_after_exposure() {
    let mut h = with(|q| q.reset_polls = 10_000);
    goto(&mut h, Phase::Resetting);
    tick_until_terminal(&mut h, RESET_TICKS + 5);
    assert_eq!((h.m.phase(), h.m.terminal_reason()), (Phase::Failed, Some(Reason::ResetTimeout)));
    // After exposure the same failure is a quarantine and keeps every page.
    let mut h = qemu();
    goto(&mut h, Phase::Running);
    h.d.q.reset_polls = 10_000;
    ok(&mut h, Operation::ClearRunStop);
    { let st = h.d.status(); ok(&mut h, Operation::ObserveStatus(st)); }
    ok(&mut h, Operation::ClearBusMaster);
    ok(&mut h, Operation::ObservePci { bme: false, msi: false });
    ok(&mut h, Operation::AssertReset);
    tick_until_terminal(&mut h, RESET_TICKS + 5);
    assert_eq!((h.m.phase(), h.m.terminal_reason()), (Phase::Quarantined, Some(Reason::ResetTimeout)));
    for k in ALL_PAGES {
        refused(&mut h, Operation::DmaRelease(k), Reason::AfterTerminal);
        assert_ne!(h.m.page(k).state, PageState::Free);
    }
}

#[test]
fn reset_that_does_not_clear_the_controller_is_unverified() {
    let mut h = with(|q| q.reset_leaves_pointers = true);
    goto(&mut h, Phase::Running);
    ok(&mut h, Operation::ClearRunStop);
    { let st = h.d.status(); ok(&mut h, Operation::ObserveStatus(st)); }
    ok(&mut h, Operation::ClearBusMaster);
    ok(&mut h, Operation::ObservePci { bme: false, msi: false });
    ok(&mut h, Operation::AssertReset);
    let rs_ = h.d.reset_state(); let a = ok(&mut h, Operation::ObserveReset(rs_));
    assert_eq!((a.to, a.why), (Phase::Quarantined, Rule::FaultResetUnverified));
    assert_eq!(h.m.terminal_reason(), Some(Reason::ResetUnverified));
    // Each missing reset property is individually fatal.
    for bad in [ResetState { hch: false, ..good_reset() }, ResetState { usbcmd_zero: false, ..good_reset() },
                ResetState { pointers_zero: false, ..good_reset() }, ResetState { bme: true, ..good_reset() },
                ResetState { ports_quiet: false, ..good_reset() }] {
        let mut h = qemu();
        goto(&mut h, Phase::Resetting);
        let a = ok(&mut h, Operation::ObserveReset(bad));
        assert_eq!((a.to, a.why), (Phase::Failed, Rule::FaultResetUnverified), "{bad:?}");
    }
}
fn good_reset() -> ResetState {
    ResetState { cnr: false, hch: true, usbcmd_zero: true, pointers_zero: true, bme: false, ports_quiet: true }
}

#[test]
fn start_and_command_timeouts_quarantine_and_run_the_containment_tail() {
    // Command that never completes.
    let mut h = with(|q| q.completion_polls = 10_000);
    goto(&mut h, Phase::Running);
    let (i, c) = h.m.command_cursor();
    ok(&mut h, Operation::PublishNoop { index: i, cycle: c });
    ok(&mut h, Operation::RingCommandDoorbell);
    tick_until_terminal(&mut h, COMMAND_TICKS + 5);
    assert_eq!((h.m.phase(), h.m.terminal_reason()), (Phase::Quarantined, Some(Reason::CommandTimeout)));
    // The controller is still running: the tail halts first, then clears BME, then verifies.
    assert_eq!(h.m.tail(), "need-halt");
    refused(&mut h, Operation::ClearBusMaster, Reason::AfterTerminal);
    refused(&mut h, Operation::AssertReset, Reason::AfterTerminal);
    let a = ok(&mut h, Operation::ClearRunStop);
    assert_eq!((a.why, a.to), (Rule::QuarantineHalt, Phase::Quarantined));
    assert_eq!(h.m.tail(), "await-halt");
    { let st = h.d.status(); ok(&mut h, Operation::ObserveStatus(st)); }
    assert_eq!(h.m.tail(), "need-bme-clear");
    ok(&mut h, Operation::ClearBusMaster);
    { let b = h.d.pci().0; ok(&mut h, Operation::ObservePci { bme: b, msi: false }); }
    assert_eq!(h.m.tail(), "done");
    assert!(!h.d.bme);
    refused(&mut h, Operation::ClearBusMaster, Reason::AfterTerminal);
    assert!(h.d.violations.is_empty(), "{:?}", h.d.violations);
    // Start that never leaves the halted state.
    let mut h = with(|q| q.start_polls = 10_000);
    goto(&mut h, Phase::Starting);
    tick_until_terminal(&mut h, START_TICKS + 5);
    assert_eq!(h.m.terminal_reason(), Some(Reason::StartTimeout));
}

#[test]
fn a_stuck_bus_master_is_reported_and_never_retried() {
    let mut h = with(|q| q.stuck_bme = true);
    goto(&mut h, Phase::Running);
    ok(&mut h, Operation::ObserveStatus(Status { hch: false, hse: true, hce: false }));
    assert_eq!(h.m.phase(), Phase::Quarantined);
    assert_eq!(h.m.tail(), "need-halt");
    h.run(0, 20);
    assert!(h.m.bme_stuck() && h.d.bme);
    assert_eq!(h.m.tail(), "done");
    refused(&mut h, Operation::ClearBusMaster, Reason::AfterTerminal);
}

#[test]
fn controller_errors_clock_faults_and_unexpected_halts() {
    for s in [Status { hch: true, hse: true, hce: false }, Status { hch: true, hse: false, hce: true }] {
        let mut h = running();
        let a = ok(&mut h, Operation::ObserveStatus(s));
        assert_eq!((a.to, a.why), (Phase::Quarantined, Rule::FaultStatus));
        assert_eq!(h.m.terminal_reason(), Some(Reason::ControllerFault));
    }
    let mut h = running();
    let a = ok(&mut h, Operation::ObserveStatus(Status { hch: true, hse: false, hce: false }));
    assert_eq!(a.why, Rule::FaultUnexpectedHalt);
    assert_eq!(h.m.terminal_reason(), Some(Reason::UnexpectedHalt));
    assert_eq!(h.m.tail(), "need-halt", "R/S is still believed set; clearing it is harmless and keeps the 4.21.2 order");
    let mut h = qemu();
    goto(&mut h, Phase::Contained);
    ok(&mut h, Operation::Tick(100));
    let a = ok(&mut h, Operation::Tick(99));
    assert_eq!((a.why, a.to), (Rule::FaultClock, Phase::Failed));
    assert_eq!(h.m.terminal_reason(), Some(Reason::ClockBackwards));
    let mut h = qemu();
    goto(&mut h, Phase::Owned);
    let a = ok(&mut h, Operation::ObserveStatus(Status { hch: false, hse: false, hce: false }));
    assert_eq!(a.why, Rule::FaultInconsistent);
}

#[test]
fn cancel_routes_every_phase_to_a_safe_path() {
    let mut h = qemu();
    goto(&mut h, Phase::Owned);
    let a = ok(&mut h, Operation::Cancel);
    assert_eq!((a.to, a.why), (Phase::Failed, Rule::FaultCancelled));
    for phase in [Phase::Contained, Phase::Programming, Phase::Programmed, Phase::BusMastering, Phase::EventRingArmed] {
        let mut h = qemu();
        goto(&mut h, phase);
        ok(&mut h, Operation::Cancel);
        assert_eq!(h.m.phase(), Phase::Halted);
        h.run(0, 100);
        assert_eq!(h.m.phase(), Phase::Quiesced, "{phase:?}");
        assert!(h.d.violations.is_empty(), "{phase:?}: {:?}", h.d.violations);
    }
    let mut h = running();
    refused(&mut h, Operation::Cancel, Reason::UseClearRunStop);
    refused(&mut h, Operation::ClearBusMaster, Reason::BmeClearBeforeHalt);
    refused(&mut h, Operation::AssertReset, Reason::ResetBeforeHalt);
}

#[test]
fn halt_requests_are_refused_when_there_is_nothing_to_halt() {
    // An idle controller (R/S already clear) must not be sent an unneeded R/S write.
    let mut h = qemu();
    goto(&mut h, Phase::Owned);
    assert!(!h.m.run_stop());
    refused(&mut h, Operation::ClearRunStop, Reason::AlreadyHalted);
    assert!(h.d.writes.is_empty());
    let st = h.d.status();
    ok(&mut h, Operation::ObserveStatus(st));
    refused(&mut h, Operation::ClearRunStop, Reason::AlreadyHalted);
    for phase in [Phase::Contained, Phase::Programmed, Phase::EventRingArmed] {
        let mut h = qemu();
        goto(&mut h, phase);
        refused(&mut h, Operation::ClearRunStop, Reason::AlreadyHalted);
    }
}

#[test]
fn deadlines_expire_exactly_at_their_tick_count() {
    // One tick short of the deadline is still waiting; the deadline tick itself is the timeout.
    for (setup, ticks, cause) in [(Phase::Halting, HALT_TICKS, Reason::HaltTimeout), (Phase::Resetting, RESET_TICKS, Reason::ResetTimeout),
                                  (Phase::Starting, START_TICKS, Reason::StartTimeout)] {
        let mut h = with(|q| match setup {
            Phase::Halting => { q.firmware_running = true; q.halt_polls = 10_000 }
            Phase::Resetting => q.reset_polls = 10_000,
            _ => q.start_polls = 10_000,
        });
        if setup == Phase::Halting {
            { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
            ok(&mut h, Operation::ClearRunStop);
        } else {
            goto(&mut h, setup);
        }
        let start = h.now;
        let a = ok(&mut h, Operation::Tick(start + ticks - 1));
        assert_eq!(a.why, Rule::TickAdvance, "{setup:?}");
        let a = ok(&mut h, Operation::Tick(start + ticks));
        assert_eq!(a.why, Rule::FaultTimeout, "{setup:?}");
        assert_eq!(h.m.terminal_reason(), Some(cause));
    }
}

#[test]
fn a_completed_command_leaves_no_deadline_behind() {
    let mut h = running();
    let e = post_one(&mut h);
    ok(&mut h, Operation::ConsumeEvent(Some(e)));
    let a_ = h.m.expected_erdp();
    ok(&mut h, Operation::SetEventDequeue { addr: a_, ehb: true });
    // Far beyond the command deadline: a healthy idle controller must not be quarantined.
    let t = h.now + 10 * COMMAND_TICKS;
    let a = ok(&mut h, Operation::Tick(t));
    assert_eq!((a.to, a.why), (Phase::Running, Rule::TickAdvance));
    assert_eq!(h.m.phase(), Phase::Running);
}

#[test]
fn the_halted_belief_is_invalidated_whenever_run_stop_is_written() {
    let mut h = qemu();
    goto(&mut h, Phase::EventRingArmed);
    let st = h.d.status();
    ok(&mut h, Operation::ObserveStatus(st));
    assert!(h.m.halt_observed(), "halted was read before Run");
    ok(&mut h, Operation::SetRunStop);
    assert!(!h.m.halt_observed(), "writing R/S makes the earlier halted read stale");
    let st = h.d.status();
    ok(&mut h, Operation::ObserveStatus(st));
    ok(&mut h, Operation::ClearRunStop);
    assert!(!h.m.halt_observed(), "clearing R/S makes it unknown until the next read");
    let st = h.d.status();
    ok(&mut h, Operation::ObserveStatus(st));
    assert!(h.m.halt_observed());
    // A firmware-running controller: the halt request also invalidates the belief.
    let mut h = with(|q| q.firmware_running = true);
    { let f = h.d.facts(); ok(&mut h, Operation::Qualify(f)); }
    assert!(!h.m.halt_observed());
    ok(&mut h, Operation::ClearRunStop);
    assert!(!h.m.halt_observed());
}

#[test]
fn terminal_phases_accept_nothing_but_their_defined_cleanup() {
    // Build one machine in each terminal phase; then every sample operation must be refused
    // (or be the single defined cleanup) and must not change the phase or the terminal cause.
    let samples = || -> Vec<Operation> {
        vec![
            Operation::Tick(1_000), Operation::SetBusMaster, Operation::SetRunStop, Operation::SetErstBase(ERS), Operation::AssertReset,
            Operation::RingCommandDoorbell, Operation::ConsumeEvent(None), Operation::Cancel, Operation::BeginPollStep,
            Operation::DmaAllocate { kind: PageKind::Command, addr: CMD }, Operation::SetMaxSlots(1), Operation::RequestOsOwnership,
            Operation::ObservePorts { connected: 0 }, Operation::ObserveHandoff { bios: false, os: true },
            Operation::Qualify(Device::new(Quirks::qemu()).facts()), Operation::PublishNoop { index: 0, cycle: true },
            Operation::Forbidden(Forbidden::PortWrite), Operation::ObserveReset(good_reset()),
        ]
    };
    let mut machines: Vec<(&str, Harness)> = Vec::new();
    let mut refused_m = qemu();
    ok(&mut refused_m, Operation::Qualify(bad_facts(|f| f.msi = true)));
    machines.push(("refused", refused_m));
    let mut failed = qemu();
    goto(&mut failed, Phase::Owned);
    ok(&mut failed, Operation::Cancel);
    assert_eq!(failed.m.phase(), Phase::Failed);
    machines.push(("failed", failed));
    let mut quar = running();
    ok(&mut quar, Operation::ObservePci { bme: true, msi: true });
    machines.push(("quarantined", quar));
    let mut quiesced = qemu();
    quiesced.run(1, 10_000);
    machines.push(("quiesced", quiesced));
    for (name, mut h) in machines {
        let phase = h.m.phase();
        let cause = h.m.terminal_reason();
        for op in samples() {
            let before = h.m.clone();
            let r = h.apply(op);
            assert_eq!(r, Err(Reason::AfterTerminal), "{name}: {op:?}");
            assert_eq!(before, h.m);
            assert_eq!((h.m.phase(), h.m.terminal_reason()), (phase, cause));
        }
    }
}

// ---- randomized transitions ----------------------------------------------------------------------------------------------------------

/// The declared phase graph (independent of the machine). Self-loops are always allowed; a fault or
/// timeout may enter Failed or Quarantined from any non-terminal phase.
fn edge_ok(from: Phase, to: Phase) -> bool {
    use Phase::*;
    if from == to || (!from.terminal() && matches!(to, Failed | Quarantined)) {
        return true;
    }
    matches!(
        (from, to),
        (Idle, Refused) | (Idle, Owned) | (Idle, Qualified)
            | (Qualified, Handoff) | (Qualified, Owned) | (Handoff, HandedOff) | (HandedOff, Owned)
            | (Owned, Halting) | (Owned, Halted) | (Halting, Halted) | (Halted, Resetting)
            | (Resetting, Contained) | (Resetting, Quiesced)
            | (Contained, Programming) | (Programming, Programmed) | (Contained, Programmed)
            | (Programmed, BusMastering) | (BusMastering, EventRingArmed) | (EventRingArmed, Starting)
            | (Starting, Running) | (Starting, Halting) | (Running, Halting)
            | (Contained, Halted) | (Programming, Halted) | (Programmed, Halted) | (BusMastering, Halted) | (EventRingArmed, Halted)
    )
}

fn is_register_write(op: &Operation) -> bool {
    matches!(op, Operation::RequestOsOwnership | Operation::DisableLegacySmi | Operation::ClearRunStop | Operation::ClearBusMaster
        | Operation::AssertReset | Operation::SetMaxSlots(_) | Operation::SetDcbaap(_) | Operation::SetCommandRing { .. }
        | Operation::SetErstSize(_) | Operation::SetEventDequeue { .. } | Operation::SetBusMaster | Operation::SetErstBase(_)
        | Operation::SetRunStop | Operation::RingCommandDoorbell)
}

/// Independent write-order checker over the forwarded event log (does not use the machine).
fn check_event_log(events: &[ModelEvent], q: &Quirks) -> Result<(), String> {
    let (mut rs, mut bme) = (q.firmware_running, q.firmware_bme);
    let (mut statics, mut erstba) = (0u8, false);
    for (i, e) in events.iter().enumerate() {
        let at = |m: &str| Err(format!("event {i} {} {}: {m}", e.id, e.target.name()));
        match e.target {
            Target::RunStop if e.value == 1 => {
                if !(erstba && bme && statics == 31 && !rs) { return at("R/S set before ERSTBA/BME/statics"); }
                rs = true;
            }
            Target::RunStop => rs = false,
            Target::PciBme if e.value == 1 => {
                if statics != 31 || rs || bme || erstba { return at("BME set out of order"); }
                bme = true;
            }
            Target::PciBme => {
                if rs { return at("BME cleared while R/S set"); }
                bme = false;
            }
            Target::Hcrst => {
                if rs || bme { return at("HCRST with R/S or BME set"); }
                statics = 0;
                erstba = false;
            }
            Target::Erstba => {
                if !(bme && statics == 31 && !erstba) { return at("ERSTBA out of order"); }
                erstba = true;
            }
            Target::Config | Target::Dcbaap | Target::Crcr | Target::Erstsz => {
                let bit = match e.target { Target::Config => 1, Target::Dcbaap => 2, Target::Crcr => 4, _ => 8 };
                if rs || bme || statics & bit != 0 { return at("static register out of order or repeated"); }
                statics |= bit;
            }
            Target::Erdp => {
                if !rs && !bme && statics & 16 == 0 {
                    statics |= 16;
                } else if !rs || e.id != "N-15" {
                    return at("ERDP out of order");
                }
            }
            Target::Doorbell0 => {
                if !rs || e.value != 0 { return at("doorbell while halted or wrong target"); }
            }
            Target::LegacyOsOwned | Target::LegacySmi => {
                if statics != 0 || rs && !q.firmware_running { return at("ownership write after programming"); }
            }
        }
    }
    Ok(())
}

fn tail(h: &Harness) -> String {
    let ok: Vec<_> = h.log.iter().filter(|(o, r)| r.is_ok() && !matches!(o, Operation::Tick(_))).collect();
    ok[ok.len().saturating_sub(24)..].iter().map(|(o, r)| format!("  {} => {}", encode(o), outcome(r))).collect::<Vec<_>>().join("\n")
}

fn walk(seed: u64, steps: usize, truthful: bool) -> Harness {
    let mut r = Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
    let policy = if r.chance(1, 5) { ReleasePolicy::Never } else { ReleasePolicy::AfterQuiesce };
    let mut h = Harness::new(random_quirks(&mut r), policy);
    let target = 1 + r.below(300) as u32;
    let mut published_ever = false;
    for _ in 0..steps {
        let op = if r.chance(2, 3) { h.next(target).unwrap_or_else(|| random_op(&mut r, &h)) } else { random_op(&mut r, &h) };
        let op = if truthful { truthify(op, &h) } else { op };
        if r.chance(1, 60) {
            let m = r.below(0xff) as u32;
            h.d.attach(m);
        }
        let before = h.m.phase();
        let was_terminal = before.terminal();
        let pages_before: Vec<PageState> = ALL_PAGES.iter().map(|k| h.m.page(*k).state).collect();
        let res = h.apply(op);
        let after = h.m.phase();
        if let Ok(a) = res {
            assert_eq!((a.from, a.to), (before, after), "{op:?}");
            assert!(edge_ok(a.from, a.to), "undeclared transition {} -> {} by {op:?}", a.from.name(), a.to.name());
            assert_eq!(a.event.is_some(), is_register_write(&op), "{op:?}: {a:?}");
            if was_terminal {
                assert_eq!(before, after, "phase changed after a terminal failure: {op:?}");
            }
            if let Some(e) = a.event {
                assert!(e.id.starts_with("N-"), "{e:?}");
            }
        }
        if was_terminal {
            assert_eq!(before, after);
        }
        // Published pages leave the Published state only through an accepted release in Quiesced.
        for (k, was) in ALL_PAGES.iter().zip(pages_before) {
            let now = h.m.page(*k).state;
            if was == PageState::Published && now != PageState::Published {
                assert!(matches!(op, Operation::DmaRelease(_)) && before == Phase::Quiesced && res.is_ok(), "{k:?} {op:?}");
            }
            if now == PageState::Published {
                published_ever = true;
            }
        }
        if published_ever {
            assert_ne!(h.m.phase(), Phase::Failed, "an exposed device must never end in a clean failure");
            assert_ne!(h.m.phase(), Phase::Refused);
        }
        if h.m.phase() == Phase::Idle && r.chance(1, 50) {
            h.now += 1;
        }
    }
    // Safety property: given truthful observations, the device never saw a violation, because only
    // accepted writes reach it. (A protocol cannot be safe against false observations.)
    if truthful {
        assert!(h.d.violations.is_empty(), "seed {seed}: {:?}\n{}", h.d.violations, tail(&h));
    }
    if let Err(e) = check_event_log(&h.d.writes, &h.d.q) {
        panic!("seed {seed}: {e}\n{}", tail(&h));
    }
    h
}

const SEEDS: u64 = 1500;
const STEPS: usize = 260;

#[test]
fn randomized_transitions_keep_every_invariant() {
    let mut phases: HashSet<&str> = HashSet::new();
    let mut terminal_causes: HashSet<&str> = HashSet::new();
    let mut quiesced = 0;
    for seed in 0..SEEDS {
        let h = walk(seed, STEPS, true);
        phases.insert(h.m.phase().name());
        if let Some(c) = h.m.terminal_reason() {
            terminal_causes.insert(c.name());
        }
        quiesced += (h.m.phase() == Phase::Quiesced) as u32;
    }
    assert!(quiesced > 100, "random walks must also reach complete sessions: {quiesced}");
    for p in ["quiesced", "failed", "quarantined", "refused", "running"] {
        assert!(phases.contains(p), "no walk ended in {p}: {phases:?}");
    }
    assert!(terminal_causes.len() >= 15, "terminal causes seen: {terminal_causes:?}");
}

#[test]
fn randomized_transitions_with_false_observations_keep_the_state_invariants() {
    // Lying observations may drive the machine into a quarantine or an unsafe-for-hardware write
    // sequence, but never into an inconsistent state, an unjustified transition or a changed terminal phase.
    let mut ended: HashSet<&str> = HashSet::new();
    for seed in 0..SEEDS {
        let h = walk(seed + 50_000, STEPS, false);
        ended.insert(h.m.phase().name());
    }
    assert!(ended.contains("quarantined") && ended.contains("refused"), "{ended:?}");
}

#[test]
fn random_sessions_with_the_canonical_driver_never_violate_the_device() {
    for seed in 0..300 {
        let mut r = Rng(seed + 7_000);
        let mut q = random_quirks(&mut r);
        q.scratchpads = 0;
        q.dbc = Dbc::Absent;
        q.iommu = Tri::No;
        q.identity = Tri::Yes;
        q.no_snoop = Tri::No;
        q.msi = false;
        q.connected = 0;
        q.bios_release_polls = Some(1);
        q.never_halts = false;
        q.stuck_bme = false;
        q.reset_leaves_pointers = false;
        let mut h = Harness::new(q, ReleasePolicy::AfterQuiesce);
        h.run(1 + (seed % 270) as u32, 200_000);
        assert_eq!(h.m.phase(), Phase::Quiesced, "seed {seed}: {:?}", h.m.terminal_reason());
        assert!(h.d.violations.is_empty());
        check_event_log(&h.d.writes, &h.d.q).unwrap();
    }
}

#[test]
fn the_event_log_checker_itself_rejects_misordered_logs() {
    let q = Quirks::qemu();
    let w = |t: Target, v: u64, id: &'static str| ModelEvent { id, target: t, width: 32, value: v };
    // ERSTBA before BME; BME before statics; R/S before ERSTBA; HCRST with BME; doorbell while halted.
    let bad: Vec<Vec<ModelEvent>> = vec![
        vec![w(Target::Erstba, 1, "N-12")],
        vec![w(Target::PciBme, 1, "N-11")],
        vec![w(Target::RunStop, 1, "N-13")],
        vec![w(Target::Doorbell0, 0, "N-14")],
        vec![w(Target::Config, 1, "N-06"), w(Target::Config, 1, "N-06")],
        vec![w(Target::Crcr, 1, "N-08"), w(Target::Dcbaap, 1, "N-07"), w(Target::Erstsz, 1, "N-09"), w(Target::Config, 1, "N-06"),
             w(Target::Erdp, 1, "N-10"), w(Target::PciBme, 1, "N-11"), w(Target::Hcrst, 2, "N-18")],
    ];
    for log in bad {
        assert!(check_event_log(&log, &q).is_err(), "{log:?}");
    }
    let mut h = qemu();
    h.run(3, 10_000);
    assert!(check_event_log(&h.d.writes, &h.d.q).is_ok());
}

// ---- trace export for the independent Python oracle ------------------------------------------------------------------------------------------

#[test]
fn export_traces_for_the_oracle() {
    let Ok(path) = std::env::var("XHCI_INIT_TRACE") else { return };
    use std::io::Write;
    let n: u64 = std::env::var("XHCI_INIT_SEEDS").ok().and_then(|s| s.parse().ok()).unwrap_or(600);
    let mut out = std::io::BufWriter::new(std::fs::File::create(path).unwrap());
    for seed in 0..n {
        let truthful = seed % 2 == 0;
        let h = walk(10_000 + seed, STEPS, truthful);
        writeln!(out, "T {} {} {}", 10_000 + seed, if truthful { "truthful" } else { "lying" }, h.m.policy().name()).unwrap();
        for ((op, res), cause) in h.log.iter().zip(&h.causes) {
            writeln!(out, "S {} => {} @ {}", encode(op), outcome(res), cause).unwrap();
        }
    }
}

// ---- completeness -----------------------------------------------------------------------------------------------------------------------------

/// Every named rule and every named reason is exercised by the suite. Runs last under
/// `--test-threads=1` (alphabetical order).
#[test]
fn zz_every_rule_and_every_reason_is_exercised() {
    // Make this test independent of execution order by re-running the cheap generators.
    for seed in 0..SEEDS {
        let _ = walk(seed, STEPS, true);
        let _ = walk(seed + 50_000, STEPS, false);
    }
    let g = COVERAGE.lock().unwrap();
    let (rules, reasons) = g.as_ref().unwrap();
    let missing_rules: Vec<_> = Rule::ALL.iter().map(|r| r.name()).filter(|n| !rules.contains(n)).collect();
    let missing_reasons: Vec<_> = Reason::ALL.iter().map(|r| r.name()).filter(|n| !reasons.contains(n)).collect();
    assert!(missing_rules.is_empty(), "rules never accepted: {missing_rules:?}");
    assert!(missing_reasons.is_empty(), "reasons never produced: {missing_reasons:?}");
}
