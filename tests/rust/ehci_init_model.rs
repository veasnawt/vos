//! USB.3-A hosted tests for the EHCI qualification and handoff model. Everything here is host data: no PCI, MMIO,
//! port, DMA or QEMU access. A write is a value (`Event`); the tests only compare values and never perform them.
//!
//! Covered: both Dell fixtures and their provenance, qualification versus per-step readiness, the full abstract
//! sequence on each Dell controller, firmware non-release and revocation, SMI-management refusal, schedule/halt/reset
//! timeouts, unverified reset, host-system-error and read failures, missing / inferred / contradictory / malformed
//! facts, stale observations, ordering, terminal phases, the no-legacy and already-owned paths, and a seeded safety
//! property checked against an independent tracker (not the machine's own state). The same seeded traces are exported
//! for the independent Python oracle when `EHCI_TRACE_OUT` names a file.
#![forbid(unsafe_code)]
use std::collections::HashSet;
use std::sync::Mutex;
use vos_ehci_init::dell::{self, Note};
use vos_ehci_init::*;

static COVERAGE: Mutex<Option<(HashSet<&'static str>, HashSet<&'static str>)>> = Mutex::new(None);

fn record(r: &Result<Accepted, Reason>) {
    let mut g = COVERAGE.lock().unwrap();
    let (rules, reasons) = g.get_or_insert_with(Default::default);
    match r {
        Ok(a) => {
            rules.insert(a.why.name());
        }
        Err(e) => {
            reasons.insert(e.name());
        }
    }
}

fn step(m: &mut Machine, op: Operation) -> Result<Accepted, Reason> {
    let r = m.apply(op);
    record(&r);
    r
}

fn ok(m: &mut Machine, op: Operation) -> Accepted {
    step(m, op).unwrap_or_else(|e| panic!("{op:?} refused: {}", e.name()))
}

fn refused(m: &mut Machine, op: Operation) -> Reason {
    let before = m.clone();
    let e = step(m, op).expect_err("the operation must be refused");
    assert_eq!(*m, before, "a refusal must leave the machine untouched: {op:?}");
    e
}

fn dell() -> EhciFacts {
    dell::ehci_8c2d().facts
}

fn no_legacy() -> EhciFacts {
    EhciFacts { legacy: Fact::Observed(None), legacy_bios_owned: Fact::Unknown, legacy_os_owned: Fact::Unknown, legacy_ctl: Fact::Unknown, ..dell() }
}

const CMD0: u32 = 0x0000_0011; // run + periodic schedule enable, as surveyed
const STS_RUNNING: u32 = 0x0000_600a; // periodic + async-reclaim status, as surveyed on 8c2d
const STS_QUIET: u32 = 0x0000_2008; // schedules inactive, controller still running
const STS_HALT: u32 = 0x0000_3008; // schedules inactive, halted

fn obs(m: &mut Machine, o: Obs) -> Accepted {
    ok(m, Operation::Observe(o))
}
fn req(m: &mut Machine, s: Step) -> Accepted {
    ok(m, Operation::Request(s))
}

/// Drive a fresh machine on `facts` to `to` with the observations a cooperative controller would give.
fn at(facts: EhciFacts, to: Phase) -> Machine {
    let mut m = Machine::new();
    ok(&mut m, Operation::Qualify(facts));
    let legacy = facts.legacy == Fact::Observed(Some(0x68));
    if to == Phase::Qualified {
        return m;
    }
    if legacy {
        obs(&mut m, Obs::LegacySup { bios: true, os: false });
        req(&mut m, Step::Handoff);
        if to == Phase::Handoff {
            return m;
        }
        obs(&mut m, Obs::LegacySup { bios: false, os: true });
        ok(&mut m, Operation::Advance(1));
        obs(&mut m, Obs::LegacySup { bios: false, os: true });
    } else {
        req(&mut m, Step::Handoff);
    }
    assert_eq!(m.phase(), Phase::HandedOff);
    if to == Phase::HandedOff {
        return m;
    }
    if legacy {
        obs(&mut m, Obs::LegacyCtl(0x2005));
    }
    req(&mut m, Step::MaskSmi);
    if legacy {
        obs(&mut m, Obs::LegacyCtl(0));
    }
    assert_eq!(m.phase(), Phase::SmiMasked);
    if to == Phase::SmiMasked {
        return m;
    }
    obs(&mut m, Obs::Caplength(0x20));
    obs(&mut m, Obs::Usbcmd(CMD0));
    obs(&mut m, Obs::Usbsts(STS_RUNNING));
    req(&mut m, Step::StopSchedules);
    obs(&mut m, Obs::Usbsts(STS_QUIET));
    assert_eq!(m.phase(), Phase::Quiet);
    if to == Phase::Quiet {
        return m;
    }
    obs(&mut m, Obs::Usbcmd(0x0000_0001));
    obs(&mut m, Obs::Usbsts(STS_QUIET));
    req(&mut m, Step::Halt);
    obs(&mut m, Obs::Usbsts(STS_HALT));
    assert_eq!(m.phase(), Phase::Halted);
    if to == Phase::Halted {
        return m;
    }
    obs(&mut m, Obs::Usbcmd(0));
    obs(&mut m, Obs::Usbsts(STS_HALT));
    req(&mut m, Step::Reset);
    assert_eq!(m.phase(), to);
    m
}

#[test]
fn the_two_dell_fixtures_are_distinct_and_carry_provenance() {
    let [a, b] = dell::both();
    assert_eq!((a.name, a.location, a.facts.device), ("intel-ehci-8c2d", (0, 0x1a, 0), Fact::Observed(0x8c2d)));
    assert_eq!((b.name, b.location, b.facts.device), ("intel-ehci-8c26", (0, 0x1d, 0), Fact::Observed(0x8c26)));
    assert_ne!(a.facts, b.facts);
    assert_eq!((a.facts.usbsts, b.facts.usbsts), (Fact::Observed(0x600a), Fact::Observed(0x6008)));
    assert_eq!((a.facts.legacy_ctl, b.facts.legacy_ctl), (Fact::Observed(0x000a_2005), Fact::Observed(0x0000_2005)));
    for f in [a, b] {
        let x = f.facts;
        assert_eq!((x.vendor, x.class_code, x.hci_version), (Fact::Observed(0x8086), Fact::Observed(0x000c_0320), Fact::Observed(0x0100)));
        assert_eq!((x.n_ports, x.n_companions, x.ports_per_companion), (Fact::Observed(2), Fact::Observed(0), Fact::Observed(0)));
        assert_eq!((x.legacy, x.legacy_bios_owned, x.legacy_os_owned), (Fact::Observed(Some(0x68)), Fact::Observed(true), Fact::Observed(false)));
        assert_eq!((x.usbcmd, x.configflag), (Fact::Observed(0x11), Fact::Observed(true)));
        // not printed by the survey: left unknown, never invented
        assert_eq!(x.caplength, Fact::Unknown);
        assert_eq!(x.firmware_active, Fact::Inferred(true), "an inference stays an inference");
        let note = |field: &str| f.provenance.iter().find(|p| p.field == field).unwrap().note;
        assert_eq!(note("caplength"), Note::Unobserved);
        assert_eq!(note("legacy-control"), Note::ReadOnce, "the SMI control values were read from the photograph once");
        assert_eq!(note("firmware-active"), Note::Inferred);
        assert_eq!(note("root-port-1-device-identity"), Note::Unobserved, "the survey does not identify what is on the root port");
        assert!(f.provenance.iter().all(|p| !p.survey_line.is_empty()));
    }
    assert_eq!(dell::SOURCE_COMMIT.len(), 40);
    assert_eq!(dell::PHOTO_SHA256.len(), 64);
    assert_eq!(dell::IMAGE_SHA256.len(), 64);
}

#[test]
fn qualification_and_readiness_are_separate_and_unknown_facts_block_only_their_steps() {
    let f = dell();
    assert_eq!(qualify(&f), Ok(Qualification { ports: 2 }));
    assert_eq!(readiness(&f, Step::Handoff), Readiness::Ready);
    assert_eq!(readiness(&f, Step::MaskSmi), Readiness::Ready);
    // CAPLENGTH was not surveyed: every operational-register step is blocked, the legacy steps are not
    for s in [Step::StopSchedules, Step::Halt, Step::Reset] {
        assert_eq!(readiness(&f, s), Readiness::Blocked(Reason::MissingFact(Field::Caplength)), "{s:?}");
    }
    let with_cap = EhciFacts { caplength: Fact::Observed(0x20), ..f };
    for s in [Step::StopSchedules, Step::Halt, Step::Reset, Step::Handoff, Step::MaskSmi] {
        assert_eq!(readiness(&with_cap, s), Readiness::Ready, "{s:?}");
    }
    // an unknown legacy-control value blocks only the SMI step, an unknown USBSTS only the operational steps
    let g = EhciFacts { legacy_ctl: Fact::Unknown, ..with_cap };
    assert_eq!(readiness(&g, Step::MaskSmi), Readiness::Blocked(Reason::MissingFact(Field::LegacyCtl)));
    assert_eq!(readiness(&g, Step::Handoff), Readiness::Ready);
    assert_eq!(readiness(&g, Step::Halt), Readiness::Ready);
    let h = EhciFacts { usbsts: Fact::Unknown, ..with_cap };
    assert_eq!(readiness(&h, Step::Halt), Readiness::Blocked(Reason::MissingFact(Field::Usbsts)));
    assert_eq!(readiness(&h, Step::Handoff), Readiness::Ready);
    assert_eq!(readiness(&h, Step::MaskSmi), Readiness::Ready);
    // unknown unrelated fields do not stop qualification
    let q = EhciFacts { usbcmd: Fact::Unknown, legacy_ctl: Fact::Unknown, caplength: Fact::Unknown, n_companions: Fact::Unknown, configflag: Fact::Unknown, ..f };
    assert_eq!(qualify(&q), Ok(Qualification { ports: 2 }));
    // companions unknown blocks only reset
    assert_eq!(readiness(&EhciFacts { n_companions: Fact::Unknown, ..with_cap }, Step::Reset), Readiness::Blocked(Reason::MissingFact(Field::Companions)));
    assert_eq!(readiness(&EhciFacts { n_companions: Fact::Unknown, ..with_cap }, Step::Halt), Readiness::Ready);
    assert_eq!(readiness(&EhciFacts { n_companions: Fact::Observed(2), ports_per_companion: Fact::Observed(3), ..with_cap }, Step::Reset), Readiness::Blocked(Reason::CompanionsPresent));
}

#[test]
fn the_full_abstract_sequence_on_each_dell_controller() {
    for fx in dell::both() {
        let f = fx.facts;
        let mut m = Machine::new();
        assert_eq!(ok(&mut m, Operation::Qualify(f)).to, Phase::Qualified);
        // static survey facts alone never produce a write
        assert_eq!(refused(&mut m, Operation::Request(Step::Handoff)), Reason::StaleObservation(Field::LegacyBios));
        obs(&mut m, Obs::LegacySup { bios: true, os: false });
        let a = req(&mut m, Step::Handoff);
        assert_eq!((a.to, a.why, a.event), (Phase::Handoff, Rule::HandoffRequest, Some(Event::LegacyOsSemaphore { cap: 0x68 })));
        assert_eq!(m.deadline(), Some(HANDOFF_TICKS));
        assert_eq!(obs(&mut m, Obs::LegacySup { bios: true, os: true }).why, Rule::HandoffPending);
        ok(&mut m, Operation::Advance(10));
        assert_eq!(obs(&mut m, Obs::LegacySup { bios: false, os: true }).why, Rule::HandoffComplete);
        assert_eq!(obs(&mut m, Obs::LegacySup { bios: false, os: true }).why, Rule::HandoffComplete, "stability needs time to pass");
        ok(&mut m, Operation::Advance(1));
        let a = obs(&mut m, Obs::LegacySup { bios: false, os: true });
        assert_eq!((a.to, a.why), (Phase::HandedOff, Rule::HandoffStable));
        assert_eq!(m.deadline(), None);
        // SMI management
        assert_eq!(refused(&mut m, Operation::Request(Step::MaskSmi)), Reason::StaleObservation(Field::LegacyCtl));
        let ctl = f.legacy_ctl.observed().unwrap();
        obs(&mut m, Obs::LegacyCtl(ctl));
        let a = req(&mut m, Step::MaskSmi);
        assert_eq!((a.to, a.why), (Phase::MaskingSmi, Rule::SmiRequest));
        assert_eq!(a.event, Some(Event::LegacyCtlWrite { cap: 0x68, value: 0 }), "enables clear, status bits written as 0");
        assert_eq!(obs(&mut m, Obs::LegacyCtl(0x2005)).why, Rule::SmiPending);
        assert_eq!(obs(&mut m, Obs::LegacyCtl(0)).to, Phase::SmiMasked);
        // schedules: blocked until CAPLENGTH is observed live
        assert_eq!(refused(&mut m, Operation::Request(Step::StopSchedules)), Reason::MissingFact(Field::Caplength));
        obs(&mut m, Obs::Caplength(0x20));
        assert_eq!(refused(&mut m, Operation::Request(Step::StopSchedules)), Reason::StaleObservation(Field::Usbcmd));
        obs(&mut m, Obs::Usbcmd(CMD0));
        assert_eq!(refused(&mut m, Operation::Request(Step::StopSchedules)), Reason::StaleObservation(Field::Usbsts));
        obs(&mut m, Obs::Usbsts(f.usbsts.observed().unwrap()));
        let a = req(&mut m, Step::StopSchedules);
        assert_eq!((a.to, a.why, a.event), (Phase::Stopping, Rule::SchedulesRequest, Some(Event::UsbcmdWrite { value: 0x01 })));
        assert_eq!(obs(&mut m, Obs::Usbsts(f.usbsts.observed().unwrap())).why, Rule::SchedulesPending);
        assert_eq!(obs(&mut m, Obs::Usbsts(STS_QUIET)).to, Phase::Quiet);
        // halt
        obs(&mut m, Obs::Usbcmd(0x01));
        obs(&mut m, Obs::Usbsts(STS_QUIET));
        let a = req(&mut m, Step::Halt);
        assert_eq!((a.to, a.why, a.event), (Phase::Halting, Rule::HaltRequest, Some(Event::UsbcmdWrite { value: 0 })));
        assert_eq!(obs(&mut m, Obs::Usbsts(STS_QUIET)).why, Rule::HaltPending);
        assert_eq!(obs(&mut m, Obs::Usbsts(STS_HALT)).to, Phase::Halted);
        // reset: three confirmations, in any order
        obs(&mut m, Obs::Usbcmd(0));
        obs(&mut m, Obs::Usbsts(STS_HALT));
        let a = req(&mut m, Step::Reset);
        assert_eq!((a.to, a.why, a.event), (Phase::Resetting, Rule::ResetRequest, Some(Event::UsbcmdWrite { value: 0x02 })));
        assert_eq!(obs(&mut m, Obs::Usbcmd(0x02)).why, Rule::ResetPending);
        obs(&mut m, Obs::Usbsts(0x1000));
        obs(&mut m, Obs::Configflag(false));
        let a = obs(&mut m, Obs::Usbcmd(0));
        assert_eq!((a.to, a.why), (Phase::Reset, Rule::ResetVerified));
        assert_eq!(m.writes(), 5, "semaphore, SMI control, schedules, run/stop, reset: nothing else");
        assert!(m.phase().terminal());
        assert_eq!(m.failure(), None);
    }
}

#[test]
fn firmware_that_does_not_release_ownership_is_never_overridden() {
    let mut m = at(dell(), Phase::Qualified);
    obs(&mut m, Obs::LegacySup { bios: true, os: false });
    req(&mut m, Step::Handoff);
    for _ in 0..9 {
        assert_eq!(obs(&mut m, Obs::LegacySup { bios: true, os: true }).why, Rule::HandoffPending);
        ok(&mut m, Operation::Advance(HANDOFF_TICKS / 10));
    }
    let a = ok(&mut m, Operation::Advance(HANDOFF_TICKS / 10));
    assert_eq!((a.to, a.why, a.failure), (Phase::Failed, Rule::FaultTimeout, Some(Reason::FirmwareNoRelease)));
    assert_eq!(m.writes(), 1, "no second write to force the BIOS semaphore");
    assert_eq!(refused(&mut m, Operation::Request(Step::MaskSmi)), Reason::Terminal);
    // firmware that takes the OS semaphore back
    let mut m = at(dell(), Phase::Qualified);
    obs(&mut m, Obs::LegacySup { bios: true, os: false });
    req(&mut m, Step::Handoff);
    let a = obs(&mut m, Obs::LegacySup { bios: true, os: false });
    assert_eq!((a.to, a.why, a.failure), (Phase::Failed, Rule::FaultRevoked, Some(Reason::FirmwareRevoked)));
    // a release that is taken back before it was stable restarts the stability wait
    let mut m = at(dell(), Phase::Qualified);
    obs(&mut m, Obs::LegacySup { bios: true, os: false });
    req(&mut m, Step::Handoff);
    obs(&mut m, Obs::LegacySup { bios: false, os: true });
    ok(&mut m, Operation::Advance(1));
    assert_eq!(obs(&mut m, Obs::LegacySup { bios: true, os: true }).why, Rule::HandoffPending);
    assert_eq!(obs(&mut m, Obs::LegacySup { bios: false, os: true }).why, Rule::HandoffComplete);
    assert_eq!(m.phase(), Phase::Handoff);
}

#[test]
fn smi_management_refusal_and_timeout() {
    // a controller whose SMI enables stay set (firmware rewrites them) ends in Failed, with exactly one write
    let mut m = at(dell(), Phase::HandedOff);
    obs(&mut m, Obs::LegacyCtl(0x2005));
    req(&mut m, Step::MaskSmi);
    for _ in 0..4 {
        assert_eq!(obs(&mut m, Obs::LegacyCtl(0x2005)).why, Rule::SmiPending);
        ok(&mut m, Operation::Advance(SMI_TICKS / 5));
    }
    let a = ok(&mut m, Operation::Advance(SMI_TICKS / 5));
    assert_eq!(a.failure, Some(Reason::SmiNotDisabled));
    assert_eq!(m.writes(), 2);
    // SMI management cannot be requested before ownership is established
    let mut m = at(dell(), Phase::Qualified);
    obs(&mut m, Obs::LegacyCtl(0x2005));
    assert_eq!(refused(&mut m, Operation::Request(Step::MaskSmi)), Reason::WrongPhase);
    // enables already clear: no write
    let mut m = at(dell(), Phase::HandedOff);
    obs(&mut m, Obs::LegacyCtl(0));
    let a = req(&mut m, Step::MaskSmi);
    assert_eq!((a.why, a.event, a.to), (Rule::SmiAlreadyMasked, None, Phase::SmiMasked));
    // a status-only value (write-one-to-clear bits) is not an enable
    let mut m = at(dell(), Phase::HandedOff);
    obs(&mut m, Obs::LegacyCtl(0x000a_0000));
    assert_eq!(req(&mut m, Step::MaskSmi).why, Rule::SmiAlreadyMasked);
    // the written value keeps non-enable low bits and never sets a status bit
    let mut m = at(dell(), Phase::HandedOff);
    obs(&mut m, Obs::LegacyCtl(0xffff_ffff));
    assert_eq!(req(&mut m, Step::MaskSmi).event, Some(Event::LegacyCtlWrite { cap: 0x68, value: 0x0000_1fc0 }));
}

#[test]
fn schedule_halt_and_reset_timeouts_and_failures() {
    let fail_at = |m: &mut Machine, ticks: u64| {
        for _ in 0..3 {
            ok(m, Operation::Advance(ticks / 4));
        }
        ok(m, Operation::Advance(ticks - 3 * (ticks / 4)))
    };
    let mut m = at(dell(), Phase::SmiMasked);
    obs(&mut m, Obs::Caplength(0x20));
    obs(&mut m, Obs::Usbcmd(CMD0));
    obs(&mut m, Obs::Usbsts(STS_RUNNING));
    req(&mut m, Step::StopSchedules);
    let a = fail_at(&mut m, SCHEDULE_TICKS);
    assert_eq!(a.failure, Some(Reason::ScheduleStopTimeout));

    let mut m = at(dell(), Phase::Quiet);
    obs(&mut m, Obs::Usbcmd(1));
    obs(&mut m, Obs::Usbsts(STS_QUIET));
    req(&mut m, Step::Halt);
    let a = fail_at(&mut m, HALT_TICKS);
    assert_eq!(a.failure, Some(Reason::HaltTimeout));
    assert_eq!(refused(&mut m, Operation::Request(Step::Reset)), Reason::Terminal, "a failed halt never proceeds to reset");

    let mut m = at(dell(), Phase::Resetting);
    obs(&mut m, Obs::Usbcmd(0x02));
    let a = fail_at(&mut m, RESET_TICKS);
    assert_eq!(a.failure, Some(Reason::ResetTimeout));

    // reset bit cleared and halted but CONFIGFLAG still set: the reset did not take effect
    let mut m = at(dell(), Phase::Resetting);
    obs(&mut m, Obs::Usbcmd(0));
    obs(&mut m, Obs::Usbsts(0x1000));
    let a = obs(&mut m, Obs::Configflag(true));
    assert_eq!((a.to, a.why, a.failure), (Phase::Failed, Rule::FaultResetUnverified, Some(Reason::ResetUnverified)));
    // CONFIGFLAG still set while HCRESET is still pending is just progress
    let mut m = at(dell(), Phase::Resetting);
    assert_eq!(obs(&mut m, Obs::Configflag(true)).why, Rule::ResetPending);
    assert_eq!(m.phase(), Phase::Resetting);
    // the reset is not verified without all three confirmations
    let mut m = at(dell(), Phase::Resetting);
    obs(&mut m, Obs::Usbcmd(0));
    obs(&mut m, Obs::Configflag(false));
    assert_eq!(m.phase(), Phase::Resetting);

    // a host system error or a failed read ends any active phase
    for phase in [Phase::Qualified, Phase::Handoff, Phase::SmiMasked, Phase::Halted] {
        let mut m = at(dell(), phase);
        let a = obs(&mut m, Obs::Usbsts(0x0000_0010));
        assert_eq!((a.to, a.failure), (Phase::Failed, Some(Reason::HostSystemError)), "{phase:?}");
        let mut m = at(dell(), phase);
        let a = obs(&mut m, Obs::ReadFailed);
        assert_eq!((a.to, a.failure), (Phase::Failed, Some(Reason::ReadFailed)), "{phase:?}");
    }
}

#[test]
fn halt_and_reset_refuse_unsafe_register_states() {
    // firmware re-enabled a schedule: halt must not clear Run/Stop under it
    let mut m = at(dell(), Phase::Quiet);
    obs(&mut m, Obs::Usbcmd(0x11));
    obs(&mut m, Obs::Usbsts(STS_QUIET));
    assert_eq!(refused(&mut m, Operation::Request(Step::Halt)), Reason::SchedulesStillActive);
    obs(&mut m, Obs::Usbcmd(0x01));
    obs(&mut m, Obs::Usbsts(0x0000_6008));
    assert_eq!(refused(&mut m, Operation::Request(Step::Halt)), Reason::SchedulesStillActive);
    // reset needs the controller observed halted and Run/Stop clear
    let mut m = at(dell(), Phase::Halted);
    obs(&mut m, Obs::Usbcmd(0x01));
    obs(&mut m, Obs::Usbsts(STS_HALT));
    assert_eq!(refused(&mut m, Operation::Request(Step::Reset)), Reason::NotHalted);
    obs(&mut m, Obs::Usbcmd(0));
    obs(&mut m, Obs::Usbsts(STS_QUIET));
    assert_eq!(refused(&mut m, Operation::Request(Step::Reset)), Reason::NotHalted);
    // a stale observation after a write is not enough
    let mut m = at(dell(), Phase::Halted);
    assert_eq!(refused(&mut m, Operation::Request(Step::Reset)), Reason::StaleObservation(Field::Usbcmd));
}

#[test]
fn missing_inferred_contradictory_and_malformed_facts() {
    let refuses = |f: EhciFacts, why: Reason| {
        let mut m = Machine::new();
        assert_eq!(step(&mut m, Operation::Qualify(f)), Err(why));
        assert_eq!(m.phase(), Phase::Refused);
        assert_eq!(m.failure(), Some(why));
        assert_eq!(step(&mut m, Operation::Request(Step::Handoff)), Err(Reason::Terminal));
    };
    refuses(EhciFacts::UNKNOWN, Reason::MissingFact(Field::Vendor));
    refuses(EhciFacts { vendor: Fact::Inferred(0x8086), ..dell() }, Reason::InferredOnly(Field::Vendor));
    refuses(EhciFacts { vendor: Fact::Contradictory, ..dell() }, Reason::ContradictoryFact(Field::Vendor));
    refuses(EhciFacts { vendor: Fact::Observed(0x1b36), ..dell() }, Reason::NotIntel);
    refuses(EhciFacts { device: Fact::Unknown, ..dell() }, Reason::MissingFact(Field::Device));
    refuses(EhciFacts { class_code: Fact::Observed(0x000c_0330), ..dell() }, Reason::NotEhciClass);
    refuses(EhciFacts { class_code: Fact::Unknown, ..dell() }, Reason::MissingFact(Field::ClassCode));
    refuses(EhciFacts { hci_version: Fact::Observed(0x0095), ..dell() }, Reason::HciVersionUnsupported);
    refuses(EhciFacts { hci_version: Fact::Unknown, ..dell() }, Reason::MissingFact(Field::HciVersion));
    refuses(EhciFacts { n_ports: Fact::Observed(0), ..dell() }, Reason::PortsInvalid);
    refuses(EhciFacts { n_ports: Fact::Observed(16), ..dell() }, Reason::PortsInvalid);
    refuses(EhciFacts { n_ports: Fact::Unknown, ..dell() }, Reason::MissingFact(Field::Ports));
    refuses(EhciFacts { n_ports: Fact::Contradictory, ..dell() }, Reason::ContradictoryFact(Field::Ports));
    // N_PORTS larger than N_CC x N_PCC cannot both be true
    refuses(EhciFacts { n_ports: Fact::Observed(6), n_companions: Fact::Observed(2), ports_per_companion: Fact::Observed(2), ..dell() }, Reason::ContradictoryFact(Field::Ports));
    // a maximal port count is accepted
    assert_eq!(qualify(&EhciFacts { n_ports: Fact::Observed(15), ..dell() }), Ok(Qualification { ports: 15 }));

    // required-fact matrix: unknown / inferred / contradictory block exactly the dependent step
    let base = EhciFacts { caplength: Fact::Observed(0x20), ..dell() };
    type Set = fn(EhciFacts, Fact<u8>, Fact<bool>, Fact<u32>, Fact<Option<u8>>) -> EhciFacts;
    let cases: [(Step, Field, Set); 9] = [
        (Step::Handoff, Field::Legacy, |f, _, _, _, l| EhciFacts { legacy: l, ..f }),
        (Step::Handoff, Field::LegacyBios, |f, _, b, _, _| EhciFacts { legacy_bios_owned: b, ..f }),
        (Step::Handoff, Field::LegacyOs, |f, _, b, _, _| EhciFacts { legacy_os_owned: b, ..f }),
        (Step::MaskSmi, Field::LegacyCtl, |f, _, _, v, _| EhciFacts { legacy_ctl: v, ..f }),
        (Step::Halt, Field::Caplength, |f, c, _, _, _| EhciFacts { caplength: c, ..f }),
        (Step::Halt, Field::Usbcmd, |f, _, _, v, _| EhciFacts { usbcmd: v, ..f }),
        (Step::StopSchedules, Field::Usbsts, |f, _, _, v, _| EhciFacts { usbsts: v, ..f }),
        (Step::Reset, Field::Companions, |f, c, _, _, _| EhciFacts { n_companions: c, ..f }),
        (Step::Reset, Field::Usbcmd, |f, _, _, v, _| EhciFacts { usbcmd: v, ..f }),
    ];
    for (s, field, set) in cases {
        for (kind, mk) in [("unknown", 0), ("inferred", 1), ("contradictory", 2)] {
            let f = set(
                base,
                [Fact::Unknown, Fact::Inferred(0), Fact::Contradictory][mk],
                [Fact::Unknown, Fact::Inferred(true), Fact::Contradictory][mk],
                [Fact::Unknown, Fact::Inferred(0), Fact::Contradictory][mk],
                [Fact::Unknown, Fact::Inferred(Some(0x68)), Fact::Contradictory][mk],
            );
            let want = match mk {
                0 => Reason::MissingFact(field),
                1 => Reason::InferredOnly(field),
                _ => Reason::ContradictoryFact(field),
            };
            assert_eq!(readiness(&f, s), Readiness::Blocked(want), "{s:?} with {field:?} {kind}");
        }
    }
    // malformed capability offsets and CAPLENGTH values
    for off in [0u8, 0x3c, 0x41, 0xfc, 0xff] {
        assert_eq!(readiness(&EhciFacts { legacy: Fact::Observed(Some(off)), ..base }, Step::Handoff), Readiness::Blocked(Reason::LegacyOffsetInvalid), "offset {off:#x}");
    }
    for off in [0x40u8, 0x68, 0xf8] {
        assert_eq!(readiness(&EhciFacts { legacy: Fact::Observed(Some(off)), ..base }, Step::Handoff), Readiness::Ready);
    }
    for c in [0u8, 0x0c, 0x0f, 0x21, 0x41, 0x44, 0xff] {
        assert_eq!(readiness(&EhciFacts { caplength: Fact::Observed(c), ..base }, Step::Halt), Readiness::Blocked(Reason::CaplengthInvalid), "caplength {c:#x}");
    }
    for c in [0x10u8, 0x20, 0x40] {
        assert_eq!(readiness(&EhciFacts { caplength: Fact::Observed(c), ..base }, Step::Halt), Readiness::Ready);
    }
}

#[test]
fn fact_reconciliation_rules() {
    use Fact::*;
    assert_eq!(Unknown.reconcile(Observed(1u8)), Observed(1));
    assert_eq!(Observed(1u8).reconcile(Unknown), Observed(1));
    assert_eq!(Observed(1u8).reconcile(Observed(1)), Observed(1));
    assert_eq!(Observed(1u8).reconcile(Observed(2)), Contradictory);
    assert_eq!(Observed(1u8).reconcile(Inferred(2)), Observed(1), "an observation replaces a disagreeing inference");
    assert_eq!(Inferred(2u8).reconcile(Observed(1)), Observed(1));
    assert_eq!(Inferred(1u8).reconcile(Inferred(1)), Inferred(1));
    assert_eq!(Inferred(1u8).reconcile(Inferred(2)), Contradictory);
    assert_eq!(Contradictory.reconcile(Observed(1u8)), Contradictory);
    assert_eq!(Observed(1u8).reconcile(Contradictory), Contradictory);
    assert_eq!(Unknown::<u8>.reconcile(Unknown), Unknown);
    assert_eq!(Inferred(true).observed(), None);
    assert_eq!(Observed(true).observed(), Some(true));
    // a live CAPLENGTH that disagrees with a surveyed one makes the fact contradictory and blocks the steps that use it
    let mut m = at(EhciFacts { caplength: Fact::Observed(0x20), ..dell() }, Phase::Qualified);
    assert_eq!(obs(&mut m, Obs::Caplength(0x20)).why, Rule::Recorded);
    assert_eq!(obs(&mut m, Obs::Caplength(0x30)).why, Rule::Contradicted);
    assert_eq!(m.facts().caplength, Fact::Contradictory);
    assert_eq!(obs(&mut m, Obs::Caplength(0x20)).why, Rule::Recorded, "a contradiction is not silently healed");
    assert_eq!(m.facts().caplength, Fact::Contradictory);
    assert_eq!(readiness(m.facts(), Step::Halt), Readiness::Blocked(Reason::ContradictoryFact(Field::Caplength)));
    assert_eq!(readiness(m.facts(), Step::Handoff), Readiness::Ready, "unrelated steps are unaffected");
}

#[test]
fn ordering_and_terminal_phases() {
    // every step is refused from every phase except its predecessor
    let pred = |s: Step| match s {
        Step::Handoff => Phase::Qualified,
        Step::MaskSmi => Phase::HandedOff,
        Step::StopSchedules => Phase::SmiMasked,
        Step::Halt => Phase::Quiet,
        Step::Reset => Phase::Halted,
    };
    for want in [Phase::Qualified, Phase::HandedOff, Phase::SmiMasked, Phase::Quiet, Phase::Halted] {
        for s in Step::ALL {
            if pred(*s) == want {
                continue;
            }
            let mut m = at(dell(), want);
            assert_eq!(refused(&mut m, Operation::Request(*s)), Reason::WrongPhase, "{s:?} from {want:?}");
        }
    }
    // nothing but Qualify is accepted from Idle, and Qualify only from Idle
    let mut m = Machine::new();
    for op in [Operation::Observe(Obs::Usbcmd(0)), Operation::Advance(1), Operation::Request(Step::Handoff)] {
        assert_eq!(refused(&mut m, op), Reason::WrongPhase);
    }
    let mut m = at(dell(), Phase::Qualified);
    assert_eq!(refused(&mut m, Operation::Qualify(dell())), Reason::WrongPhase);
    // terminal phases accept nothing and emit nothing
    for terminal in [Phase::Reset, Phase::Failed, Phase::Refused] {
        let mut m = match terminal {
            Phase::Reset => {
                let mut m = at(dell(), Phase::Resetting);
                obs(&mut m, Obs::Usbcmd(0));
                obs(&mut m, Obs::Usbsts(0x1000));
                obs(&mut m, Obs::Configflag(false));
                m
            }
            Phase::Failed => {
                let mut m = at(dell(), Phase::Qualified);
                obs(&mut m, Obs::ReadFailed);
                m
            }
            _ => {
                let mut m = Machine::new();
                let _ = step(&mut m, Operation::Qualify(EhciFacts::UNKNOWN));
                m
            }
        };
        assert_eq!(m.phase(), terminal);
        let w = m.writes();
        for op in [Operation::Observe(Obs::Usbcmd(0)), Operation::Advance(5), Operation::Request(Step::Handoff), Operation::Request(Step::Reset), Operation::Qualify(dell())] {
            assert_eq!(refused(&mut m, op), Reason::Terminal, "{terminal:?}");
        }
        assert_eq!(m.writes(), w);
    }
    // time must move forward, and cannot overflow
    let mut m = at(dell(), Phase::Qualified);
    assert_eq!(refused(&mut m, Operation::Advance(0)), Reason::BadTicks);
    ok(&mut m, Operation::Advance(u64::MAX - 1));
    assert_eq!(refused(&mut m, Operation::Advance(2)), Reason::BadTicks);
}

#[test]
fn no_legacy_and_already_owned_paths_need_no_write() {
    // a controller without a legacy capability (as QEMU's EHCI): handoff and SMI steps do not apply
    let mut m = Machine::new();
    ok(&mut m, Operation::Qualify(no_legacy()));
    assert_eq!(readiness(&no_legacy(), Step::Handoff), Readiness::NotNeeded);
    assert_eq!(readiness(&no_legacy(), Step::MaskSmi), Readiness::NotNeeded);
    let a = req(&mut m, Step::Handoff);
    assert_eq!((a.why, a.event), (Rule::HandoffNotNeeded, None));
    let a = req(&mut m, Step::MaskSmi);
    assert_eq!((a.why, a.event), (Rule::SmiNotNeeded, None));
    assert_eq!(m.writes(), 0);
    // already owned: BIOS clear, OS set, observed fresh
    let mut m = at(dell(), Phase::Qualified);
    obs(&mut m, Obs::LegacySup { bios: false, os: true });
    let a = req(&mut m, Step::Handoff);
    assert_eq!((a.why, a.event, a.to), (Rule::HandoffAlreadyOwned, None, Phase::HandedOff));
    // OS semaphore already set and BIOS still holding: wait without writing again
    let mut m = at(dell(), Phase::Qualified);
    obs(&mut m, Obs::LegacySup { bios: true, os: true });
    let a = req(&mut m, Step::Handoff);
    assert_eq!((a.why, a.event, a.to), (Rule::HandoffWait, None, Phase::Handoff));
    // nobody owns it: the OS takes the semaphore
    let mut m = at(dell(), Phase::Qualified);
    obs(&mut m, Obs::LegacySup { bios: false, os: false });
    assert_eq!(req(&mut m, Step::Handoff).event, Some(Event::LegacyOsSemaphore { cap: 0x68 }));
    // schedules already off and controller already halted: no write
    let mut m = at(no_legacy(), Phase::SmiMasked);
    obs(&mut m, Obs::Caplength(0x20));
    obs(&mut m, Obs::Usbcmd(0x0000_0001));
    obs(&mut m, Obs::Usbsts(STS_QUIET));
    let a = req(&mut m, Step::StopSchedules);
    assert_eq!((a.why, a.event), (Rule::SchedulesAlreadyOff, None));
    obs(&mut m, Obs::Usbcmd(0));
    obs(&mut m, Obs::Usbsts(STS_HALT));
    let a = req(&mut m, Step::Halt);
    assert_eq!((a.why, a.event, a.to), (Rule::HaltAlreadyHalted, None, Phase::Halted));
    // Run/Stop already clear but not yet halted, schedules enable clear but status still set: wait without writing
    let mut m = at(no_legacy(), Phase::SmiMasked);
    obs(&mut m, Obs::Caplength(0x20));
    obs(&mut m, Obs::Usbcmd(0x0000_0001));
    obs(&mut m, Obs::Usbsts(0x0000_6000));
    let a = req(&mut m, Step::StopSchedules);
    assert_eq!((a.why, a.event, a.to), (Rule::SchedulesWait, None, Phase::Stopping));
    obs(&mut m, Obs::Usbsts(STS_QUIET));
    obs(&mut m, Obs::Usbcmd(0));
    obs(&mut m, Obs::Usbsts(STS_QUIET));
    let a = req(&mut m, Step::Halt);
    assert_eq!((a.why, a.event, a.to), (Rule::HaltWait, None, Phase::Halting));
}

#[test]
fn malformed_capability_offset_and_caplength_are_refused_by_the_machine() {
    let mut m = at(EhciFacts { legacy: Fact::Observed(Some(0x41)), ..dell() }, Phase::Qualified);
    obs(&mut m, Obs::LegacySup { bios: true, os: false });
    assert_eq!(refused(&mut m, Operation::Request(Step::Handoff)), Reason::LegacyOffsetInvalid);
    // a live CAPLENGTH that is out of range blocks the operational steps; the machine reaches them via a no-legacy controller
    let mut m = at(no_legacy(), Phase::SmiMasked);
    obs(&mut m, Obs::Caplength(0x44));
    obs(&mut m, Obs::Usbcmd(CMD0));
    obs(&mut m, Obs::Usbsts(STS_RUNNING));
    assert_eq!(refused(&mut m, Operation::Request(Step::StopSchedules)), Reason::CaplengthInvalid);
}

#[test]
fn reset_is_refused_when_companion_controllers_exist() {
    // a reset clears CONFIGFLAG and would hand the ports to companions the model does not cover
    let f = EhciFacts { n_companions: Fact::Observed(1), ports_per_companion: Fact::Observed(2), ..dell() };
    let mut m = at(f, Phase::Halted);
    obs(&mut m, Obs::Usbcmd(0));
    obs(&mut m, Obs::Usbsts(STS_HALT));
    assert_eq!(refused(&mut m, Operation::Request(Step::Reset)), Reason::CompanionsPresent);
    assert_eq!(m.phase(), Phase::Halted);
}

#[test]
fn the_model_never_assumes_what_is_behind_a_root_port() {
    // The facts record a connected root port only as an observation of the controller; the vocabulary has no field
    // for a device, keyboard, mouse or hub, and the fixtures state that the identity is unobserved.
    for fx in dell::both() {
        let p = fx.provenance.iter().find(|p| p.field == "root-port-1-device-identity").unwrap();
        assert_eq!(p.note, Note::Unobserved);
        assert!(p.survey_line.contains("a hub behind it?"));
    }
    let names: Vec<&str> = Field::ALL.iter().map(|f| f.name()).collect();
    for banned in ["keyboard", "mouse", "hub", "hid", "device-identity"] {
        assert!(names.iter().all(|n| !n.contains(banned)), "{banned}");
    }
}

#[test]
fn names_are_unique_and_stable() {
    let mut seen = HashSet::new();
    for r in Rule::ALL {
        assert!(seen.insert(r.name()), "{}", r.name());
    }
    for p in Phase::ALL {
        assert!(seen.insert(p.name()), "{}", p.name());
    }
    let mut reasons = HashSet::new();
    let mut all = vec![
        Reason::WrongPhase,
        Reason::Terminal,
        Reason::NotIntel,
        Reason::NotEhciClass,
        Reason::HciVersionUnsupported,
        Reason::PortsInvalid,
        Reason::CompanionsPresent,
        Reason::LegacyOffsetInvalid,
        Reason::CaplengthInvalid,
        Reason::SchedulesStillActive,
        Reason::NotHalted,
        Reason::BadTicks,
        Reason::FirmwareNoRelease,
        Reason::FirmwareRevoked,
        Reason::SmiNotDisabled,
        Reason::ScheduleStopTimeout,
        Reason::HaltTimeout,
        Reason::ResetTimeout,
        Reason::ResetUnverified,
        Reason::HostSystemError,
        Reason::ReadFailed,
    ];
    for f in Field::ALL {
        all.push(Reason::MissingFact(*f));
        all.push(Reason::InferredOnly(*f));
        all.push(Reason::ContradictoryFact(*f));
    }
    for f in [Field::LegacyBios, Field::LegacyCtl, Field::Usbcmd, Field::Usbsts, Field::Caplength] {
        all.push(Reason::StaleObservation(f));
    }
    for r in &all {
        reasons.insert(r.name());
    }
    // stale.legacy-owner is shared by the two owner bits; every other name is unique to one reason
    assert_eq!(reasons.len(), all.len(), "reason names collide");
}

// ---- seeded safety property against an independent tracker -------------------------------------------------------

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
    fn pick<T: Copy>(&mut self, xs: &[T]) -> T {
        xs[self.below(xs.len() as u64) as usize]
    }
}

/// What a cooperative controller would let happen next from `phase`, so the random streams also get deep into the protocol.
fn guided_op(r: &mut Rng, phase: Phase) -> Operation {
    let o = |x: Obs| Operation::Observe(x);
    match phase {
        Phase::Qualified => r.pick(&[o(Obs::LegacySup { bios: true, os: false }), Operation::Request(Step::Handoff)]),
        Phase::Handoff => r.pick(&[o(Obs::LegacySup { bios: false, os: true }), Operation::Advance(1)]),
        Phase::HandedOff => r.pick(&[o(Obs::LegacyCtl(0x2005)), Operation::Request(Step::MaskSmi)]),
        Phase::MaskingSmi => o(Obs::LegacyCtl(0)),
        Phase::SmiMasked => r.pick(&[o(Obs::Caplength(0x20)), o(Obs::Usbcmd(CMD0)), o(Obs::Usbsts(STS_RUNNING)), Operation::Request(Step::StopSchedules)]),
        Phase::Stopping => o(Obs::Usbsts(STS_QUIET)),
        Phase::Quiet => r.pick(&[o(Obs::Usbcmd(1)), o(Obs::Usbsts(STS_QUIET)), Operation::Request(Step::Halt)]),
        Phase::Halting => o(Obs::Usbsts(STS_HALT)),
        Phase::Halted => r.pick(&[o(Obs::Usbcmd(0)), o(Obs::Usbsts(STS_HALT)), Operation::Request(Step::Reset)]),
        _ => r.pick(&[o(Obs::Usbcmd(0)), o(Obs::Usbsts(0x1000)), o(Obs::Configflag(false)), o(Obs::Configflag(true))]),
    }
}

fn random_op(r: &mut Rng) -> Operation {
    match r.below(10) {
        0 => Operation::Observe(Obs::Caplength(r.pick(&[0x20, 0x20, 0x20, 0x30, 0x0f, 0x44]))),
        1 | 2 => Operation::Observe(Obs::LegacySup { bios: r.below(2) == 0, os: r.below(3) != 0 }),
        3 => Operation::Observe(Obs::LegacyCtl(r.pick(&[0, 0, 0x2005, 0x000a_2005, 0xffff_ffff, 0x0000_0001]))),
        4 => Operation::Observe(Obs::Usbcmd(r.pick(&[0x11, 0x01, 0x00, 0x02, 0x31, 0x10]))),
        5 => Operation::Observe(Obs::Usbsts(r.pick(&[0x600a, 0x2008, 0x3008, 0x1000, 0x0000, 0x6000, 0x0010, 0x8000]))),
        6 => Operation::Observe(Obs::Configflag(r.below(2) == 0)),
        7 => Operation::Advance(r.pick(&[1, 1, 5, 20, 100, 400, 1500])),
        _ => Operation::Request(r.pick(Step::ALL)),
    }
}

/// An independent record of what has been observed since the last write that touches each register.
#[derive(Default)]
struct Tracker {
    sup: Option<(bool, bool)>,
    ctl: Option<u32>,
    cmd: Option<u32>,
    sts: Option<u32>,
    rank: u8,
    writes: u8,
    terminal: bool,
}

fn export(line: &str, out: &mut Option<String>) {
    if let Some(s) = out {
        s.push_str(line);
        s.push('\n');
    }
}

fn fact_text<T: Copy, F: Fn(T) -> String>(f: Fact<T>, show: F) -> String {
    match f {
        Fact::Unknown => "U".into(),
        Fact::Contradictory => "C".into(),
        Fact::Observed(v) => format!("O:{}", show(v)),
        Fact::Inferred(v) => format!("I:{}", show(v)),
    }
}

fn facts_text(f: &EhciFacts) -> String {
    let x = |v: u32| format!("{v:x}");
    let b = |v: bool| (v as u8).to_string();
    [
        fact_text(f.vendor, |v| x(v.into())),
        fact_text(f.device, |v| x(v.into())),
        fact_text(f.class_code, x),
        fact_text(f.hci_version, |v| x(v.into())),
        fact_text(f.caplength, |v| x(v.into())),
        fact_text(f.n_ports, |v| x(v.into())),
        fact_text(f.n_companions, |v| x(v.into())),
        fact_text(f.ports_per_companion, |v| x(v.into())),
        fact_text(f.legacy, |v| v.map_or("none".to_string(), |o| x(o.into()))),
        fact_text(f.legacy_bios_owned, b),
        fact_text(f.legacy_os_owned, b),
        fact_text(f.legacy_ctl, x),
        fact_text(f.usbcmd, x),
        fact_text(f.usbsts, x),
        fact_text(f.configflag, b),
        fact_text(f.firmware_active, b),
    ]
    .join(" ")
}

fn op_text(op: &Operation) -> String {
    match op {
        Operation::Qualify(f) => format!("Q {}", facts_text(f)),
        Operation::Observe(Obs::Caplength(v)) => format!("O CAP {v:x}"),
        Operation::Observe(Obs::LegacySup { bios, os }) => format!("O SUP {} {}", *bios as u8, *os as u8),
        Operation::Observe(Obs::LegacyCtl(v)) => format!("O CTL {v:x}"),
        Operation::Observe(Obs::Usbcmd(v)) => format!("O CMD {v:x}"),
        Operation::Observe(Obs::Usbsts(v)) => format!("O STS {v:x}"),
        Operation::Observe(Obs::Configflag(v)) => format!("O CFG {}", *v as u8),
        Operation::Observe(Obs::ReadFailed) => "O FAIL".into(),
        Operation::Advance(n) => format!("A {n}"),
        Operation::Request(s) => format!("R {}", s.name()),
    }
}

fn result_text(r: &Result<Accepted, Reason>) -> String {
    match r {
        Err(e) => format!("err {}", e.name()),
        Ok(a) => {
            let ev = match a.event {
                None => "none".to_string(),
                Some(Event::LegacyOsSemaphore { cap }) => format!("os:{cap:x}"),
                Some(Event::LegacyCtlWrite { cap, value }) => format!("ctl:{cap:x}:{value:x}"),
                Some(Event::UsbcmdWrite { value }) => format!("cmd:{value:x}"),
            };
            format!("ok {} {} {} {} {}", a.from.name(), a.to.name(), a.why.name(), ev, a.failure.map_or("none", |f| f.name()))
        }
    }
}

#[test]
fn seeded_random_operations_keep_the_safety_properties() {
    let seeds: u64 = 3000;
    let ops_per_seed = 60;
    let mut trace = std::env::var_os("EHCI_TRACE_OUT").map(|_| String::new());
    let mut reached = HashSet::new();
    let mut events = HashSet::new();
    for seed in 1..=seeds {
        let mut r = Rng(seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1);
        let facts = match seed % 4 {
            0 => dell::ehci_8c2d().facts,
            1 => dell::ehci_8c26().facts,
            2 => no_legacy(),
            _ => EhciFacts { caplength: Fact::Observed(0x20), ..dell::ehci_8c2d().facts },
        };
        let mut m = Machine::new();
        let mut t = Tracker::default();
        let export_this = trace.is_some() && seed <= 300;
        let mut local = if export_this { Some(String::new()) } else { None };
        let q = Operation::Qualify(facts);
        let res = step(&mut m, q);
        export(&format!("T {seed}"), &mut local);
        export(&op_text(&q), &mut local);
        export(&result_text(&res), &mut local);
        for _ in 0..ops_per_seed {
            let op = if r.below(10) < 6 { guided_op(&mut r, m.phase()) } else { random_op(&mut r) };
            let before_phase = m.phase();
            let res = step(&mut m, op);
            export(&op_text(&op), &mut local);
            export(&result_text(&res), &mut local);
            reached.insert(m.phase().name());
            // terminal phases accept nothing and emit nothing
            if t.terminal {
                assert_eq!(res, Err(Reason::Terminal), "seed {seed}: {op:?}");
            }
            match (&op, &res) {
                (Operation::Observe(o), Ok(_)) => match o {
                    Obs::LegacySup { bios, os } => t.sup = Some((*bios, *os)),
                    Obs::LegacyCtl(v) => t.ctl = Some(*v),
                    Obs::Usbcmd(v) => t.cmd = Some(*v),
                    Obs::Usbsts(v) => t.sts = Some(*v),
                    _ => {}
                },
                _ => {}
            }
            if let Ok(a) = &res {
                // an event comes only from an accepted request, never from an observation or a tick
                if a.event.is_some() {
                    assert!(matches!(op, Operation::Request(_)), "seed {seed}: event from {op:?}");
                    t.writes += 1;
                    events.insert(a.why.name());
                }
                if a.to == Phase::Failed || a.to == Phase::Reset {
                    t.terminal = true;
                }
                match a.event {
                    Some(Event::LegacyOsSemaphore { cap }) => {
                        assert_eq!((a.why, cap), (Rule::HandoffRequest, 0x68));
                        assert!(t.rank < 1, "seed {seed}: semaphore out of order");
                        t.rank = 1;
                        // written only when the last observation showed the OS semaphore clear
                        assert_eq!(t.sup.map(|s| s.1), Some(false), "seed {seed}: semaphore written without a fresh observation");
                        t.sup = None;
                    }
                    Some(Event::LegacyCtlWrite { value, .. }) => {
                        assert!(t.rank < 2, "seed {seed}: SMI control out of order");
                        t.rank = 2;
                        assert_eq!(value & LEGCTL_ENABLES, 0);
                        assert_eq!(value & 0xffff_0000, 0, "status bits must be written as zero");
                        assert!(t.ctl.is_some_and(|c| c & LEGCTL_ENABLES != 0), "seed {seed}: write without an enable set");
                        assert_eq!(value, t.ctl.unwrap() & 0xffff & !LEGCTL_ENABLES);
                        t.ctl = None;
                    }
                    Some(Event::UsbcmdWrite { value }) => {
                        let (cmd, sts) = (t.cmd.expect("fresh USBCMD"), t.sts.expect("fresh USBSTS"));
                        match a.why {
                            Rule::SchedulesRequest => {
                                assert!(t.rank < 3, "seed {seed}: schedules out of order");
                                t.rank = 3;
                                assert_eq!(value, cmd & !(CMD_PERIODIC_ENABLE | CMD_ASYNC_ENABLE));
                            }
                            Rule::HaltRequest => {
                                assert!(t.rank < 4, "seed {seed}: halt out of order");
                                t.rank = 4;
                                // independent check: schedules observed off immediately before Run/Stop is cleared
                                assert_eq!(cmd & (CMD_PERIODIC_ENABLE | CMD_ASYNC_ENABLE), 0, "seed {seed}");
                                assert_eq!(sts & (STS_PERIODIC_ACTIVE | STS_ASYNC_ACTIVE), 0, "seed {seed}");
                                assert_eq!(value, cmd & !CMD_RUN);
                            }
                            Rule::ResetRequest => {
                                assert!(t.rank < 5, "seed {seed}: reset out of order");
                                t.rank = 5;
                                // independent check: halted and Run/Stop clear when the reset is requested
                                assert_ne!(sts & STS_HALTED, 0, "seed {seed}");
                                assert_eq!(cmd & CMD_RUN, 0, "seed {seed}");
                                assert_eq!(value, cmd | CMD_HCRESET);
                            }
                            other => panic!("seed {seed}: USBCMD write under {other:?}"),
                        }
                        t.cmd = None;
                        t.sts = None;
                    }
                    None => {}
                }
                assert_eq!(m.writes(), t.writes, "seed {seed}");
            } else if before_phase.terminal() {
                assert_eq!(m.writes(), t.writes);
            }
            if t.terminal {
                assert!(m.phase().terminal());
            }
        }
        if let (Some(all), Some(one)) = (&mut trace, &local) {
            all.push_str(one);
        }
    }
    // the property is not vacuous: every non-terminal phase and every kind of write were reached
    for p in ["qualified", "handoff", "handed-off", "masking-smi", "smi-masked", "stopping-schedules", "quiet", "halting", "halted", "resetting", "reset", "failed"] {
        assert!(reached.contains(p), "phase {p} never reached");
    }
    for e in ["handoff.request", "smi.request", "schedules.request", "halt.request", "reset.request"] {
        assert!(events.contains(e), "event {e} never emitted");
    }
    if let (Some(path), Some(text)) = (std::env::var_os("EHCI_TRACE_OUT"), trace) {
        std::fs::write(path, text).unwrap();
    }
}

#[test]
fn zz_every_rule_and_simple_reason_is_exercised() {
    let g = COVERAGE.lock().unwrap();
    let (rules, reasons) = g.as_ref().expect("the other tests ran first (use --test-threads=1)");
    let missing: Vec<&str> = Rule::ALL.iter().map(|r| r.name()).filter(|n| !rules.contains(n)).collect();
    assert!(missing.is_empty(), "rules never exercised: {missing:?}");
    for r in [
        Reason::WrongPhase,
        Reason::Terminal,
        Reason::NotIntel,
        Reason::NotEhciClass,
        Reason::HciVersionUnsupported,
        Reason::PortsInvalid,
        Reason::CompanionsPresent,
        Reason::LegacyOffsetInvalid,
        Reason::CaplengthInvalid,
        Reason::SchedulesStillActive,
        Reason::NotHalted,
        Reason::BadTicks,
        Reason::FirmwareNoRelease,
        Reason::FirmwareRevoked,
        Reason::SmiNotDisabled,
        Reason::ScheduleStopTimeout,
        Reason::HaltTimeout,
        Reason::ResetTimeout,
        Reason::ResetUnverified,
        Reason::HostSystemError,
        Reason::ReadFailed,
    ] {
        // failure reasons are reported through `Accepted::failure`, refusal reasons through `Err`
        let seen_as_refusal = reasons.contains(r.name());
        let failure_reason = matches!(
            r,
            Reason::FirmwareNoRelease | Reason::FirmwareRevoked | Reason::SmiNotDisabled | Reason::ScheduleStopTimeout | Reason::HaltTimeout | Reason::ResetTimeout | Reason::ResetUnverified | Reason::HostSystemError | Reason::ReadFailed
        );
        assert!(seen_as_refusal || failure_reason, "reason never exercised: {}", r.name());
    }
}
