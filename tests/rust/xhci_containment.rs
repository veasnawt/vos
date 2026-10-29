//! USB.2-C3b.1 pure tests: containment policy, the dry-run state machine, the
//! proposed write widths/offsets/masks, the D1 interrupt rule and the C3b.2
//! failure model. Observed values are taken from real QEMU boots (OVMF and
//! SeaBIOS, idle and with a keyboard); the oracle below is written from the
//! requirement table, not from the implementation.
#![forbid(unsafe_code)]
use vos_xhci::xhci::containment::*;
use vos_xhci::xhci::lifecycle::{self, Facts, Step, Violation};
use vos_xhci::xhci::residue::{DEVICES, ERDP_OUTSIDE, EXHAUSTED, HAZARD, MALFORMED, READ_FAILED, RING_OPEN, SCRATCHPADS, UNINSPECTED};

const OP: u32 = 0x40;

/// Idle OVMF controller as observed (guard boot, PLAN VALID).
fn idle() -> Observed {
    Observed {
        bar: 0xc0_0000_0000,
        id: ID,
        class_revision: CLASS_REVISION,
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
    Context { uefi: true, qualified: true, operational: OP }
}

/// Scripted observations: pass `k` returns `script[k]` (None = unreadable).
struct Script {
    obs: Vec<Option<Observed>>,
    calls: usize,
}
impl Source for Script {
    fn observe(&mut self, out: &mut Observed) -> bool {
        let k = self.calls;
        self.calls += 1;
        match self.obs.get(k).copied().flatten() {
            Some(o) => {
                *out = o;
                true
            }
            None => false,
        }
    }
}
fn run(obs: Vec<Option<Observed>>, c: &Context) -> (Rehearsal, usize, Observed) {
    let mut s = Script { obs, calls: 0 };
    let mut first = Observed::default();
    let r = rehearse(&mut s, c, &mut first);
    (r, s.calls, first)
}
fn steady(o: Observed) -> Vec<Option<Observed>> {
    vec![Some(o); 3]
}

#[test]
fn eligible_idle_uefi_completes_and_issues_nothing() {
    assert!(!ISSUED, "C3b.1 issues nothing");
    let (r, calls, first) = run(steady(idle()), &ctx());
    assert_eq!((r.state, r.refusals, r.plan_len, calls), (State::DryRunComplete, 0, 5, 3), "{r:?}");
    assert_eq!(first, idle());
    let t = r.entries();
    let kinds: Vec<(u8, u8)> = t.iter().map(|e| (e.kind, e.result)).collect();
    assert_eq!(kinds, vec![(K_REVALIDATE, R_OK), (K_PCI_WRITE, R_NOT_ISSUED), (K_REVALIDATE, R_OK),
                           (K_MMIO_WRITE, R_NOT_ISSUED), (K_RESET_POLL, R_NOT_RUN), (K_VERIFY, R_NOT_RUN)]);
    assert_eq!(t.iter().map(|e| e.step).collect::<Vec<_>>(), vec![1, 2, 3, 4, 5, 6]);
    // The two proposed writes, exactly.
    assert_eq!((t[1].width, t[1].offset, t[1].old, t[1].new), (16, 0x04, 0x0007, 0x0003));
    assert_eq!((t[3].width, t[3].offset, t[3].old, t[3].new), (32, OP, 0, 2));
    // Poll and verify expectations are definitions only.
    assert_eq!((t[4].offset, t[4].old, t[4].new), (OP + 4, POLL_BOUND, 1 << 11));
    assert_eq!((t[5].offset, t[5].old), (OP + 4, IDLE_USBSTS));
    // Both revalidations saw the same fingerprint as the first observation.
    assert!(t[0].old == t[0].new && t[2].old == t[2].new && t[0].old == t[2].old && t[0].old == idle().fingerprint());
    // Plan: no mask, no halt, exactly the effects the model requires.
    assert!(idle().interrupts_disabled());
}

#[test]
fn each_requirement_is_individually_enforced() {
    type Edit = fn(&mut Observed);
    let cases: Vec<(&str, u32, Edit)> = vec![
        ("wrong vendor/device", PROFILE, |o| o.id = 0x0194_1033),
        ("wrong class/revision", PROFILE, |o| o.class_revision = 0x0c03_2001),
        ("HCIVERSION/CAPLENGTH", PROFILE, |o| o.header[0] = 0x0100_0080),
        ("HCSPARAMS1", PROFILE, |o| o.header[1] = 0x0800_0040),
        ("HCSPARAMS3", PROFILE, |o| o.header[3] = 1),
        ("HCCPARAMS1 (64-byte contexts)", PROFILE, |o| o.header[4] = 0x0008_7005),
        ("DBOFF", PROFILE, |o| o.header[5] = 0x3000),
        ("RTSOFF", PROFILE, |o| o.header[6] = 0x2000),
        ("extended capability count", PROFILE, |o| o.extended = 3),
        ("protocol ports", PROFILE, |o| o.protocol_ports = 0x0f),
        ("legacy support capability", PROFILE, |o| o.legacy = true),
        ("command 0x0003 (no bus master)", PCI_COMMAND, |o| o.command = 0x0003),
        ("command 0x0107 (SeaBIOS, SERR)", PCI_COMMAND, |o| o.command = 0x0107),
        ("command 0x0407 (INTx disabled)", PCI_COMMAND, |o| o.command = 0x0407),
        ("command 0", PCI_COMMAND, |o| o.command = 0),
        ("status INTx asserted", PCI_STATUS | INTERRUPTS, |o| o.status = 0x0018),
        ("status error bit", PCI_STATUS, |o| o.status = 0x8010),
        ("no PCIe capability", PCIE_UNKNOWN, |o| o.pcie = PCIE_ABSENT),
        ("PCIe transactions pending", PCIE_BUSY, |o| o.pcie = PCIE_PENDING),
        ("USBCMD.INTE", USBCMD | INTERRUPTS, |o| o.usbcmd = 4),
        ("USBSTS.EINT", USBSTS | INTERRUPTS, |o| o.usbsts = 9),
        ("IMAN.IP pending", INTERRUPTS, |o| o.ip_pending = 1),
        ("MSI-X enabled", INTERRUPTS, |o| o.msix_control = 0x800f),
        ("MSI enabled", INTERRUPTS, |o| o.msi_control = 1),
        ("running (R/S)", RUNNING | USBCMD, |o| o.usbcmd = 1),
        ("not halted", RUNNING | USBSTS, |o| o.usbsts = 0),
        ("USBCMD.HCRST", USBCMD, |o| o.usbcmd = 2),
        ("USBSTS.HSE", USBSTS, |o| o.usbsts = 5),
        ("USBSTS.PCD", USBSTS, |o| o.usbsts = 0x11),
        ("USBSTS.SRE", USBSTS, |o| o.usbsts = 0x401),
        ("USBSTS.CNR", USBSTS, |o| o.usbsts = 0x801),
        ("USBSTS.HCE", USBSTS, |o| o.usbsts = 0x1001),
        ("port connected or changed", PORTS, |o| o.ports_busy = true),
        ("active slot (device context)", SLOTS | RESIDUE, |o| o.findings = DEVICES),
        ("scratchpad finding", SCRATCH | RESIDUE, |o| o.findings = SCRATCHPADS),
        ("scratchpads advertised (low)", SCRATCH, |o| o.header[2] = 0x0800_000f),
        ("scratchpads advertised (high)", SCRATCH, |o| o.header[2] = 0x0020_000f),
        ("residue uninspected", RESIDUE, |o| o.findings = UNINSPECTED),
        ("residue hazard", RESIDUE | MEMORY, |o| {
            o.findings = HAZARD;
            o.hazards = 1;
        }),
        ("residue exhausted", RESIDUE, |o| o.findings = EXHAUSTED),
        ("residue malformed", RESIDUE, |o| o.findings = MALFORMED),
        ("command ring open", RESIDUE, |o| o.findings = RING_OPEN),
        ("ERDP outside", RESIDUE, |o| o.findings = ERDP_OUTSIDE),
        ("read failed", RESIDUE, |o| o.findings = READ_FAILED),
        ("frames reserved (not excluded)", MEMORY, |o| o.reserved = 1),
        ("frames already owned", MEMORY, |o| o.owned_again = 1),
        ("frames protected only", MEMORY, |o| o.protected = 1),
        ("no excluded frame at all", MEMORY, |o| o.excluded = 0),
    ];
    for (what, bits, edit) in cases {
        let mut o = idle();
        edit(&mut o);
        assert_eq!(refusals(&idle(), &ctx()), 0);
        let m = refusals(&o, &ctx());
        assert!(m & bits == bits && m != 0, "{what}: mask {m:#x} lacks {bits:#x}");
        let (r, calls, _) = run(steady(o), &ctx());
        assert_eq!((r.state, calls, r.len), (State::Refused, 1, 0), "{what}: refusal stops at the first pass, no trace");
        assert!(r.refusals & bits == bits && r.plan_len == 0, "{what}");
    }
    let (r, ..) = run(steady(idle()), &Context { uefi: false, ..ctx() });
    assert_eq!(r.refusals, NOT_UEFI, "BIOS is refused");
    let (r, ..) = run(steady(idle()), &Context { qualified: false, ..ctx() });
    assert_eq!(r.refusals, UNQUALIFIED);
}

/// Requirement table restated independently of `refusals`.
fn oracle(o: &Observed, c: &Context) -> u32 {
    let mut m = 0;
    if !c.uefi { m |= 1 << 0 }
    if !c.qualified { m |= 1 << 1 }
    let header_ok = o.header[0] == 0x0100_0040 && o.header[1] == 0x0800_1040 && o.header[2] == 0xf && o.header[3] == 0
        && o.header[4] == 0x0008_7001 && o.header[5] == 0x2000 && o.header[6] == 0x1000;
    if !(o.id == 0x000d_1b36 && o.class_revision == 0x0c03_3001 && header_ok && o.extended == 2
        && o.protocol_ports == 0xff && !o.legacy) { m |= 1 << 2 }
    if o.command != 0x0007 { m |= 1 << 3 }
    if o.status != 0x0010 { m |= 1 << 4 }
    if o.pcie == 0 { m |= 1 << 5 }
    if o.pcie == 2 { m |= 1 << 6 }
    let quiet = o.usbcmd & 4 == 0 && o.usbsts & 8 == 0 && o.ip_pending == 0 && o.msix_control & 0x8000 == 0
        && o.msi_control & 1 == 0 && o.status & 8 == 0;
    if !quiet { m |= 1 << 7 }
    if o.usbcmd & 1 != 0 || o.usbsts & 1 == 0 { m |= 1 << 8 }
    if o.usbcmd != 0 { m |= 1 << 9 }
    if o.usbsts != 1 { m |= 1 << 10 }
    if o.ports_busy { m |= 1 << 11 }
    if o.findings & 0x20 != 0 { m |= 1 << 12 }
    if o.findings & 0x40 != 0 || (o.header[2] >> 21) & 0x1f != 0 || (o.header[2] >> 27) != 0 { m |= 1 << 13 }
    if o.findings != 0 { m |= 1 << 14 }
    if o.reserved != 0 || o.owned_again != 0 || o.protected != 0 || o.hazards != 0 || o.excluded == 0 { m |= 1 << 15 }
    m
}

#[test]
fn refusal_mask_equals_an_independent_oracle_on_random_observations() {
    let mut x = 0x9e37_79b9_7f4a_7c15u64;
    let mut rnd = move |n: u64| {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x % n
    };
    let (mut eligible, mut refused) = (0u32, 0u32);
    for round in 0..20_000 {
        let mut o = idle();
        for _ in 0..rnd(4) {
            match rnd(16) {
                0 => o.id = [ID, 0x0194_1033, 0][rnd(3) as usize],
                1 => o.header[rnd(7) as usize] = [HEADER[0], 0, 0xf, 0x0800_000f, 0x8000_0000][rnd(5) as usize],
                2 => o.extended = rnd(4) as u8,
                3 => o.command = [7, 3, 0x107, 0x407, 0, 5][rnd(6) as usize],
                4 => o.status = [0x10, 0x18, 0x8010, 0][rnd(4) as usize],
                5 => o.pcie = rnd(3) as u8,
                6 => o.usbcmd = [0, 1, 2, 4, 8, 0x80][rnd(6) as usize],
                7 => o.usbsts = [1, 0, 9, 5, 0x11, 0x801, 0x1001, 0x401][rnd(8) as usize],
                8 => o.ports_busy = rnd(2) == 1,
                9 => o.findings = [0, DEVICES, SCRATCHPADS, UNINSPECTED, HAZARD, MALFORMED, 0x3][rnd(7) as usize],
                10 => o.excluded = rnd(3) as u8 * 3,
                11 => o.reserved = rnd(2) as u8,
                12 => o.ip_pending = rnd(3) as u16,
                13 => o.msix_control = [0x000f, 0x800f][rnd(2) as usize],
                14 => o.msi_control = rnd(2) as u16,
                _ => o.hazards = rnd(2) as u8,
            }
        }
        let c = Context { uefi: rnd(5) != 0, qualified: rnd(7) != 0, operational: OP };
        let (got, want) = (refusals(&o, &c), oracle(&o, &c));
        assert_eq!(got, want, "round {round}: {o:?}");
        let (r, calls, _) = run(steady(o), &c);
        if want == 0 {
            eligible += 1;
            assert_eq!((r.state, calls), (State::DryRunComplete, 3));
        } else {
            refused += 1;
            assert_eq!((r.state, calls, r.len), (State::Refused, 1, 0));
        }
    }
    assert!(eligible > 100 && refused > 1000, "the property exercised both outcomes ({eligible}/{refused})");
}

#[test]
fn observed_configurations_are_refused_with_exactly_the_native_masks() {
    // UEFI + usb-kbd: halted, EINT set, slot 1 live, port connected (native mask 0x5c80).
    let mut o = idle();
    o.usbsts = 9;
    o.findings = DEVICES;
    o.ports_busy = true;
    o.excluded = 8;
    assert_eq!(refusals(&o, &ctx()), INTERRUPTS | USBSTS | PORTS | SLOTS | RESIDUE);
    // SeaBIOS + usb-kbd: controller running, firmware memory map-protected (native mask 0xcf89).
    let mut o = idle();
    (o.command, o.usbcmd, o.usbsts, o.findings, o.ports_busy, o.excluded) = (0x0107, 1, 8, 3, true, 0);
    assert_eq!(refusals(&o, &Context { uefi: false, ..ctx() }),
               NOT_UEFI | PCI_COMMAND | INTERRUPTS | RUNNING | USBCMD | USBSTS | PORTS | RESIDUE | MEMORY);
    // SeaBIOS idle: PORTSC change bit set, residue uninspected (native mask 0xc809).
    let mut o = idle();
    (o.command, o.findings, o.ports_busy, o.excluded) = (0x0107, 2, true, 0);
    assert_eq!(refusals(&o, &Context { uefi: false, ..ctx() }), NOT_UEFI | PCI_COMMAND | PORTS | RESIDUE | MEMORY);
    assert_eq!(0x5c80, INTERRUPTS | USBSTS | PORTS | SLOTS | RESIDUE);
}

#[test]
fn revalidation_detects_every_change_and_unreadable_pass() {
    type Edit = fn(&mut Observed);
    let edits: Vec<(&str, Edit)> = vec![
        ("BAR moved", |o| o.bar += 0x4000),
        ("device id", |o| o.id ^= 1),
        ("class", |o| o.class_revision ^= 1),
        ("command", |o| o.command = 0x0003),
        ("status", |o| o.status = 0x0018),
        ("MSI-X control", |o| o.msix_control = 0x800f),
        ("MSI control", |o| o.msi_control = 1),
        ("PCIe pending", |o| o.pcie = PCIE_PENDING),
        ("legacy", |o| o.legacy = true),
        ("ports", |o| o.ports_busy = true),
        ("capability header", |o| o.header[4] ^= 8),
        ("extended caps", |o| o.extended += 1),
        ("protocol ports", |o| o.protocol_ports ^= 1),
        ("USBCMD", |o| o.usbcmd = 4),
        ("USBSTS", |o| o.usbsts = 9),
        ("CONFIG", |o| o.config = 0x20),
        ("CRCR", |o| o.crcr += 0x40),
        ("DCBAAP", |o| o.dcbaap += 0x1000),
        ("IE mask", |o| o.interrupter_enables = 3),
        ("pending", |o| o.ip_pending = 2),
        ("findings", |o| o.findings = UNINSPECTED),
        ("reserved", |o| o.reserved = 1),
        ("excluded", |o| o.excluded = 6),
        ("memory digest", |o| o.memory ^= 1),
    ];
    for (what, edit) in &edits {
        let mut moved = idle();
        edit(&mut moved);
        assert_ne!(moved, idle(), "{what}");
        // Changed before the first write (revalidation 0) and before the second (1).
        let (r, calls, _) = run(vec![Some(idle()), Some(moved), Some(idle())], &ctx());
        assert_eq!((r.state, r.refusals, calls, r.len), (State::Refused, CHANGED, 2, 1), "{what} at R0: {r:?}");
        assert_eq!(r.entries()[0].result, R_CHANGED);
        let (r, calls, _) = run(vec![Some(idle()), Some(idle()), Some(moved)], &ctx());
        assert_eq!((r.state, r.refusals, calls, r.len), (State::Refused, CHANGED, 3, 3), "{what} at R1: {r:?}");
        assert_eq!((r.entries()[1].kind, r.entries()[1].result), (K_PCI_WRITE, R_NOT_ISSUED));
        assert_eq!(r.entries()[2].result, R_CHANGED);
    }
    // A command register that already shows the proposed value means somebody wrote it.
    let mut cleared = idle();
    cleared.command = 0x0003;
    let (r, ..) = run(vec![Some(idle()), Some(idle()), Some(cleared)], &ctx());
    assert_eq!(r.refusals, CHANGED, "an unexpected bus-master clear is detected, never assumed");
    // Unreadable passes fail closed with their own reason.
    for k in 0..3 {
        let mut obs = steady(idle());
        obs[k] = None;
        let (r, calls, _) = run(obs, &ctx());
        assert_eq!((r.state, r.refusals, calls), (State::Refused, UNREADABLE, k + 1), "pass {k}");
    }
    // No path observes more than three times.
    let (_, calls, _) = run(steady(idle()), &ctx());
    assert_eq!(calls, 3);
}

#[test]
fn proposed_writes_have_exact_widths_offsets_masks_and_preserve_reserved_bits() {
    // W1: PCI command register, 16 bits at offset 4; the Status register (offset 6) is never covered.
    assert_eq!((W1_OFFSET, W1_WIDTH, BUS_MASTER), (0x04, 16, 0x0004));
    assert!(W1_OFFSET + u32::from(W1_WIDTH) / 8 <= 0x06, "RW1C status bits are untouched");
    let mut cleared_only_bit2 = 0u32;
    for old in 0..=u16::MAX {
        let new = old & !BUS_MASTER;
        assert!(old ^ new == 0 || old ^ new == BUS_MASTER, "{old:#x}: only bit 2 may change, reserved bits are kept");
        assert_eq!(expected_command(old, true), new);
        assert_eq!(expected_command(old, false), old);
        let o = Observed { command: old, ..idle() };
        // Only the exact eligible value passes the policy.
        assert_eq!(refusals(&o, &ctx()) & PCI_COMMAND == 0, old == COMMAND, "{old:#x}");
        cleared_only_bit2 += u32::from(old ^ new == BUS_MASTER);
    }
    assert_eq!(cleared_only_bit2, 1 << 15);
    // W2: USBCMD, 32 bits, HCRST only, from a register that must read 0.
    assert_eq!((W2_WIDTH, HCRST, IDLE_USBSTS), (32, 0x2, 0x1));
    assert_eq!(HCRST.count_ones(), 1);
    for old in [0u32, 1, 2, 4, 8, 0x80, 0x100, 0xffff_ffff] {
        let o = Observed { usbcmd: old, ..idle() };
        assert_eq!(refusals(&o, &ctx()) & USBCMD == 0, old == 0, "USBCMD {old:#x}");
    }
    let (r, ..) = run(steady(idle()), &Context { operational: 0x40, ..ctx() });
    assert_eq!(r.entries()[3].new, idle().usbcmd | HCRST);
    assert_eq!(r.entries()[3].offset, 0x40, "USBCMD is the first operational register");
    // The reset poll watches USBSTS (operational + 4) for CNR (bit 11).
    assert_eq!((r.entries()[4].offset, r.entries()[4].new), (0x44, 0x800));
}

#[test]
fn trace_is_data_only_and_every_write_is_marked_not_issued() {
    let (r, ..) = run(steady(idle()), &ctx());
    for e in r.entries() {
        if matches!(e.kind, K_PCI_WRITE | K_MMIO_WRITE) {
            assert_eq!(e.result, R_NOT_ISSUED, "{e:?}");
        }
        assert!(matches!(e.result, R_OK | R_NOT_ISSUED | R_NOT_RUN), "{e:?}");
    }
    assert_eq!(r.entries().iter().filter(|e| matches!(e.kind, K_PCI_WRITE | K_MMIO_WRITE)).count(), 2);
    assert_eq!(r.trace.len(), TRACE_LIMIT);
    // A refused rehearsal proposes nothing.
    let (r, ..) = run(steady(Observed { usbcmd: 1, ..idle() }), &ctx());
    assert!(r.entries().is_empty() && r.plan_len == 0);
}

#[test]
fn d1_mask_is_omitted_only_when_interrupts_are_verifiably_disabled() {
    let facts = |o: &Observed| Facts {
        qualified: true,
        residue_complete: o.findings == 0,
        halted: o.usbsts & 1 != 0,
        run_stop: o.usbcmd & 1 != 0,
        bus_master: o.command & BUS_MASTER != 0,
        controller_ready: o.usbsts & (1 << 11) == 0,
        interrupter_enables: o.interrupter_enables,
        interrupts_disabled: o.interrupts_disabled(),
        legacy_present: false,
        legacy_os_owned: false,
    };
    let f = facts(&idle());
    assert!(f.interrupts_disabled && f.interrupter_enables == 1);
    let plan = lifecycle::plan(&f).unwrap();
    assert_eq!(plan.steps(), [Step::AwaitHalted, Step::DisableMaster, Step::Reset, Step::AwaitResetReady, Step::VerifyCleared]);
    assert_eq!(lifecycle::validate(plan.steps(), &f), Ok(()));
    // A gratuitous mask is not a needed write.
    let with_mask = [Step::MaskInterrupter(0), Step::AwaitHalted, Step::DisableMaster, Step::Reset, Step::AwaitResetReady,
                     Step::VerifyCleared];
    assert_eq!(lifecycle::validate(&with_mask, &f), Err(Violation::Unneeded));
    // Every way interrupts could be enabled keeps the original requirement.
    type Edit = fn(&mut Observed);
    let enabled: Vec<(&str, Edit)> = vec![
        ("INTE", |o| o.usbcmd = 4),
        ("EINT", |o| o.usbsts = 9),
        ("IP", |o| o.ip_pending = 1),
        ("MSI-X", |o| o.msix_control = 0x800f),
        ("MSI", |o| o.msi_control = 1),
        ("INTx", |o| o.status = 0x18),
    ];
    for (what, edit) in enabled {
        let mut o = idle();
        edit(&mut o);
        assert!(!o.interrupts_disabled(), "{what}");
        let f = facts(&o);
        let plan = lifecycle::plan(&f).unwrap_or_else(|e| panic!("{what}: {e:?}"));
        assert_eq!(plan.steps().first(), Some(&Step::MaskInterrupter(0)), "{what}: the mask stays required");
        assert_eq!(lifecycle::validate(&plan.steps()[1..], &f), Err(Violation::MissingMask), "{what}");
    }
    // Other controller states are unchanged: a running controller still needs mask and halt.
    let running = Facts { halted: false, run_stop: true, interrupts_disabled: false, ..facts(&idle()) };
    let plan = lifecycle::plan(&running).unwrap();
    assert_eq!(&plan.steps()[..3], [Step::MaskInterrupter(0), Step::Halt, Step::AwaitHalted]);
}

fn after(command: u16, rest: impl Fn(&mut Observed)) -> Observed {
    let mut o = idle();
    o.command = command;
    rest(&mut o);
    o
}

#[test]
fn bus_master_failure_model() {
    let before = idle();
    assert_eq!(bus_master_outcome(&before, &after(0x0003, |_| {})), State::BusMasterOff);
    assert_eq!(bus_master_outcome(&before, &before), State::BusMasterStuck, "bit 2 still set");
    assert_eq!(bus_master_outcome(&before, &after(0x0007, |o| o.usbsts = 5)), State::BusMasterStuck);
    // Anything besides bit 2 changing is an integrity fault, never a success.
    assert_eq!(bus_master_outcome(&before, &after(0x0001, |_| {})), State::IntegrityFault);
    assert_eq!(bus_master_outcome(&before, &after(0x0003, |o| o.usbsts = 5)), State::IntegrityFault, "HSE after the clear");
    assert_eq!(bus_master_outcome(&before, &after(0x0003, |o| o.dcbaap = 0)), State::IntegrityFault);
    assert_eq!(bus_master_outcome(&before, &after(0x0003, |o| o.memory ^= 1)), State::IntegrityFault);
    assert_eq!(after_revalidation_failure(), State::ContainedBmeOnly);
}

#[test]
fn reset_completion_and_timeout_model() {
    let done = Reset { polls: 3, hcrst_clear: true, cnr_clear: true, usbcmd: 0, usbsts: 1, crcr: 0, dcbaap: 0,
                       interrupters_clear: true };
    assert_eq!(reset_outcome(&done), State::Contained);
    assert_eq!(reset_outcome(&Reset { polls: POLL_BOUND, ..done }), State::Contained, "the bound itself is allowed");
    assert_eq!(reset_outcome(&Reset { polls: POLL_BOUND + 1, ..done }), State::ResetStuck);
    assert_eq!(reset_outcome(&Reset { hcrst_clear: false, ..done }), State::ResetStuck);
    assert_eq!(reset_outcome(&Reset { cnr_clear: false, ..done }), State::ResetStuck);
    // Completed but not verified: every DMA-relevant register must read as reset.
    for bad in [
        Reset { usbcmd: 1, ..done }, Reset { usbcmd: 4, ..done }, Reset { usbsts: 0, ..done },
        Reset { usbsts: 9, ..done }, Reset { crcr: 0x6756040, ..done }, Reset { crcr: 8, ..done },
        Reset { dcbaap: 0x6756000, ..done }, Reset { interrupters_clear: false, ..done },
    ] {
        assert_eq!(reset_outcome(&bad), State::ResetUnverified, "{bad:?}");
    }
    // CRCR's reserved/low bits other than CRR do not matter when the pointer is zero.
    assert_eq!(reset_outcome(&Reset { crcr: 0x1, ..done }), State::Contained);
}

#[test]
fn fingerprint_tracks_the_fields_it_summarizes() {
    let base = idle().fingerprint();
    assert_eq!(base, idle().fingerprint());
    for (what, edit) in [
        ("command", (|o: &mut Observed| o.command = 3) as fn(&mut Observed)),
        ("usbsts", |o| o.usbsts = 9),
        ("dcbaap", |o| o.dcbaap += 0x1000),
        ("memory", |o| o.memory ^= 1),
        ("bar", |o| o.bar += 0x4000),
    ] {
        let mut o = idle();
        edit(&mut o);
        assert_ne!(o.fingerprint(), base, "{what}");
    }
}
