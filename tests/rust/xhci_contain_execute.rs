//! USB.2-C3b.2 executor tests (built only with `--cfg vos_contain_test`).
//!
//! A scripted device models the QEMU xHCI profile: PCI command, USBCMD/USBSTS,
//! the reset completing synchronously, and a read budget per phase. Faults are
//! injected at exact points. An independent oracle (a stage table written from
//! the contract, not from the code) predicts the terminal state and the number
//! of writes; invariants hold on every run, including 20,000 randomized ones:
//! at most one PCI write and one USBCMD write, W1 before W2, W1 only ever
//! clears Bus Master Enable, bus mastering is never set again, W2 only with
//! bus mastering off, every poll bounded.
use vos_xhci::xhci::containment::*;

const OP: u32 = 0x40;
const AFTER_RESET_READS: u32 = 112; // 8 operational + 8 ports + 16 interrupters x 6

fn idle() -> Observed {
    Observed {
        bar: 0xc0_0000_0000,
        id: ID,
        class_revision: CLASS_REVISION,
        subsystem: SUBSYSTEM_ID,
        command: 0x0007,
        status: 0x0010,
        msix_control: 0x000f,
        msi_control: 0,
        pcie: PCIE_IDLE,
        legacy: false,
        ports_busy: false,
        extended: 2,
        header: HEADER,
        protocol_ports: 0xff,
        usbcmd: 0,
        usbsts: 1,
        config: 0x40,
        crcr: 0x675_6241,
        dcbaap: 0x675_6000,
        interrupter_enables: 1,
        ip_pending: 0,
        findings: 0,
        reserved: 0,
        owned_again: 0,
        protected: 0,
        hazards: 0,
        excluded: 7,
        memory: 0x0123_4567_89ab_cdef,
    }
}
fn ctx() -> Context {
    Context { uefi: true, qualified: true, operational: OP, execute: true, environment: 0 }
}
fn clean_after(o: &Observed) -> AfterReset {
    AfterReset {
        usbcmd: 0, usbsts: 1, dnctrl: 0, config: 0, crcr: 0, dcbaap: 0, ports_active: false, interrupters_clear: true,
        command: o.command & !BUS_MASTER, status: o.status, msix_control: o.msix_control, msi_control: o.msi_control,
        pcie: PCIE_IDLE, bar: o.bar,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Check { Usbcmd, Usbsts, Dnctrl, Config, Crcr, Dcbaap, Ports, Interrupters, Command, Status, Msix, Msi, Pcie, Bar }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Fault {
    // eligibility and the three rehearsal passes
    Ineligible, Environment, Subsystem, RehearsalDrift(u32), RehearsalUnreadable(u32),
    // before W1
    Unreadable4, Drift4, Quick1Drift, Quick1Unreadable, PortAtQuick1,
    // W1 and its verification
    W1Refused, Quick2Unreadable, W1Ignored, W1ClearsMore, W1StatusChanged, Hce, Hse, Running,
    Unreadable5, Drift5, Observe5Command,
    // before W2
    Quick3Drift, Quick3Unreadable, PortAtQuick3, W2Refused,
    // reset and verification
    ResetNeverCompletes, CnrNeverClears, PollUnreadable, Slow(u32), PhaseRefused1, PhaseRefused2, AfterResetUnreadable,
    BudgetUnlimited, Verify(Check),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum W { Pci { old: u16, new: u16 }, Mmio { old: u32, new: u32 } }

struct Dev {
    faults: Vec<Fault>,
    cmd: u16,
    status: u16,
    usbcmd: u32,
    usbsts: u32,
    obs: u32,
    quicks: u32,
    w1_calls: Vec<(u16, u16)>,
    w2_calls: Vec<(u32, u32)>,
    log: Vec<W>,
    phases: Vec<u32>,
    budget: u32,
    after_w2_quicks: u32,
    reset_done: bool,
    bme_set_after_w1: bool,
}

impl Dev {
    fn new(faults: &[Fault]) -> Self {
        Dev { faults: faults.to_vec(), cmd: 0x0007, status: 0x0010, usbcmd: 0, usbsts: 1, obs: 0, quicks: 0,
              w1_calls: vec![], w2_calls: vec![], log: vec![], phases: vec![], budget: 256, after_w2_quicks: 0,
              reset_done: false, bme_set_after_w1: false }
    }
    fn has(&self, f: Fault) -> bool {
        self.faults.contains(&f)
    }
    fn spend(&mut self, reads: u32) -> bool {
        if self.budget < reads {
            return false;
        }
        self.budget -= reads;
        true
    }
}

impl Source for Dev {
    fn observe(&mut self, o: &mut Observed) -> bool {
        self.obs += 1;
        let n = self.obs;
        if self.has(Fault::RehearsalUnreadable(n)) || (n == 4 && self.has(Fault::Unreadable4))
            || (n == 5 && self.has(Fault::Unreadable5)) {
            return false;
        }
        *o = idle();
        o.command = self.cmd;
        o.usbcmd = self.usbcmd;
        o.usbsts = self.usbsts;
        if n == 1 && self.has(Fault::Ineligible) {
            o.usbsts = 0x9;
        }
        if n == 1 && self.has(Fault::Subsystem) {
            o.subsystem = 0x1100_1af5;
        }
        if self.has(Fault::RehearsalDrift(n)) || (n == 4 && self.has(Fault::Drift4)) || (n == 5 && self.has(Fault::Drift5)) {
            o.memory ^= u64::from(n) << 8; // unique per pass, so two drifting passes still differ
        }
        if n == 5 && self.has(Fault::Observe5Command) {
            o.command |= 0x400;
        }
        true
    }
}

impl Effects for Dev {
    fn write_command(&mut self, old: u16, value: u16) -> bool {
        // Any attempt to set Bus Master Enable, or any second call, is the violation.
        if value & BUS_MASTER != 0 || !self.w1_calls.is_empty() {
            self.bme_set_after_w1 = true;
        }
        self.w1_calls.push((old, value));
        if self.has(Fault::W1Refused) {
            return false;
        }
        self.log.push(W::Pci { old, new: value });
        if self.has(Fault::W1Ignored) {
            return true;
        }
        self.cmd = value;
        if self.has(Fault::W1ClearsMore) {
            self.cmd &= !1;
        }
        if self.has(Fault::W1StatusChanged) {
            self.status |= 0x0100;
        }
        if self.has(Fault::Hce) {
            self.usbsts |= 0x1000;
        }
        if self.has(Fault::Hse) {
            self.usbsts |= 0x4;
        }
        if self.has(Fault::Running) {
            self.usbcmd |= 1;
            self.usbsts &= !1;
        }
        true
    }
    fn write_usbcmd(&mut self, old: u32, value: u32) -> bool {
        self.w2_calls.push((old, value));
        if self.has(Fault::W2Refused) {
            return false;
        }
        self.log.push(W::Mmio { old, new: value });
        // QEMU: the reset runs inside the write; HCRST and CNR are then clear.
        self.usbcmd = value;
        self.reset_done = true;
        true
    }
    fn quick(&mut self, q: &mut Quick, ports: bool) -> bool {
        self.quicks += 1;
        let n = self.quicks;
        if self.reset_done {
            assert!(!ports, "the reset poll does not read the ports");
            self.after_w2_quicks += 1;
            assert!(self.after_w2_quicks <= 5000, "unbounded poll");
            if !self.spend(2) {
                return false;
            }
            let k = self.after_w2_quicks;
            if self.has(Fault::PollUnreadable) && k == 1 {
                return false;
            }
            let slow = self.faults.iter().filter_map(|f| if let Fault::Slow(s) = f { Some(*s) } else { None }).max();
            let (mut usbcmd, mut usbsts) = (0u32, 1u32);
            if self.has(Fault::ResetNeverCompletes) || slow.is_some_and(|s| k <= s) {
                usbcmd = HCRST;
            }
            if self.has(Fault::CnrNeverClears) {
                usbsts = 1 | 1 << 11;
            }
            *q = Quick { command: self.cmd, status: self.status, usbcmd, usbsts, ports_busy: false };
            return true;
        }
        if (n == 1 && self.has(Fault::Quick1Unreadable)) || (n == 2 && self.has(Fault::Quick2Unreadable))
            || (n == 3 && self.has(Fault::Quick3Unreadable)) {
            return false;
        }
        *q = Quick { command: self.cmd, status: self.status, usbcmd: self.usbcmd, usbsts: self.usbsts, ports_busy: false };
        assert!(ports || n == 2, "the immediate re-reads before each write include the ports; the read-back does not");
        if (n == 1 && self.has(Fault::PortAtQuick1)) || (n == 3 && self.has(Fault::PortAtQuick3)) {
            q.ports_busy = true;
        }
        if n == 1 && self.has(Fault::Quick1Drift) {
            q.usbsts ^= 0x10;
        }
        if n == 3 && self.has(Fault::Quick3Drift) {
            q.status ^= 0x0008;
        }
        true
    }
    fn phase(&mut self, reads: u32) -> bool {
        self.phases.push(reads);
        let i = self.phases.len();
        if (i == 1 && self.has(Fault::PhaseRefused1)) || (i == 2 && self.has(Fault::PhaseRefused2)) {
            return false;
        }
        self.budget = if self.has(Fault::BudgetUnlimited) { u32::MAX } else { reads };
        true
    }
    fn after_reset(&mut self, a: &mut AfterReset) -> bool {
        if self.has(Fault::AfterResetUnreadable) || !self.spend(AFTER_RESET_READS) {
            return false;
        }
        *a = clean_after(&idle());
        match self.faults.iter().find_map(|f| if let Fault::Verify(v) = f { Some(*v) } else { None }) {
            Some(Check::Usbcmd) => a.usbcmd = 1,
            Some(Check::Usbsts) => a.usbsts = 9,
            Some(Check::Dnctrl) => a.dnctrl = 1,
            Some(Check::Config) => a.config = 0x40,
            Some(Check::Crcr) => a.crcr = 0x675_6241,
            Some(Check::Dcbaap) => a.dcbaap = 0x675_6000,
            Some(Check::Ports) => a.ports_active = true,
            Some(Check::Interrupters) => a.interrupters_clear = false,
            Some(Check::Command) => a.command |= BUS_MASTER,
            Some(Check::Status) => a.status |= 8,
            Some(Check::Msix) => a.msix_control |= 0x8000,
            Some(Check::Msi) => a.msi_control |= 1,
            Some(Check::Pcie) => a.pcie = PCIE_PENDING,
            Some(Check::Bar) => a.bar += 0x1000,
            None => {}
        }
        true
    }
}

fn run_with(faults: &[Fault], c: &Context) -> (Execution, Dev) {
    let mut d = Dev::new(faults);
    let c = if faults.contains(&Fault::Environment) { Context { environment: 1, ..*c } } else { *c };
    let mut first = Observed::default();
    let x = execute(&mut d, &c, &mut first);
    (x, d)
}
fn run(faults: &[Fault]) -> (Execution, Dev) {
    run_with(faults, &ctx())
}

/// The independent oracle: (stage, terminal state, writes) per fault, from the
/// contract. The earliest stage present decides; none present means contained.
fn oracle_one(f: Fault) -> (u8, State, u8) {
    use Fault::*;
    match f {
        Ineligible | Environment | Subsystem | RehearsalDrift(_) | RehearsalUnreadable(_) => (0, State::Refused, 0),
        Unreadable4 | Drift4 | Quick1Drift | Quick1Unreadable | PortAtQuick1 => (1, State::PreW1Drift, 0),
        W1Refused => (2, State::W1Failed, 0),
        Quick2Unreadable => (3, State::W1Failed, 1),
        W1Ignored => (4, State::BusMasterStuck, 1),
        W1ClearsMore | W1StatusChanged => (5, State::PciChanged, 1),
        Hce | Hse | Running => (6, State::ControllerError, 1),
        Unreadable5 | Drift5 | Observe5Command => (7, State::ContainedBmeOnly, 1),
        Quick3Drift | Quick3Unreadable | PortAtQuick3 | W2Refused => (8, State::PreW2Drift, 1),
        ResetNeverCompletes | CnrNeverClears | PollUnreadable | PhaseRefused1 => (9, State::ResetStuck, 2),
        Slow(k) => if k <= POLL_BOUND - 1 { (11, State::Contained, 2) } else { (9, State::ResetStuck, 2) },
        BudgetUnlimited => (11, State::Contained, 2),
        PhaseRefused2 | AfterResetUnreadable | Verify(_) => (10, State::ResetUnverified, 2),
    }
}
fn oracle(faults: &[Fault]) -> (State, u8) {
    faults.iter().map(|f| oracle_one(*f)).min_by_key(|(s, ..)| *s).map_or((State::Contained, 2), |(_, st, w)| (st, w))
}

fn all_single_faults() -> Vec<Fault> {
    use Fault::*;
    let mut v = vec![Ineligible, Environment, Subsystem, Unreadable4, Drift4, Quick1Drift, Quick1Unreadable, PortAtQuick1, PortAtQuick3, W1Refused,
        Quick2Unreadable, W1Ignored, W1ClearsMore, W1StatusChanged, Hce, Hse, Running, Unreadable5, Drift5, Observe5Command,
        Quick3Drift, Quick3Unreadable, W2Refused, ResetNeverCompletes, CnrNeverClears, PollUnreadable, PhaseRefused1,
        PhaseRefused2, AfterResetUnreadable, BudgetUnlimited, Slow(1), Slow(500), Slow(1023), Slow(1024), Slow(1025), Slow(5000)];
    for k in 1..=3 {
        v.push(RehearsalDrift(k));
        v.push(RehearsalUnreadable(k));
    }
    for x in [Check::Usbcmd, Check::Usbsts, Check::Dnctrl, Check::Config, Check::Crcr, Check::Dcbaap, Check::Ports,
              Check::Interrupters, Check::Command, Check::Status, Check::Msix, Check::Msi, Check::Pcie, Check::Bar] {
        v.push(Verify(x));
    }
    v
}

/// Properties that hold for every run, whatever the outcome.
fn invariants(x: &Execution, d: &Dev, faults: &[Fault]) {
    let ctxs = format!("{faults:?} -> {:?}", x.state);
    assert!(d.w1_calls.len() <= 1 && d.w2_calls.len() <= 1, "at most one call per write adapter: {ctxs}");
    for (old, new) in &d.w1_calls {
        assert!(old & BUS_MASTER != 0 && new & BUS_MASTER == 0 && *new == old & !BUS_MASTER, "W1 clears only BME: {ctxs}");
    }
    for (old, new) in &d.w2_calls {
        assert_eq!(*new, old | HCRST, "W2 adds only HCRST: {ctxs}");
        assert!(!d.w1_calls.is_empty() && d.log.iter().any(|w| matches!(w, W::Pci { .. })), "W2 only after a confirmed W1: {ctxs}");
        assert_eq!(*old, 0, "W2 only from USBCMD = 0: {ctxs}");
    }
    let order: Vec<_> = d.log.iter().map(|w| matches!(w, W::Pci { .. })).collect();
    assert!(matches!(order.as_slice(), [] | [true] | [true, false]), "W1 then W2, nothing else: {ctxs}");
    assert_eq!(usize::from(x.writes), d.log.len(), "reported writes equal issued writes: {ctxs}");
    assert!(!d.bme_set_after_w1, "bus mastering is never set again: {ctxs}");
    assert!(d.after_w2_quicks <= POLL_BOUND, "polls bounded: {ctxs}");
    assert!(x.polls <= POLL_BOUND, "reported polls bounded: {ctxs}");
    let allowed: &[State] = match x.writes {
        0 => &[State::Refused, State::PreW1Drift, State::W1Failed],
        1 => &[State::W1Failed, State::BusMasterStuck, State::PciChanged, State::ControllerError, State::ContainedBmeOnly,
               State::PreW2Drift],
        _ => &[State::ResetStuck, State::ResetUnverified, State::Contained],
    };
    assert!(allowed.contains(&x.state), "terminal state matches the number of writes: {ctxs}");
    if x.writes >= 1 {
        assert_eq!(d.cmd & BUS_MASTER == 0, !faults.contains(&Fault::W1Ignored), "bus mastering ends off after W1: {ctxs}");
    }
}

#[test]
fn eligible_run_issues_exactly_w1_then_w2() {
    let (x, d) = run(&[]);
    assert_eq!(x.state, State::Contained);
    assert_eq!(x.writes, 2);
    assert_eq!(d.log, vec![W::Pci { old: 0x0007, new: 0x0003 }, W::Mmio { old: 0, new: 2 }]);
    assert_eq!(d.w1_calls, vec![(0x0007, 0x0003)]);
    assert_eq!(d.w2_calls, vec![(0, 2)]);
    assert_eq!(x.w1, [0x0007, 0x0003, 0x0003, 0x0010]);
    assert_eq!(x.w2, [0, 2]);
    assert_eq!(x.polls, 1, "QEMU completes the reset inside the write");
    assert_eq!(x.usbsts_after_w1, 1);
    assert_eq!(d.phases, vec![2 * POLL_BOUND, 256], "bounded phases for the poll and the verification");
    assert_eq!(x.rehearsal.state, State::DryRunComplete);
    invariants(&x, &d, &[]);
}

#[test]
fn execution_needs_the_execute_flag_even_when_eligible() {
    let (x, d) = run_with(&[], &Context { execute: false, ..ctx() });
    assert_eq!(x.state, State::Refused);
    assert_eq!((x.writes, d.w1_calls.len(), d.w2_calls.len()), (0, 0, 0));
    assert_eq!(x.rehearsal.state, State::DryRunComplete, "the dry-run itself still completes");
}

#[test]
fn environment_and_subsystem_refuse_only_when_executing() {
    let o = idle();
    assert_eq!(refusals(&o, &ctx()), 0);
    assert_eq!(refusals(&o, &Context { environment: 1, ..ctx() }), ENVIRONMENT);
    assert_eq!(refusals(&o, &Context { environment: 7, ..ctx() }), ENVIRONMENT);
    let bad = Observed { subsystem: 0x1100_1af5, ..o };
    assert_eq!(refusals(&bad, &ctx()), SUBSYSTEM);
    assert_eq!(refusals(&Observed { subsystem: 0, ..o }, &ctx()), SUBSYSTEM);
    let dry = Context { execute: false, ..ctx() };
    assert_eq!(refusals(&bad, &dry), 0, "a dry-run never looks at the subsystem or the environment");
    assert_eq!(refusals(&o, &Context { environment: 7, ..dry }), 0);
    assert_eq!(refusals(&bad, &Context { environment: 1, ..ctx() }), SUBSYSTEM | ENVIRONMENT);
    assert_eq!((SUBSYSTEM_ID & 0xffff, SUBSYSTEM_ID >> 16), (0x1af4, 0x1100), "QEMU's default subsystem");
}

#[test]
fn every_single_fault_matches_the_oracle_and_invariants() {
    for f in all_single_faults() {
        let (x, d) = run(&[f]);
        let (state, writes) = oracle(&[f]);
        assert_eq!((x.state, x.writes), (state, writes), "{f:?}");
        invariants(&x, &d, &[f]);
        if x.writes == 0 {
            // Only a refused W1 call can precede a zero-write outcome; W2 is never called.
            assert!(d.w2_calls.is_empty() && d.log.is_empty(), "nothing written, no W2 call: {f:?}");
            assert_eq!(d.w1_calls.len(), usize::from(f == Fault::W1Refused), "{f:?}");
        }
        if x.state == State::Contained {
            assert_eq!(x.writes, 2);
        }
    }
}

#[test]
fn eligibility_faults_never_reach_an_adapter() {
    use Fault::*;
    for f in [Ineligible, Environment, Subsystem, RehearsalDrift(1), RehearsalDrift(2), RehearsalDrift(3),
              RehearsalUnreadable(1), RehearsalUnreadable(2), RehearsalUnreadable(3)] {
        let (x, d) = run(&[f]);
        assert_eq!(x.state, State::Refused, "{f:?}");
        assert!(d.w1_calls.is_empty() && d.w2_calls.is_empty() && d.log.is_empty(), "{f:?}");
        assert_eq!(d.obs <= 3, true, "stops within the rehearsal: {f:?}");
    }
}

#[test]
fn reset_timeout_is_bounded_and_exact() {
    let (x, d) = run(&[Fault::ResetNeverCompletes]);
    assert_eq!((x.state, x.polls, d.after_w2_quicks), (State::ResetStuck, POLL_BOUND, POLL_BOUND));
    let (x, d) = run(&[Fault::CnrNeverClears]);
    assert_eq!((x.state, x.polls, d.after_w2_quicks), (State::ResetStuck, POLL_BOUND, POLL_BOUND));
    // Completes on poll k+1; the last allowed poll is POLL_BOUND.
    for (slow, state, polls) in [(0, State::Contained, 1), (1, State::Contained, 2), (1022, State::Contained, 1023),
                                 (1023, State::Contained, 1024), (1024, State::ResetStuck, 1024), (9999, State::ResetStuck, 1024)] {
        let (x, d) = run(&[Fault::Slow(slow)]);
        assert_eq!((x.state, x.polls), (state, polls), "slow {slow}");
        invariants(&x, &d, &[Fault::Slow(slow)]);
    }
    // The explicit bound holds on its own, even if the platform's read budget were broken.
    let (x, d) = run(&[Fault::ResetNeverCompletes, Fault::BudgetUnlimited]);
    assert_eq!((x.state, x.polls, d.after_w2_quicks), (State::ResetStuck, POLL_BOUND, POLL_BOUND));
    let (x, _) = run(&[Fault::PollUnreadable]);
    assert_eq!(x.state, State::ResetStuck, "an unreadable poll stops the loop; nothing is written");
    let (x, d) = run(&[Fault::PhaseRefused1]);
    assert_eq!((x.state, d.after_w2_quicks), (State::ResetStuck, 0), "no poll without a budget");
}

#[test]
fn after_a_stuck_reset_nothing_more_is_written() {
    for f in [Fault::ResetNeverCompletes, Fault::CnrNeverClears, Fault::PollUnreadable] {
        let (x, d) = run(&[f]);
        assert_eq!(x.state, State::ResetStuck);
        assert_eq!(d.log.len(), 2, "W1 and W2 only; no abort, retry or second reset: {f:?}");
        assert_eq!(d.cmd & BUS_MASTER, 0, "bus mastering stays off: {f:?}");
    }
}

#[test]
fn bus_mastering_is_never_restored_on_any_path() {
    for f in all_single_faults() {
        let (x, d) = run(&[f]);
        for (_, new) in &d.w1_calls {
            assert_eq!(new & BUS_MASTER, 0, "{f:?}");
        }
        assert!(!d.bme_set_after_w1, "{f:?}");
        if x.writes >= 1 && f != Fault::W1Ignored {
            assert_eq!(d.cmd & BUS_MASTER, 0, "{f:?}");
        }
        assert!(d.w1_calls.len() <= 1, "never a second command write: {f:?}");
    }
}

#[test]
fn w1_ignored_by_the_device_is_final_and_blocks_the_reset() {
    let (x, d) = run(&[Fault::W1Ignored]);
    assert_eq!(x.state, State::BusMasterStuck);
    assert_eq!(x.writes, 1);
    assert!(d.w2_calls.is_empty(), "no reset while bus mastering is still on");
    assert_eq!(d.cmd & BUS_MASTER, BUS_MASTER, "and nothing else touches it");
}

#[test]
fn failures_after_w1_block_w2() {
    use Fault::*;
    for f in [Quick2Unreadable, W1ClearsMore, W1StatusChanged, Hce, Hse, Running, Unreadable5, Drift5, Observe5Command,
              Quick3Drift, Quick3Unreadable, PortAtQuick3, W2Refused] {
        let (x, d) = run(&[f]);
        assert_eq!(x.writes, 1, "{f:?}");
        assert!(d.log.iter().all(|w| matches!(w, W::Pci { .. })), "{f:?}");
    }
}

#[test]
fn reset_verification_checks_every_field_individually() {
    let first = idle();
    let good = clean_after(&first);
    assert!(reset_verified(&first, &good));
    let bad: Vec<(&str, AfterReset)> = vec![
        ("usbcmd", AfterReset { usbcmd: 1, ..good }), ("usbcmd 2", AfterReset { usbcmd: 2, ..good }),
        ("usbsts", AfterReset { usbsts: 0, ..good }), ("usbsts hce", AfterReset { usbsts: 0x1001, ..good }),
        ("usbsts cnr", AfterReset { usbsts: 0x801, ..good }), ("dnctrl", AfterReset { dnctrl: 2, ..good }),
        ("config", AfterReset { config: 1, ..good }), ("crcr", AfterReset { crcr: 0x40, ..good }),
        ("crcr crr", AfterReset { crcr: 8, ..good }), ("dcbaap", AfterReset { dcbaap: 0x1000, ..good }),
        ("ports", AfterReset { ports_active: true, ..good }), ("interrupters", AfterReset { interrupters_clear: false, ..good }),
        ("bme back", AfterReset { command: first.command, ..good }), ("command other", AfterReset { command: 0x0001, ..good }),
        ("status", AfterReset { status: 0x0018, ..good }), ("msix", AfterReset { msix_control: 0x800f, ..good }),
        ("msi", AfterReset { msi_control: 1, ..good }), ("pcie", AfterReset { pcie: PCIE_PENDING, ..good }),
        ("pcie absent", AfterReset { pcie: PCIE_ABSENT, ..good }), ("bar", AfterReset { bar: first.bar + 1, ..good }),
    ];
    for (name, a) in bad {
        assert!(!reset_verified(&first, &a), "{name}");
    }
}

#[test]
fn verification_failures_end_unverified_with_both_writes_made() {
    for v in [Check::Usbcmd, Check::Usbsts, Check::Dnctrl, Check::Config, Check::Crcr, Check::Dcbaap, Check::Ports,
              Check::Interrupters, Check::Command, Check::Status, Check::Msix, Check::Msi, Check::Pcie, Check::Bar] {
        let (x, d) = run(&[Fault::Verify(v)]);
        assert_eq!((x.state, x.writes), (State::ResetUnverified, 2), "{v:?}");
        assert_eq!(d.log.len(), 2, "{v:?}");
    }
}

#[test]
fn randomized_faults_match_the_oracle() {
    let singles = all_single_faults();
    let mut s = 0x9e37_79b9_7f4a_7c15u64;
    let mut rnd = |n: u64| {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        s % n
    };
    let mut outcomes = std::collections::BTreeMap::new();
    for _ in 0..20_000 {
        let k = rnd(4) as usize; // 0..=3 faults
        let faults: Vec<Fault> = (0..k).map(|_| singles[rnd(singles.len() as u64) as usize]).collect();
        let (x, d) = run(&faults);
        let (state, writes) = oracle(&faults);
        assert_eq!((x.state, x.writes), (state, writes), "{faults:?}");
        invariants(&x, &d, &faults);
        *outcomes.entry(format!("{:?}", x.state)).or_insert(0u32) += 1;
    }
    for s in ["Refused", "PreW1Drift", "W1Failed", "BusMasterStuck", "PciChanged", "ControllerError", "ContainedBmeOnly",
              "PreW2Drift", "ResetStuck", "ResetUnverified", "Contained"] {
        assert!(outcomes.get(s).copied().unwrap_or(0) > 0, "random runs reach {s}: {outcomes:?}");
    }
}

#[test]
fn rehearsal_inside_an_execution_is_the_read_only_c3b1_rehearsal() {
    let (x, d) = run(&[]);
    assert_eq!(x.rehearsal.entries().len(), 6);
    assert!(x.rehearsal.entries().iter().filter(|t| t.kind == K_PCI_WRITE || t.kind == K_MMIO_WRITE)
        .all(|t| t.result == R_NOT_ISSUED), "the rehearsal part only proposes");
    // Observations: A, R0, R1 of the rehearsal, then the pre-W1 and post-W1 passes.
    assert_eq!(d.obs, 5);
    assert_eq!(d.quicks, 3 + 1, "pre-W1, post-W1, pre-W2 and one poll");
}
