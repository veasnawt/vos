//! USB.2-C4.2a hosted tests for the backend contract: typed observations through a validating session,
//! sequence/identity/freshness handling, read and write faults, untrusted reports, ownership conflicts,
//! model-only DMA ownership with the provisional scratchpad policy, fail-closed behaviour after terminal
//! errors, seeded randomized runs with injected faults, and completeness of the named causes and reasons.
//! No test touches PCI, MMIO or physical memory. Cases where software CANNOT detect the problem are recorded
//! as such (they assert the limitation, they do not hide it).
#![forbid(unsafe_code)]
#[path = "xhci_init_backend_support.rs"]
mod support;
use support::base::*;
use support::*;
use vos_xhci_init::*;

fn qualified(f: impl FnOnce(&mut Quirks)) -> SH {
    let mut h = SH::with(f);
    ok(&mut h, Req::Bind);
    ok(&mut h, Req::Qualify);
    h
}
fn running() -> SH {
    let mut h = SH::qemu();
    h.goto(Phase::Running);
    h
}
fn ok(h: &mut SH, r: Req) -> Done {
    match h.req(r) {
        Ok(d) => {
            assert!(d.closed.is_none(), "{r:?} closed the session: {:?}", d.closed);
            d
        }
        Err(e) => panic!("{r:?} refused: {e:?} in {}", h.s.machine().phase().name()),
    }
}
fn closes(h: &mut SH, r: Req) -> Cause {
    match h.req(r) {
        Ok(Done { closed: Some(c), .. }) => c,
        other => panic!("{r:?} did not close the session: {other:?}"),
    }
}
fn refuses(h: &mut SH, r: Req, why: Refusal) {
    assert_eq!(h.req(r).unwrap_err(), why, "{r:?} in {}", h.s.machine().phase().name());
}
/// Schedule an injection for the k-th upcoming stamped backend call (1 = the next one).
fn inj(h: &mut SH, k: u64, i: Inj) {
    let c = h.fake().calls + k;
    h.fake().at(c, i);
}
fn writes(h: &mut SH) -> usize {
    h.fake().performed.len()
}

// ---- happy paths ----------------------------------------------------------------------------------------------

#[test]
fn a_session_completes_260_noops_with_consecutive_sequence_numbers_and_one_ticket_per_write() {
    let mut h = SH::qemu();
    h.run(260, 200_000);
    assert_eq!(h.s.machine().phase(), Phase::Quiesced, "{:?}", h.s.machine().terminal_reason());
    assert_eq!(h.s.machine().noops_completed(), 260);
    assert!(h.s.closed().is_none());
    assert!(h.fake().d.violations.is_empty(), "{:?}", h.fake().d.violations);
    let performed = writes(&mut h);
    assert_eq!(h.fake().expect_nonce - 1, performed as u64, "every write consumed exactly one ticket");
    assert_eq!(h.fake().seq - 100, h.s.last_seq().unwrap() - 100, "the session saw every backend operation");
    assert_eq!(h.fake().refused_writes, 0);
    assert!(h.fake().terminal, "the backend was told the session ended");
}

#[test]
fn the_write_sequence_matches_the_inventory() {
    let mut h = SH::qemu();
    h.run(3, 50_000);
    let ids: Vec<&str> = h.fake().performed.iter().map(|e| e.id).collect();
    let expected = ["N-03", "N-06", "N-07", "N-08", "N-09", "N-10", "N-11", "N-12", "N-13", "N-14", "N-15", "N-14", "N-15", "N-14", "N-15", "N-16", "N-17", "N-18"];
    assert_eq!(ids, expected);
}

#[test]
fn a_slow_legacy_firmware_running_controller_completes() {
    let mut q = Quirks::slow();
    q.firmware_running = true;
    q.firmware_bme = true;
    let mut h = SH::new(q, ReleasePolicy::AfterQuiesce);
    h.run(20, 100_000);
    assert_eq!(h.s.machine().phase(), Phase::Quiesced, "{:?} {:?}", h.s.machine().terminal_reason(), h.s.closed());
    assert!(h.fake().d.violations.is_empty(), "{:?}", h.fake().d.violations);
}

// ---- observations only through the backend --------------------------------------------------------------------------

#[test]
fn a_caller_cannot_supply_observations() {
    let mut h = running();
    let f = h.fake().d.facts();
    for op in [Operation::Qualify(f), Operation::Tick(9), Operation::ObserveHandoff { bios: false, os: true },
               Operation::ObserveStatus(Status { hch: true, hse: false, hce: false }),
               Operation::ObserveReset(ResetState { cnr: false, hch: true, usbcmd_zero: true, pointers_zero: true, bme: false, ports_quiet: true }),
               Operation::ObservePci { bme: false, msi: false }, Operation::ObservePorts { connected: 0 }, Operation::ConsumeEvent(None)] {
        refuses(&mut h, Req::Op(op), Refusal::Session(SReason::UseBackendObservation));
    }
}

#[test]
fn bind_requires_one_complete_topology_and_is_required_first() {
    let mut h = SH::qemu();
    refuses(&mut h, Req::Qualify, Refusal::Session(SReason::NotBound));
    refuses(&mut h, Req::Status, Refusal::Session(SReason::NotBound));
    ok(&mut h, Req::Bind);
    refuses(&mut h, Req::Bind, Refusal::Session(SReason::AlreadyBound));
    refuses(&mut h, Req::Status, Refusal::Session(SReason::NotQualified));
    for (setup, cause) in [(0u8, Cause::AmbiguousTopology), (1, Cause::AmbiguousTopology), (2, Cause::AmbiguousTopology)] {
        let mut h = SH::qemu();
        match setup {
            0 => h.fake().xhci_functions = 2,
            1 => h.fake().xhci_functions = 0,
            _ => h.fake().scan_complete = false,
        }
        assert_eq!(closes(&mut h, Req::Bind), cause);
        assert_eq!(h.s.machine().phase(), Phase::Idle);
        assert!(h.fake().terminal);
        refuses(&mut h, Req::Qualify, Refusal::Session(SReason::Closed));
        refuses(&mut h, Req::Bind, Refusal::Session(SReason::Closed));
    }
    let mut h = SH::qemu();
    inj(&mut h, 2, Inj::SeqSkip);
    assert_eq!(closes(&mut h, Req::Bind), Cause::SequenceGap);
}

#[test]
fn qualification_decodes_registers_into_facts_and_marks_observations_valid() {
    let mut h = qualified(|_| {});
    assert_eq!(h.s.machine().phase(), Phase::Owned);
    assert_eq!(h.s.ports(), 8);
    assert_eq!(h.s.knowledge(), (Fact::Known(false), Fact::Known(false), Fact::Known(true)));
    assert_eq!((0..5).map(|k| h.s.fresh(k)).collect::<Vec<_>>(), vec![true, true, false, true, false]);
    let _ = h.fake();
    let h = qualified(|q| { q.firmware_running = true; q.firmware_bme = true; q.legacy = true; q.bios_release_polls = Some(0); });
    assert_eq!(h.s.knowledge(), (Fact::Known(true), Fact::Known(true), Fact::Known(false)));
    assert_eq!(h.s.machine().phase(), Phase::Qualified);
    assert!(h.s.fresh(2));
    // a failed read during qualification, before anything is exposed, closes the session with the machine still idle
    let mut h = SH::qemu();
    ok(&mut h, Req::Bind);
    inj(&mut h, 1, Inj::Fault(ReadFault::Torn));
    assert_eq!(closes(&mut h, Req::Qualify), Cause::ReadTorn);
    assert_eq!(h.s.machine().phase(), Phase::Idle);
}

// ---- scratchpads (provisional policy) ----------------------------------------------------------------------------------

#[test]
fn scratchpad_counts_are_decoded_from_hcsparams2_and_refused_by_the_machine() {
    for s in [0u16, 1, 4, 31, 32, 63, 1023] {
        let hcs2 = 0xfu64 | (u64::from(s >> 5) << 21) | (u64::from(s & 31) << 27);
        assert_eq!(scratchpads(hcs2), s, "{s}");
    }
    for s in [1u16, 4, 31, 32, 1023] {
        let mut h = SH::with(|q| q.scratchpads = s);
        ok(&mut h, Req::Bind);
        let d = ok(&mut h, Req::Qualify);
        assert_eq!(d.accepted.unwrap().to, Phase::Refused);
        assert_eq!(h.s.machine().terminal_reason(), Some(Reason::ScratchpadsRequired), "{s}");
        assert!(h.s.closed().is_none(), "a machine refusal is not a session closure");
        assert!(h.fake().d.writes.is_empty());
    }
}

// ---- stale, invalidated and contradictory observations ---------------------------------------------------------------------

#[test]
fn a_write_invalidates_the_observations_it_can_change_and_later_writes_refuse_stale_inputs() {
    let mut h = qualified(|q| { q.firmware_running = true; q.firmware_bme = true; });
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    assert!(!h.s.fresh(0) && h.s.fresh(1), "R/S invalidates status only");
    // the session refuses before the machine is even consulted (the machine would say bme-clear-before-halt)
    refuses(&mut h, Req::Op(Operation::ClearBusMaster), Refusal::Session(SReason::StaleObservation));
    ok(&mut h, Req::Status);
    assert_eq!(h.s.machine().phase(), Phase::Halted);
    ok(&mut h, Req::Op(Operation::ClearBusMaster));
    assert!(!h.s.fresh(1), "a BME write invalidates the PCI observation");
    refuses(&mut h, Req::Op(Operation::AssertReset), Refusal::Session(SReason::StaleObservation));
    ok(&mut h, Req::Pci);
    ok(&mut h, Req::Op(Operation::AssertReset));
    assert!(!h.s.fresh(0) && !h.s.fresh(3) && !h.s.fresh(4), "reset invalidates status, ports and events");
}

#[test]
fn a_doorbell_needs_a_status_read_after_the_previous_doorbell() {
    let mut h = running();
    let (i, c) = h.s.machine().command_cursor();
    ok(&mut h, Req::Op(Operation::PublishNoop { index: i, cycle: c }));
    ok(&mut h, Req::Status);
    ok(&mut h, Req::Op(Operation::RingCommandDoorbell));
    assert!(!h.s.fresh(0) && !h.s.fresh(4));
    ok(&mut h, Req::Event);
    let a_ = h.s.machine().expected_erdp();
    ok(&mut h, Req::Op(Operation::SetEventDequeue { addr: a_, ehb: true }));
    let (i, c) = h.s.machine().command_cursor();
    ok(&mut h, Req::Op(Operation::PublishNoop { index: i, cycle: c }));
    refuses(&mut h, Req::Op(Operation::RingCommandDoorbell), Refusal::Session(SReason::StaleObservation));
    ok(&mut h, Req::Status);
    ok(&mut h, Req::Op(Operation::RingCommandDoorbell));
    // an acknowledgment needs a fresh event read
    let a_ = h.s.machine().expected_erdp();
    refuses(&mut h, Req::Op(Operation::SetEventDequeue { addr: a_, ehb: true }), Refusal::Session(SReason::StaleObservation));
}

#[test]
fn sequence_and_session_defects_close_the_session_fail_closed() {
    for (i, cause) in [(Inj::SeqDup, Cause::SequenceDuplicate), (Inj::SeqBack, Cause::SequenceReordered), (Inj::SeqSkip, Cause::SequenceGap),
                       (Inj::ForeignSession, Cause::SessionMismatch), (Inj::TickBack, Cause::ClockBackwards)] {
        let mut h = running();
        let before = writes(&mut h);
        inj(&mut h, 1, i);
        assert_eq!(closes(&mut h, Req::Status), cause, "{i:?}");
        assert_eq!(h.s.closed(), Some(cause));
        assert_eq!(h.s.machine().phase(), Phase::Quarantined, "an exposed controller is quarantined");
        assert_eq!(h.s.machine().terminal_reason(), Some(Reason::ControllerFault));
        assert!(h.fake().terminal);
        assert_eq!(writes(&mut h), before, "closing writes nothing");
        refuses(&mut h, Req::Tick, Refusal::Session(SReason::Closed));
        refuses(&mut h, Req::Op(Operation::RingCommandDoorbell), Refusal::Session(SReason::Closed));
    }
    // the session's own clock source going backwards
    let mut h = running();
    h.fake().now = 0;
    assert_eq!(closes(&mut h, Req::Tick), Cause::ClockBackwards);
}

#[test]
fn a_duplicated_observation_replayed_from_a_cache_is_rejected() {
    let mut h = running();
    ok(&mut h, Req::Status);
    // a backend that answers from a cache repeats the previous sequence number
    inj(&mut h, 1, Inj::SeqDup);
    assert_eq!(closes(&mut h, Req::Status), Cause::SequenceDuplicate);
}

#[test]
fn responses_about_the_wrong_register_or_width_are_rejected() {
    for (i, cause) in [(Inj::WrongReg, Cause::WrongRegister), (Inj::WrongIndex, Cause::WrongRegister), (Inj::BadWidth, Cause::WidthMismatch),
                       (Inj::Lie(0xffff_ffff), Cause::ReadAllOnes)] {
        let mut h = running();
        inj(&mut h, 1, i);
        assert_eq!(closes(&mut h, Req::Status), cause, "{i:?}");
    }
    let mut h = running();
    let (i, c) = h.s.machine().command_cursor();
    ok(&mut h, Req::Op(Operation::PublishNoop { index: i, cycle: c }));
    ok(&mut h, Req::Status);
    ok(&mut h, Req::Op(Operation::RingCommandDoorbell));
    inj(&mut h, 1, Inj::WrongIndex);
    assert_eq!(closes(&mut h, Req::Event), Cause::EventMismatch);
}

#[test]
fn every_read_fault_closes_with_its_own_cause_and_an_absent_optional_register_does_not() {
    for (k, cause) in [(ReadFault::AllOnes, Cause::ReadAllOnes), (ReadFault::Width, Cause::ReadWidth), (ReadFault::Misaligned, Cause::ReadMisaligned),
                       (ReadFault::OutOfExtent, Cause::ReadOutOfExtent), (ReadFault::Torn, Cause::ReadTorn), (ReadFault::Partial, Cause::ReadPartial),
                       (ReadFault::Budget, Cause::ReadBudget), (ReadFault::LeaseLost, Cause::LeaseLost), (ReadFault::Other, Cause::ReadOther),
                       (ReadFault::NotPresent, Cause::ReadOther)] {
        let mut h = running();
        inj(&mut h, 1, Inj::Fault(k));
        assert_eq!(closes(&mut h, Req::Status), cause, "{k:?}");
        assert!(h.s.machine().phase().terminal());
    }
    // the MSI/MSI-X controls are optional capabilities: not present is fine
    let mut h = running();
    inj(&mut h, 2, Inj::Fault(ReadFault::NotPresent));
    ok(&mut h, Req::Pci);
    // a failure before exposure ends in a clean failure
    let mut h = qualified(|_| {});
    inj(&mut h, 1, Inj::Fault(ReadFault::Partial));
    assert_eq!(closes(&mut h, Req::Status), Cause::ReadPartial);
    assert_eq!(h.s.machine().phase(), Phase::Failed);
}

#[test]
fn contradictory_observations_are_faults() {
    // reserved USBSTS bits set (at qualification and later)
    let mut h = SH::qemu();
    ok(&mut h, Req::Bind);
    inj(&mut h, 12, Inj::ReservedSts);
    assert_eq!(closes(&mut h, Req::Qualify), Cause::ReservedBitsSet);
    let mut h = qualified(|_| {});
    inj(&mut h, 1, Inj::ReservedSts);
    assert_eq!(closes(&mut h, Req::Status), Cause::ReservedBitsSet);
    // halted cannot become running without a Run/Stop write
    let mut h = qualified(|_| {});
    inj(&mut h, 1, Inj::Halt(false));
    assert_eq!(closes(&mut h, Req::Status), Cause::ImpossibleTransition);
    // CNR cannot appear without a reset
    let mut h = qualified(|_| {});
    inj(&mut h, 1, Inj::Cnr(5));
    assert_eq!(closes(&mut h, Req::Status), Cause::ImpossibleTransition);
    // bus master enable cannot change behind our back
    let mut h = qualified(|_| {});
    inj(&mut h, 1, Inj::Bme(true));
    assert_eq!(closes(&mut h, Req::Pci), Cause::UnexpectedChange);
}

#[test]
fn a_halted_report_is_corroborated_and_a_contradiction_is_caught() {
    // normal: one status read plus the USBCMD and second USBSTS corroboration reads
    let mut h = qualified(|_| {});
    ok(&mut h, Req::Status);
    assert_eq!(h.log.last().unwrap().io.len(), 3);
    // HCH=1 reported while USBCMD.R/S reads 1 after we cleared it
    let mut h = qualified(|q| q.firmware_running = true);
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    inj(&mut h, 1, Inj::Lie(1));
    inj(&mut h, 1, Inj::Rs(true));
    assert_eq!(closes(&mut h, Req::Status), Cause::UncorroboratedHalt);
    // the second USBSTS read disagrees
    let mut h = qualified(|q| q.firmware_running = true);
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    inj(&mut h, 3, Inj::Lie(0));
    assert_eq!(closes(&mut h, Req::Status), Cause::UncorroboratedHalt);
}

#[test]
fn limitation_a_consistent_liar_is_not_detected() {
    // RECORDED NEGATIVE CASE. The controller never halts, but every USBSTS read says HCH=1 and USBCMD.R/S
    // truthfully reads 0 after our clear. Corroboration cannot expose this: the session proceeds on a false halt.
    let mut h = qualified(|q| { q.firmware_running = true; q.never_halts = true; });
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    inj(&mut h, 1, Inj::Lie(1));
    inj(&mut h, 3, Inj::Lie(1));
    ok(&mut h, Req::Status);
    assert_eq!(h.s.machine().phase(), Phase::Halted, "the session accepted the false halted report");
    assert!(!h.fake().d.hch, "while the device is in fact not halted");
}

#[test]
fn a_controller_with_more_than_16_ports_is_refused_without_reading_ports() {
    let mut h = SH::with(|q| q.ports = 17);
    ok(&mut h, Req::Bind);
    let d = ok(&mut h, Req::Qualify);
    assert_eq!(d.accepted.unwrap().to, Phase::Refused);
    assert_eq!(h.s.machine().terminal_reason(), Some(Reason::BadCapabilities));
    assert!(h.log.last().unwrap().io.iter().all(|l| !l.starts_with("R portsc")), "no port register was read");
}

#[test]
fn the_crcr_read_quirk_is_a_configuration_not_an_assumption() {
    // xHCI 1.2 section 5.4.5: CRCR pointer bits read as zero; QEMU returns the pointer. After HCRST a QEMU-like
    // controller still reporting the pointer is only "unverified" when the session is told reads should be zero.
    for (zero, expect) in [(true, Phase::Quarantined), (false, Phase::Quiesced)] {
        let mut h = SH::new_cfg(Quirks::qemu(), ReleasePolicy::AfterQuiesce, Config { crcr_reads_zero: zero });
        h.goto(Phase::Running);
        h.fake().sticky_crcr = Some(CMD);
        h.run(0, 200);
        assert_eq!(h.s.machine().phase(), expect, "crcr_reads_zero={zero}");
    }
}

#[test]
fn the_last_write_sequence_number_follows_every_write() {
    let mut h = qualified(|q| q.firmware_running = true);
    assert_eq!(h.s.last_write_seq(), 0);
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    let s1 = h.fake().seq;
    assert_eq!(h.s.last_write_seq(), s1, "a verified write records its sequence number");
    let mut h = qualified(|q| q.firmware_running = true);
    h.fake().on_write.push(("N-01", Inj::WriteFault));
    closes(&mut h, Req::Op(Operation::ClearRunStop));
    assert_eq!(h.s.last_write_seq(), h.fake().seq, "so does a failed one");
}

#[test]
fn a_failure_while_the_controller_may_still_be_running_keeps_the_halt_unconfirmed() {
    let mut h = running();
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    inj(&mut h, 1, Inj::Fault(ReadFault::Torn));
    assert_eq!(closes(&mut h, Req::Status), Cause::ReadTorn);
    assert_eq!(h.s.machine().tail(), "await-halt", "the halt was never observed: bus mastering must not be cleared yet");
}

#[test]
fn debug_capability_firmware_ownership_msi_snoop_and_attachment_are_decoded_at_qualification() {
    for (dbc, phase, cause) in [(Dbc::Absent, Phase::Owned, None), (Dbc::Disabled, Phase::Owned, None), (Dbc::Enabled, Phase::Refused, Some(Reason::DbcEnabled))] {
        let mut h = SH::with(|q| q.dbc = dbc);
        ok(&mut h, Req::Bind);
        ok(&mut h, Req::Qualify);
        assert_eq!((h.s.machine().phase(), h.s.machine().terminal_reason()), (phase, cause), "{dbc:?}");
    }
    let mut h = SH::with(|q| q.dbc = Dbc::Unknown);
    ok(&mut h, Req::Bind);
    assert_eq!(closes(&mut h, Req::Qualify), Cause::ReadOther, "an unwalkable Debug Capability is not a favourable fact");
    // firmware ownership semaphores: BIOS only, then both set
    let mut h = SH::with(|q| { q.legacy = true; q.bios_release_polls = Some(0); });
    ok(&mut h, Req::Bind);
    ok(&mut h, Req::Qualify);
    assert_eq!(h.s.machine().phase(), Phase::Qualified);
    let mut h = SH::with(|q| { q.legacy = true; q.bios_release_polls = Some(0); });
    h.fake().d.os_owned = true;
    ok(&mut h, Req::Bind);
    ok(&mut h, Req::Qualify);
    assert_eq!(h.s.machine().terminal_reason(), Some(Reason::OwnershipConflict));
    // Enable No Snoop, MSI and an attached device each refuse by their own name
    for (setup, why) in [(0u8, Reason::NoSnoopEnabled), (1, Reason::MsiEnabled), (2, Reason::DeviceAttached), (3, Reason::NoSnoopUnresolved)] {
        let mut h = SH::with(|q| match setup {
            0 => q.no_snoop = Tri::Yes,
            1 => q.msi = true,
            2 => q.connected = 0b100,
            _ => q.no_snoop = Tri::Unknown,
        });
        ok(&mut h, Req::Bind);
        ok(&mut h, Req::Qualify);
        assert_eq!(h.s.machine().terminal_reason(), Some(why));
    }
}

#[test]
fn qualification_cannot_be_repeated_and_writes_before_it_are_a_machine_refusal() {
    let mut h = SH::qemu();
    ok(&mut h, Req::Bind);
    refuses(&mut h, Req::Op(Operation::ClearRunStop), Refusal::Machine(Reason::NotQualified));
    ok(&mut h, Req::Qualify);
    refuses(&mut h, Req::Qualify, Refusal::Machine(Reason::AlreadyQualified));
    assert!(h.log.last().unwrap().io.is_empty(), "a repeated qualification reads nothing");
}

#[test]
fn msi_enabled_after_qualification_is_a_machine_fault() {
    let mut h = running();
    inj(&mut h, 1, Inj::Msi(true));
    let d = ok(&mut h, Req::Pci);
    assert_eq!(d.accepted.unwrap().why, Rule::FaultMsi);
    assert_eq!(h.s.machine().terminal_reason(), Some(Reason::MsiEnabled));
}

#[test]
fn a_device_attached_during_reset_is_unverified_and_port_observation_reads_the_connection_bit() {
    let mut h = SH::qemu();
    h.goto(Phase::Resetting);
    h.fake().d.connected = 1;
    let d = ok(&mut h, Req::Reset);
    assert_eq!(d.accepted.unwrap().why, Rule::FaultResetUnverified);
    let mut h = running();
    h.fake().d.connected = 0b10;
    ok(&mut h, Req::Ports);
    assert_eq!(h.s.machine().devices_seen(), 0b10);
}

#[test]
fn static_register_writes_recheck_cnr_at_write_time() {
    let mut h = SH::qemu();
    h.goto(Phase::Contained);
    inj(&mut h, 3, Inj::Cnr(3));
    assert_eq!(closes(&mut h, Req::Op(Operation::SetMaxSlots(1))), Cause::PreconditionChanged);
    assert_eq!(writes(&mut h), 1, "only the reset wrote");
}

#[test]
fn a_readback_of_another_register_is_a_mismatch() {
    let mut h = qualified(|q| q.firmware_bme = true);
    ok(&mut h, Req::Status);
    ok(&mut h, Req::Pci);
    h.fake().on_write.push(("N-02", Inj::ReadbackWrongRegister));
    assert_eq!(closes(&mut h, Req::Op(Operation::ClearBusMaster)), Cause::WriteMismatch);
}

// ---- identity, topology and check-then-act changes ---------------------------------------------------------------------------

#[test]
fn identity_and_topology_changes_stop_a_write_before_it_happens() {
    for (i, cause) in [(Inj::Vendor(0x8086), Cause::IdentityChanged), (Inj::Bar(0xdead_0000), Cause::IdentityChanged), (Inj::Hcs2(2), Cause::IdentityChanged),
                       (Inj::SecondXhci, Cause::TopologyChanged), (Inj::ScanIncomplete, Cause::TopologyChanged)] {
        let mut h = qualified(|q| q.firmware_running = true);
        inj(&mut h, 1, i);
        assert_eq!(closes(&mut h, Req::Op(Operation::ClearRunStop)), cause, "{i:?}");
        assert_eq!(writes(&mut h), 0, "the write was not performed");
        assert_eq!(h.s.machine().phase(), Phase::Failed);
    }
}

#[test]
fn state_that_changes_between_validation_and_the_write_precondition_is_refused() {
    // identity (1), topology (2), then the precondition read (3): the controller stopped on its own
    let mut h = qualified(|q| q.firmware_running = true);
    inj(&mut h, 3, Inj::Rs(false));
    assert_eq!(closes(&mut h, Req::Op(Operation::ClearRunStop)), Cause::PreconditionChanged);
    assert_eq!(writes(&mut h), 0);
    // a controller that restarted before a bus-master clear
    let mut h = qualified(|q| { q.firmware_bme = true; });
    h.fake().d.hch = true;
    ok(&mut h, Req::Status);
    ok(&mut h, Req::Pci);
    assert_eq!(h.s.machine().phase(), Phase::Halted);
    inj(&mut h, 3, Inj::Halt(false));
    assert_eq!(closes(&mut h, Req::Op(Operation::ClearBusMaster)), Cause::PreconditionChanged);
    assert_eq!(writes(&mut h), 0);
}

#[test]
fn limitation_a_change_after_the_last_check_is_not_prevented() {
    // RECORDED NEGATIVE CASE. The device starts running after the precondition re-read and before the store.
    // Sequence numbers and re-reads narrow this window; they cannot close it. The device model records the
    // consequence (a bus-master clear while running); the session sees a verified, successful write.
    let mut h = qualified(|q| { q.firmware_bme = true; });
    ok(&mut h, Req::Status);
    ok(&mut h, Req::Pci);
    assert_eq!(h.s.machine().phase(), Phase::Halted);
    h.fake().write_race.push(("N-02", |d| {
        d.rs = true;
        d.hch = false;
    }));
    ok(&mut h, Req::Op(Operation::ClearBusMaster));
    assert!(h.s.closed().is_none(), "the session cannot know");
    assert!(h.fake().d.violations.contains(&Violation::BmeClearWhileRunning));
}

// ---- firmware ownership -----------------------------------------------------------------------------------------------------

#[test]
fn firmware_that_never_releases_ends_in_a_clean_failure_after_one_write() {
    let mut h = SH::with(|q| { q.legacy = true; q.bios_release_polls = None; });
    h.run(1, 5000);
    assert_eq!((h.s.machine().phase(), h.s.machine().terminal_reason()), (Phase::Failed, Some(Reason::HandoffTimeout)));
    assert_eq!(writes(&mut h), 1);
    assert!(h.fake().d.bios_owned, "the BIOS bit was never forced clear");
}

#[test]
fn firmware_regaining_ownership_is_a_conflict_or_a_precondition_failure() {
    let mut h = SH::with(|q| { q.legacy = true; q.bios_release_polls = Some(1); });
    ok(&mut h, Req::Bind);
    ok(&mut h, Req::Qualify);
    ok(&mut h, Req::Op(Operation::RequestOsOwnership));
    for _ in 0..6 {
        if h.s.machine().phase() != Phase::Handoff {
            break;
        }
        ok(&mut h, Req::Tick);
        ok(&mut h, Req::Handoff);
    }
    assert_eq!(h.s.machine().phase(), Phase::HandedOff);
    inj(&mut h, 3, Inj::Bios(true));
    assert_eq!(closes(&mut h, Req::Op(Operation::DisableLegacySmi)), Cause::PreconditionChanged);
    // an observed conflict after the handoff is the machine's fault
    let mut h = SH::with(|q| { q.legacy = true; q.bios_release_polls = Some(0); });
    h.goto(Phase::Owned);
    h.fake().d.bios_owned = true;
    let d = ok(&mut h, Req::Handoff);
    assert_eq!(d.accepted.unwrap().why, Rule::FaultOwnership);
    assert_eq!(h.s.machine().terminal_reason(), Some(Reason::OwnershipConflict));
    // no legacy capability: the machine refuses by name after a real read
    let mut h = qualified(|_| {});
    assert_eq!(h.req(Req::Handoff).unwrap_err(), Refusal::Machine(Reason::NoLegacyCapability));
}

// ---- failed and uncertain writes ----------------------------------------------------------------------------------------------

const UNCERTAIN: [(Inj, Cause); 7] = [
    (Inj::WriteRefused, Cause::WriteRefused),
    (Inj::WriteBadTicket, Cause::WriteBadTicket),
    (Inj::WriteFault, Cause::WriteFailed),
    (Inj::WriteUnverifiedDone, Cause::WriteUnverified),
    (Inj::WriteUnverifiedNotDone, Cause::WriteUnverified),
    (Inj::ReadbackMissing, Cause::WriteUnverified),
    (Inj::ReadbackWrong, Cause::WriteMismatch),
];

#[test]
fn an_uncertain_run_stop_clear_keeps_the_exposed_belief_and_marks_the_facts_unknown() {
    for (i, cause) in UNCERTAIN {
        let mut h = qualified(|q| q.firmware_running = true);
        h.fake().on_write.push(("N-01", i));
        assert_eq!(closes(&mut h, Req::Op(Operation::ClearRunStop)), cause, "{i:?}");
        assert!(h.s.machine().run_stop(), "a write that reduces exposure is not assumed to have happened: {i:?}");
        let (_, rs, hch) = h.s.knowledge();
        assert_eq!((rs, hch), (Fact::Unknown, Fact::Unknown), "{i:?}");
        assert!(!h.s.fresh(0));
    }
}

#[test]
fn an_uncertain_bus_master_enable_is_assumed_to_have_happened_and_the_tail_clears_it() {
    for (i, cause) in UNCERTAIN {
        let mut h = SH::qemu();
        h.goto(Phase::Programmed);
        h.fake().on_write.push(("N-11", i));
        assert_eq!(closes(&mut h, Req::Op(Operation::SetBusMaster)), cause, "{i:?}");
        let m = h.s.machine();
        assert_eq!((m.phase(), m.bus_master()), (Phase::Quarantined, true), "{i:?}: assumed enabled");
        assert_eq!(h.s.knowledge().0, Fact::Unknown);
        h.run(0, 40);
        assert_eq!(h.s.machine().tail(), "done", "{i:?}");
        assert!(!h.fake().d.bme, "{i:?}: the tail contained bus mastering");
        assert_eq!(h.s.knowledge().0, Fact::Known(false), "{i:?}: the verified read-back restored the fact");
        assert_eq!(h.fake().refused_writes, if i == Inj::WriteBadTicket { 1 } else { 0 });
    }
}

#[test]
fn a_pci_read_turns_an_unknown_bus_master_fact_into_a_known_one() {
    // After an uncertain enable the fact is Unknown; a PCI read (allowed in the tail) restores what was seen,
    // independently of any verified write.
    let mut h = SH::qemu();
    h.goto(Phase::Programmed);
    h.fake().on_write.push(("N-11", Inj::WriteUnverifiedDone));
    assert_eq!(closes(&mut h, Req::Op(Operation::SetBusMaster)), Cause::WriteUnverified);
    assert_eq!(h.s.knowledge().0, Fact::Unknown);
    let _ = h.req(Req::Pci); // the machine's tail refuses it (not its turn), but the read happened
    assert_eq!(h.s.knowledge().0, Fact::Known(true), "the device really has bus mastering enabled");
}

#[test]
fn an_uncertain_run_stop_set_is_assumed_to_have_happened_and_the_tail_halts_first() {
    let mut h = SH::qemu();
    h.goto(Phase::EventRingArmed);
    ok(&mut h, Req::Status);
    h.fake().on_write.push(("N-13", Inj::WriteUnverifiedNotDone));
    assert_eq!(closes(&mut h, Req::Op(Operation::SetRunStop)), Cause::WriteUnverified);
    assert_eq!(h.s.machine().tail(), "need-halt");
    h.run(0, 40);
    assert_eq!(h.s.machine().tail(), "done");
    assert!(!h.fake().d.rs && !h.fake().d.bme);
    assert!(h.fake().d.violations.is_empty(), "{:?}", h.fake().d.violations);
}

#[test]
fn a_failed_reset_write_leaves_the_exposed_belief_and_a_quarantine() {
    let mut h = SH::qemu();
    h.goto(Phase::Running);
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    ok(&mut h, Req::Status);
    ok(&mut h, Req::Pci);
    ok(&mut h, Req::Op(Operation::ClearBusMaster));
    ok(&mut h, Req::Pci);
    h.fake().on_write.push(("N-18", Inj::WriteRefused));
    assert_eq!(closes(&mut h, Req::Op(Operation::AssertReset)), Cause::WriteRefused);
    assert_eq!(h.s.machine().phase(), Phase::Quarantined);
    assert!(h.s.machine().exposed());
}

// ---- terminal behaviour -------------------------------------------------------------------------------------------------------

#[test]
fn after_a_terminal_error_only_the_quarantine_tail_is_possible() {
    let mut h = running();
    inj(&mut h, 1, Inj::SeqDup);
    assert_eq!(closes(&mut h, Req::Status), Cause::SequenceDuplicate);
    assert_eq!(h.s.machine().tail(), "need-halt");
    for r in [Req::Bind, Req::Qualify, Req::Tick, Req::Reset, Req::Handoff, Req::Ports, Req::Event,
              Req::Op(Operation::RingCommandDoorbell), Req::Op(Operation::SetBusMaster), Req::Op(Operation::AssertReset)] {
        let e = h.req(r).unwrap_err();
        assert!(matches!(e, Refusal::Session(SReason::Closed | SReason::AlreadyBound)), "{r:?}: {e:?}");
    }
    // the tail: halt, observe halt, clear BME, verify, in that order
    // out of order: the machine's tail wants the halt first
    refuses(&mut h, Req::Op(Operation::ClearBusMaster), Refusal::Machine(Reason::AfterTerminal));
    let before = h.fake().performed.len();
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    assert_eq!(h.s.machine().tail(), "await-halt");
    ok(&mut h, Req::Status);
    assert_eq!(h.s.machine().tail(), "need-bme-clear");
    ok(&mut h, Req::Op(Operation::ClearBusMaster));
    ok(&mut h, Req::Pci);
    assert_eq!(h.s.machine().tail(), "done");
    assert_eq!(h.fake().performed.len(), before + 2);
    assert!(!h.fake().d.bme && !h.fake().d.rs);
    // nothing further, and the backend independently refuses non-tail writes
    refuses(&mut h, Req::Op(Operation::ClearBusMaster), Refusal::Machine(Reason::AfterTerminal));
    assert_eq!(h.fake().refused_writes, 0);
}

#[test]
fn a_fault_inside_the_tail_ends_the_tail() {
    let mut h = running();
    inj(&mut h, 1, Inj::SeqDup);
    closes(&mut h, Req::Status);
    ok(&mut h, Req::Op(Operation::ClearRunStop));
    inj(&mut h, 1, Inj::Fault(ReadFault::AllOnes));
    assert_eq!(closes(&mut h, Req::Status), Cause::ReadAllOnes);
    assert_eq!(h.s.closed(), Some(Cause::SequenceDuplicate), "the first cause is kept");
    refuses(&mut h, Req::Status, Refusal::Session(SReason::Closed));
    refuses(&mut h, Req::Op(Operation::ClearBusMaster), Refusal::Session(SReason::Closed));
}

#[test]
fn the_backend_refuses_writes_after_the_terminal_latch_and_forged_tickets() {
    let mut f = Fake::new(Quirks::qemu());
    let t = ticket_probe(); // a real ticket: nonce 1, issued for N-01
    let other = ModelEvent { id: "N-06", target: Target::Config, width: 32, value: 1 };
    // a ticket issued for another write does not authorize this one
    assert_eq!(f.perform(&other, t).unwrap_err().kind, WriteFault::BadTicket);
    let own = ModelEvent { id: "N-01", target: Target::RunStop, width: 32, value: 0 };
    f.perform(&own, t).unwrap();
    // replay of a used ticket
    assert_eq!(f.perform(&own, t).unwrap_err().kind, WriteFault::BadTicket);
    // after the terminal latch only the tail writes N-16 and N-17 are possible, each once, and never N-01
    f.latch_terminal();
    assert_eq!(f.perform(&own, t).unwrap_err().kind, WriteFault::Refused);
    assert_eq!(f.refused_writes, 3);
    assert_eq!(f.performed.len(), 1);
}

fn ticket_probe() -> WriteTicket {
    // The tests need a WriteTicket value; the only constructor is crate-private, so capture one through a
    // recording backend that keeps the ticket it was handed.
    use std::cell::RefCell;
    thread_local! { static TICKET: RefCell<Option<WriteTicket>> = const { RefCell::new(None) }; }
    struct Grab(Fake);
    impl Backend for Grab {
        fn read(&mut self, r: Reg, i: u8) -> Result<RegRead, ReadErr> { self.0.read(r, i) }
        fn read_identity(&mut self) -> ReadResult<Identity> { self.0.read_identity() }
        fn read_topology(&mut self) -> ReadResult<Topology> { self.0.read_topology() }
        fn read_platform(&mut self) -> ReadResult<PlatformFacts> { self.0.read_platform() }
        fn read_event(&mut self, i: u16, c: bool) -> ReadResult<Option<EventTrb>> { self.0.read_event(i, c) }
        fn perform(&mut self, ev: &ModelEvent, t: WriteTicket) -> Result<Receipt, WriteErr> {
            TICKET.with(|x| *x.borrow_mut() = Some(t));
            self.0.perform(ev, t)
        }
        fn latch_terminal(&mut self) { self.0.latch_terminal() }
        fn now(&self) -> u64 { self.0.now() }
    }
    let mut q = Quirks::qemu();
    q.firmware_running = true;
    let mut s = Session::new(Grab(Fake::new(q)), Machine::new(ReleasePolicy::AfterQuiesce), Config { crcr_reads_zero: false });
    s.request(Req::Bind).unwrap();
    s.request(Req::Qualify).unwrap();
    s.request(Req::Op(Operation::ClearRunStop)).unwrap();
    TICKET.with(|x| x.borrow().unwrap())
}

// ---- model-only DMA ledger ---------------------------------------------------------------------------------------------------------

fn spec(scratch: u16) -> PoolSpec {
    PoolSpec { device_limit: 1 << 30, ac64: false, scratchpads: scratch, quarantine_cap: 4, release: ReleasePolicy::AfterQuiesce }
}
fn d<T>(r: Result<T, DReason>) -> Result<T, DReason> {
    rec_d(r)
}

fn prepared_ledger(scratch: u16) -> Ledger {
    let mut l = Ledger::new(spec(scratch)).unwrap();
    let mut a = 0x40_0000u64;
    let mut next = || {
        a += 0x1000;
        a
    };
    for k in [PageKind::Command, PageKind::Event, PageKind::Dcbaa, PageKind::Erst] {
        l.allocate(Item::Core(k), next()).unwrap();
        l.zero(Item::Core(k)).unwrap();
    }
    for b in 0..scratch {
        l.allocate(Item::Buffer(b as u8), next()).unwrap();
        l.zero(Item::Buffer(b as u8)).unwrap();
        l.prepare(Item::Buffer(b as u8)).unwrap();
    }
    if scratch > 0 {
        l.allocate(Item::Array, next()).unwrap();
        l.zero(Item::Array).unwrap();
        l.prepare(Item::Array).unwrap();
    }
    l.prepare(Item::Core(PageKind::Event)).unwrap();
    l.prepare(Item::Core(PageKind::Command)).unwrap();
    l.prepare(Item::Core(PageKind::Erst)).unwrap();
    l.prepare(Item::Core(PageKind::Dcbaa)).unwrap();
    l
}

fn session_at_programmed() -> SH {
    let mut h = SH::qemu();
    h.goto(Phase::Programmed);
    h
}

#[test]
fn the_scratchpad_policy_is_provisional_at_31_and_documented() {
    assert_eq!(PROVISIONAL_MAX_SCRATCHPADS, 31);
    assert_eq!(d(Ledger::new(spec(32))).unwrap_err(), DReason::ScratchpadsExceedPolicy);
    assert_eq!(d(Ledger::new(spec(1023))).unwrap_err(), DReason::ScratchpadsExceedPolicy);
    assert_eq!(Ledger::new(spec(31)).unwrap().planned_pages(), 36);
    assert_eq!(Ledger::new(spec(0)).unwrap().planned_pages(), 4);
    assert_eq!(d(Ledger::new(PoolSpec { device_limit: 0x10_0000, ..spec(0) })).unwrap_err(), DReason::BadLimit);
    let src = include_str!("../../kernel/rust/usb/xhci_init/dma_model.rs");
    assert!(src.contains("248 bytes") && src.contains("1,023") && src.contains("PROVISIONAL"), "the discrepancy is documented next to the policy");
    assert!(include_str!("../../docs/results/usb2c42a-hosted-backend-contract.md").contains("Table 6-1"), "and in the report");
}

#[test]
fn ledger_address_and_lifetime_rules_are_named() {
    let mut l = Ledger::new(spec(2)).unwrap();
    let c = Item::Core(PageKind::Command);
    assert_eq!(d(l.allocate(Item::Buffer(2), 0x40_0000)).unwrap_err(), DReason::NoSuchItem);
    assert_eq!(d(Ledger::new(spec(0)).unwrap().allocate(Item::Array, 0x40_0000)).unwrap_err(), DReason::NoSuchItem);
    assert_eq!(d(l.allocate(c, 0x40_0800)).unwrap_err(), DReason::Misaligned);
    assert_eq!(d(l.allocate(c, 0x8000)).unwrap_err(), DReason::AddressRange);
    assert_eq!(d(l.allocate(c, 0)).unwrap_err(), DReason::AddressRange);
    assert_eq!(d(l.allocate(c, (1 << 30) - 0x1000 + 0x1000)).unwrap_err(), DReason::AddressRange);
    assert_eq!(d(l.allocate(c, u64::MAX - 0xfff)).unwrap_err(), DReason::AddressRange);
    l.allocate(c, 0x40_0000).unwrap();
    assert_eq!(d(l.allocate(c, 0x41_0000)).unwrap_err(), DReason::AlreadyOwned);
    assert_eq!(d(l.allocate(Item::Core(PageKind::Event), 0x40_0000)).unwrap_err(), DReason::Overlap);
    assert_eq!(d(l.prepare(c)).unwrap_err(), DReason::BadState);
    assert_eq!(d(l.zero(Item::Core(PageKind::Event))).unwrap_err(), DReason::NotOwned);
    l.zero(c).unwrap();
    assert_eq!(d(l.zero(c)).unwrap_err(), DReason::BadState);
    l.prepare(c).unwrap();
    l.release(c).unwrap();
    assert_eq!(d(l.release(c)).unwrap_err(), DReason::NotOwned, "double release");
    assert_eq!(d(l.zero(c)).unwrap_err(), DReason::NotOwned, "use after release");
    // 32-bit controller: pages must end below 4 GiB
    let mut hi = Ledger::new(PoolSpec { device_limit: 1 << 40, ..spec(0) }).unwrap();
    assert_eq!(d(hi.allocate(c, (1 << 32) - 0x1000 + 0x1000)).unwrap_err(), DReason::AddressRange);
    let mut hi64 = Ledger::new(PoolSpec { device_limit: 1 << 40, ac64: true, ..spec(0) }).unwrap();
    hi64.allocate(c, 1 << 32).unwrap();
}

#[test]
fn ledger_dependencies_and_publication_proofs() {
    let mut l = prepared_ledger(0);
    // proofs come only from a session whose machine holds the page published
    let h = session_at_programmed();
    let pc = h.s.publish_proof(PageKind::Command).expect("published in the machine");
    assert!(h.s.publish_proof(PageKind::Erst).is_none(), "the ERST is not published until ERSTBA");
    assert!(h.s.quiescence_proof().is_none());
    assert_eq!(d(l.publish(PageKind::Dcbaa, &pc)).unwrap_err(), DReason::BadState, "the DCBAA publishes through publish_dcbaa");
    l.publish(PageKind::Command, &pc).unwrap();
    assert_eq!(d(l.publish(PageKind::Command, &pc)).unwrap_err(), DReason::BadState);
    // dependency order
    let mut l2 = Ledger::new(spec(0)).unwrap();
    l2.allocate(Item::Core(PageKind::Erst), 0x40_1000).unwrap();
    l2.zero(Item::Core(PageKind::Erst)).unwrap();
    assert_eq!(d(l2.prepare(Item::Core(PageKind::Erst))).unwrap_err(), DReason::DependencyNotReady, "the ERST names the event ring");
}

#[test]
fn the_dcbaa_waits_for_the_scratchpad_array() {
    let mut l = Ledger::new(spec(2)).unwrap();
    let mut a = 0x70_0000u64;
    let mut nx = || {
        a += 0x1000;
        a
    };
    for b in 0..2u8 {
        l.allocate(Item::Buffer(b), nx()).unwrap();
        l.zero(Item::Buffer(b)).unwrap();
        l.prepare(Item::Buffer(b)).unwrap();
    }
    l.allocate(Item::Array, nx()).unwrap();
    l.zero(Item::Array).unwrap();
    l.allocate(Item::Core(PageKind::Dcbaa), nx()).unwrap();
    l.zero(Item::Core(PageKind::Dcbaa)).unwrap();
    assert_eq!(d(l.prepare(Item::Core(PageKind::Dcbaa))).unwrap_err(), DReason::DependencyNotReady, "entry 0 would point at an unprepared array");
    l.prepare(Item::Array).unwrap();
    l.prepare(Item::Core(PageKind::Dcbaa)).unwrap();
}

#[test]
fn scratchpad_pages_become_device_owned_and_are_never_touched_or_freed_until_quiescence() {
    for policy in [ReleasePolicy::AfterQuiesce, ReleasePolicy::Never] {
        let mut l = Ledger::new(PoolSpec { release: policy, ..spec(4) }).unwrap();
        let mut a = 0x50_0000u64;
        let mut nx = || {
            a += 0x1000;
            a
        };
        for b in 0..4u8 {
            l.allocate(Item::Buffer(b), nx()).unwrap();
            l.zero(Item::Buffer(b)).unwrap();
        }
        l.allocate(Item::Array, nx()).unwrap();
        l.zero(Item::Array).unwrap();
        assert_eq!(d(l.prepare(Item::Array)).unwrap_err(), DReason::DependencyNotReady, "buffers must exist before the array names them");
        for b in 0..4u8 {
            l.prepare(Item::Buffer(b)).unwrap();
        }
        l.prepare(Item::Array).unwrap();
        for k in [PageKind::Event, PageKind::Command, PageKind::Erst, PageKind::Dcbaa] {
            l.allocate(Item::Core(k), nx()).unwrap();
            l.zero(Item::Core(k)).unwrap();
        }
        l.prepare(Item::Core(PageKind::Event)).unwrap();
        assert_eq!(d(l.prepare(Item::Core(PageKind::Dcbaa))).unwrap(), (), "the array is ready");
        l.prepare(Item::Core(PageKind::Command)).unwrap();
        l.prepare(Item::Core(PageKind::Erst)).unwrap();
        // software may fill a prepared buffer; once published it may not touch it
        l.cpu_access(Item::Buffer(0)).unwrap();
        let h = session_at_programmed();
        let pd = h.s.publish_proof(PageKind::Dcbaa).unwrap();
        l.publish_dcbaa(&pd).unwrap();
        assert_eq!(l.state(Item::Buffer(0)), Some(DState::DeviceOwned));
        assert_eq!(l.state(Item::Array), Some(DState::DeviceOwned));
        assert_eq!(d(l.cpu_access(Item::Buffer(0))).unwrap_err(), DReason::CpuTouchOfDeviceOwned);
        assert_eq!(d(l.cpu_access(Item::Array)).unwrap_err(), DReason::CpuTouchOfDeviceOwned);
        l.cpu_access(Item::Core(PageKind::Command)).unwrap_or(());
        assert_eq!(d(l.release(Item::Buffer(1))).unwrap_err(), DReason::StillPublished);
        assert_eq!(d(l.release(Item::Core(PageKind::Dcbaa))).unwrap_err(), DReason::StillPublished);
        assert_eq!(d(l.publish_dcbaa(&pd)).unwrap_err(), DReason::BadState);
        // quiescence needs the machine to have reached Quiesced
        assert!(h.s.quiescence_proof().is_none());
        let mut done = SH::qemu();
        done.run(1, 50_000);
        let q = done.s.quiescence_proof().expect("quiesced session");
        l.quiesce(&q).unwrap();
        assert_eq!(d(l.quiesce(&q)).unwrap_err(), DReason::NothingToQuiesce);
        assert_eq!(d(l.cpu_access(Item::Buffer(0))).unwrap_err(), DReason::BadState);
        match policy {
            ReleasePolicy::AfterQuiesce => {
                l.release(Item::Buffer(0)).unwrap();
                assert_eq!(l.state(Item::Buffer(0)), Some(DState::Free));
            }
            ReleasePolicy::Never => assert_eq!(d(l.release(Item::Buffer(0))).unwrap_err(), DReason::ReleaseForbiddenByPolicy),
        }
    }
}

#[test]
fn an_incomplete_scratchpad_set_cannot_be_published() {
    let mut l = prepared_ledger(3);
    let h = session_at_programmed();
    let pd = h.s.publish_proof(PageKind::Dcbaa).unwrap();
    // revert one buffer to an earlier state by releasing and re-allocating it only to Zeroed
    l.release(Item::Buffer(1)).unwrap();
    l.allocate(Item::Buffer(1), 0x60_0000).unwrap();
    l.zero(Item::Buffer(1)).unwrap();
    assert_eq!(d(l.publish_dcbaa(&pd)).unwrap_err(), DReason::ScratchpadIncomplete);
    assert_eq!(l.state(Item::Buffer(0)), Some(DState::Prepared), "nothing was published");
}

#[test]
fn quarantine_is_bounded_and_exhaustion_closes_the_pool() {
    let mut l = Ledger::new(PoolSpec { quarantine_cap: 2, ..spec(0) }).unwrap();
    let mut a = 0x40_0000u64;
    for k in [PageKind::Command, PageKind::Event, PageKind::Dcbaa] {
        a += 0x1000;
        l.allocate(Item::Core(k), a).unwrap();
    }
    l.quarantine(Item::Core(PageKind::Command)).unwrap();
    l.quarantine(Item::Core(PageKind::Event)).unwrap();
    assert_eq!(d(l.quarantine(Item::Core(PageKind::Dcbaa))).unwrap_err(), DReason::QuarantineExhausted);
    assert!(l.closed());
    assert_eq!(l.state(Item::Core(PageKind::Dcbaa)), Some(DState::Retained));
    assert_eq!(d(l.allocate(Item::Core(PageKind::Erst), 0x50_0000)).unwrap_err(), DReason::PoolClosed);
    assert_eq!(d(l.release(Item::Core(PageKind::Command))).unwrap_err(), DReason::Quarantined);
    assert_eq!(d(l.quarantine(Item::Core(PageKind::Command))).unwrap_err(), DReason::Quarantined);
    assert_eq!(d(l.cpu_access(Item::Core(PageKind::Command))).unwrap_err(), DReason::Quarantined);
    assert_eq!(d(l.zero(Item::Core(PageKind::Command))).unwrap_err(), DReason::Quarantined);
    assert_eq!(d(l.quarantine(Item::Core(PageKind::Erst))).unwrap_err(), DReason::NotOwned);
}

#[test]
fn the_ledger_agrees_with_the_machine_through_a_whole_session() {
    let mut h = SH::qemu();
    let mut l = Ledger::new(spec(0)).unwrap();
    let mut mirrored = [false; 4];
    for _ in 0..4000 {
        let Some(r) = h.next() else { break };
        let _ = h.req(r);
        for (i, k) in ALL_PAGES.iter().enumerate() {
            let ms = h.s.machine().page(*k);
            let it = Item::Core(*k);
            // mirror allocation and publication from the machine's page states
            if ms.state != PageState::Free && l.state(it) == Some(DState::Free) {
                l.allocate(it, ms.addr).unwrap();
            }
            if ms.state == PageState::Published && !mirrored[i] {
                l.zero(it).unwrap();
                l.prepare(it).unwrap();
                let p = h.s.publish_proof(*k).unwrap();
                if *k == PageKind::Dcbaa { l.publish_dcbaa(&p).unwrap() } else { l.publish(*k, &p).unwrap() }
                mirrored[i] = true;
            }
        }
        if h.s.machine().phase() == Phase::Programmed {
            break;
        }
    }
    // the machine published three pages by the end of programming; the ledger mirrors exactly those
    for k in [PageKind::Command, PageKind::Event, PageKind::Dcbaa] {
        assert!(matches!(l.state(Item::Core(k)), Some(DState::Published)), "{k:?}");
    }
}

// ---- randomized runs with injected faults ---------------------------------------------------------------------------------------

fn kinds_required(op: &Operation, phase: Phase) -> Option<&'static [usize]> {
    use Operation as O;
    Some(match op {
        O::RequestOsOwnership | O::DisableLegacySmi => &[2],
        O::ClearRunStop | O::RingCommandDoorbell => &[0],
        O::ClearBusMaster | O::AssertReset | O::SetBusMaster | O::SetErstBase(_) | O::SetRunStop => &[0, 1],
        O::SetMaxSlots(_) | O::SetDcbaap(_) | O::SetCommandRing { .. } | O::SetErstSize(_) => &[0],
        O::SetEventDequeue { .. } => {
            if phase == Phase::Running { &[4] } else { &[0] }
        }
        _ => return None,
    })
}
fn kinds_invalidated(op: &Operation) -> &'static [usize] {
    use Operation as O;
    match op {
        O::RequestOsOwnership | O::DisableLegacySmi => &[2],
        O::ClearRunStop | O::SetRunStop | O::SetErstBase(_) => &[0],
        O::ClearBusMaster | O::SetBusMaster => &[1],
        O::AssertReset => &[0, 3, 4],
        O::RingCommandDoorbell => &[0, 4],
        O::SetEventDequeue { .. } => &[4],
        _ => &[],
    }
}

fn walk(seed: u64, steps: usize, faults: bool) -> SH {
    let mut r = Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
    let mut q = random_quirks(&mut r);
    if !faults {
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
    }
    let mut h = SH::new(q, if r.chance(1, 5) { ReleasePolicy::Never } else { ReleasePolicy::AfterQuiesce });
    h.target = 1 + r.below(120) as u32;
    let mut terminal_writes = 0usize;
    let mut terminal_seen = false;
    for _ in 0..steps {
        let req = if r.chance(2, 3) { h.next().unwrap_or(Req::Status) } else { random_req(&mut r, &h) };
        if faults && r.chance(1, 40) {
            let (at, i) = random_inj(&mut r, h.fake().calls);
            h.fake().at(at, i);
        }
        let phase0 = h.s.machine().phase();
        let sealed0 = h.s.closed().is_some() || phase0.terminal();
        let fresh0: Vec<bool> = (0..5).map(|k| h.s.fresh(k)).collect();
        let performed0 = h.fake().performed.len();
        let res = h.req(req);
        // freshness contract, checked against an independent table
        if let Req::Op(op) = &req
            && h.s.is_bound()
            && phase0 != Phase::Idle
        {
            if let Some(ks) = kinds_required(op, phase0) {
                // sealed (tail) mode: only the status read is required, and only for the bus-master clear
                let ks: &[usize] = if sealed0 { if matches!(op, Operation::ClearBusMaster) { &[0] } else { &[] } } else { ks };
                let stale = ks.iter().any(|k| !fresh0[*k]);
                if stale {
                    // sealed with a dead tail answers Closed first; otherwise staleness is decided before the machine
                    let sealed_closed = sealed0 && res == Err(Refusal::Session(SReason::Closed));
                    assert!(sealed_closed || res == Err(Refusal::Session(SReason::StaleObservation)), "seed {seed}: {req:?}: {res:?}");
                } else {
                    assert_ne!(res, Err(Refusal::Session(SReason::StaleObservation)), "seed {seed}: {req:?}");
                }
                if matches!(&res, Ok(d) if d.closed.is_none() && d.accepted.is_some()) && !sealed0 {
                    for k in kinds_invalidated(op) {
                        assert!(!h.s.fresh(*k), "seed {seed}: {req:?} left observation {k} valid");
                    }
                }
            }
        }
        // terminal discipline: no machine transition after terminal, writes only from the tail
        if sealed0 && phase0.terminal() {
            assert_eq!(h.s.machine().phase(), phase0, "seed {seed}: phase changed after terminal by {req:?}");
        }
        if sealed0 {
            terminal_seen = true;
            let new_writes: Vec<&str> = h.fake().performed[performed0..].iter().map(|e| e.id).collect();
            terminal_writes += new_writes.len();
            for id in &new_writes {
                assert!(matches!(*id, "N-16" | "N-17"), "seed {seed}: write {id} after the session ended");
            }
        }
        if let Ok(d) = &res
            && let Some(c) = d.closed
        {
            assert!(h.s.closed().is_some() || sealed0, "seed {seed}: closed {c:?} but not recorded");
        }
    }
    assert!(terminal_writes <= 2 || !terminal_seen, "seed {seed}: more tail writes than the tail has");
    assert_eq!(h.fake().refused_writes, 0, "seed {seed}: the session attempted a write the backend had to refuse");
    if !faults {
        assert!(h.fake().d.violations.is_empty(), "seed {seed}: {:?}", h.fake().d.violations);
        assert!(h.s.closed().is_none(), "seed {seed}: a truthful backend closed the session: {:?}", h.s.closed());
    }
    h
}

const SEEDS: u64 = 700;
const STEPS: usize = 220;

#[test]
fn randomized_requests_against_a_truthful_backend_keep_every_invariant_and_never_violate_the_device() {
    let mut finished = 0;
    for seed in 0..SEEDS {
        let h = walk(seed, STEPS, false);
        finished += (h.s.machine().phase() == Phase::Quiesced) as u32;
    }
    assert!(finished > 60, "complete sessions: {finished}");
}

#[test]
fn randomized_requests_with_injected_backend_faults_stay_fail_closed() {
    let mut closed = 0;
    for seed in 0..SEEDS {
        let h = walk(seed + 90_000, STEPS, true);
        closed += h.s.closed().is_some() as u32;
    }
    assert!(closed > 100, "the injections must actually close sessions: {closed}");
}

#[test]
fn export_session_traces_for_the_oracle() {
    let Ok(path) = std::env::var("XHCI_BACKEND_TRACE") else { return };
    use std::io::Write;
    let n: u64 = std::env::var("XHCI_BACKEND_SEEDS").ok().and_then(|s| s.parse().ok()).unwrap_or(300);
    let mut out = std::io::BufWriter::new(std::fs::File::create(path).unwrap());
    for seed in 0..n {
        let faults = seed % 2 == 1;
        let h = walk(200_000 + seed, STEPS, faults);
        let q = &h.s.backend().q;
        writeln!(out, "T {} {} {} {} {} {}", 200_000 + seed, if faults { "faults" } else { "truthful" }, h.s.machine().policy().name(), u8::from(q.legacy), q.ports, 0).unwrap();
        for e in &h.log {
            writeln!(out, "Q {}", req_text(&e.req)).unwrap();
            for l in &e.io {
                writeln!(out, "  {l}").unwrap();
            }
            writeln!(out, "=> {}", done_text(&e.res)).unwrap();
        }
    }
}

// ---- completeness ----------------------------------------------------------------------------------------------------------------------

#[test]
fn zz_every_cause_session_refusal_and_ledger_reason_is_exercised() {
    for seed in 0..SEEDS {
        let _ = walk(seed + 90_000, STEPS, true);
    }
    let g = COV2.lock().unwrap();
    let [causes, sreasons, dreasons] = g.as_ref().unwrap();
    let missing: Vec<_> = Cause::ALL.iter().map(|c| c.name()).filter(|n| !causes.contains(n)).collect();
    assert!(missing.is_empty(), "causes never produced: {missing:?}");
    let missing: Vec<_> = SReason::ALL.iter().map(|c| c.name()).filter(|n| !sreasons.contains(n)).collect();
    assert!(missing.is_empty(), "session refusals never produced: {missing:?}");
    let missing: Vec<_> = DReason::ALL.iter().map(|c| c.name()).filter(|n| !dreasons.contains(n)).collect();
    assert!(missing.is_empty(), "ledger reasons never produced: {missing:?}");
}
